#!/usr/bin/env python3
"""Check that the platform layer and the game agree on the integer widths of
the functions one implements for the other.

  abi_platform.py [-v] SMS_BINARY

The game is compiled against the decomp's declarations (the SDK's headers,
JSystem's, the game's own), written for MWCC, whose long is 4 bytes; the port
spells u32 and s32 int on 64-bit hosts (src/port_include/dolphin/types.h) so
they keep that width. platform/ implements the SDK (OS, CARD, DVD, VI, PAD,
AI/AR/DSP, GX, MTX...) and the port's own services for the game, compiled by
the same compiler but not always against the same declaration. A C symbol
carries no types, so a definition reading a 64-bit long where the game
passes a 32-bit word links and runs; on x86-64 it even works, because every
32-bit register write zeroes the upper half, but a sign-extended negative
value, a value computed in a 64-bit register, another compiler or an ARM64
host breaks it. A long on either side is 8 bytes on an LP64 host and 4 under
MWCC (and on Windows), so the console's 32-bit interface has no long in it.

From the binary's DWARF (g++'s units, platform and game), for every function
the platform defines and the game declares (calls), and every function the
game defines and the platform declares, it compares the result and each
argument:

- the class (integer, bool, float, pointer, aggregate) and the integer width
  as the host compiles it, and the signedness of narrow integers;
- a C long (8 bytes) on either side, where the console's is 4;
- through pointers: the integer or float a pointer points at, the members of
  a struct it points at (offset, width), and the signature of a callback (a
  pointer to a function), recursively.

It fails with the list of mismatches. Fix the platform's definition to the
game's types (u32, s32, int; a pointer-sized type only where the game passes
a pointer), and a long in the game's declaration with a decomp-patch spelling
it s32/u32 (the same type under MWCC). On a 32-bit host long is 4 bytes and
only the other checks apply. Without llvm-dwarfdump it says so and passes.
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLATFORM_DIRS = tuple(os.path.join(ROOT, d) + os.sep for d in ("platform", "src"))
GAME_DIR = os.path.join(ROOT, "decomp") + os.sep
TAG = re.compile(r"^0x([0-9a-f]+):(\s+)(DW_TAG_\w+|NULL)")
ATTR = re.compile(r"^\s+(DW_AT_\w+)\s+\((.*)\)\s*$")
REF = re.compile(r'^0x([0-9a-f]+)')
TYPES = {"DW_TAG_base_type", "DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type",
         "DW_TAG_restrict_type", "DW_TAG_enumeration_type", "DW_TAG_pointer_type", "DW_TAG_reference_type",
         "DW_TAG_rvalue_reference_type", "DW_TAG_ptr_to_member_type", "DW_TAG_subroutine_type",
         "DW_TAG_class_type", "DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_array_type"}
SCOPES = {"DW_TAG_class_type", "DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_namespace"}
FUNCS = {"DW_TAG_subprogram", "DW_TAG_subroutine_type"}
KEEP = TYPES | FUNCS | {"DW_TAG_formal_parameter", "DW_TAG_unspecified_parameters", "DW_TAG_member",
                        "DW_TAG_compile_unit", "DW_TAG_namespace"}
LONG_NAMES = {"long int", "long unsigned int", "long", "unsigned long"}
# Integers the game means to be as wide as a pointer (ARQCallback's argument).
POINTER_SIZED = {"uintptr_t", "intptr_t"}


# The game's operator new and delete are renamed in its library (JKR heaps);
# the platform's calls go to the host's.
HOST_SYMBOLS = ("_Znw", "_Zna", "_Zdl", "_Zda")


def side_of(path):
    """A compile unit's side: the port's own code, the game's (the decomp's
    sources, patched or mirrored copies of them), or neither (the mods)."""
    if path.startswith(PLATFORM_DIRS):
        return "platform"
    if path.startswith(GAME_DIR) or (path.startswith(ROOT + os.sep) and ("/patched/" in path or "/cp932/" in path)):
        return "game"
    return None


def read(binary, tool):
    """-> (types by offset, [subprograms])."""
    p = subprocess.Popen([tool, "--debug-info", binary], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         text=True, errors="replace", bufsize=1 << 20)
    types = {}
    subs = []
    params = {}  # offset -> formal parameter (a concrete one names its abstract one)
    cu = None
    stack = []  # (depth, die or None)
    die = None
    for line in p.stdout:
        if line[0] == "0":
            m = TAG.match(line)
            if not m:
                continue
            depth = len(m.group(2))
            while stack and stack[-1][0] >= depth:
                stack.pop()
            tag = m.group(3)
            if tag == "NULL" or tag not in KEEP:
                die = None
                stack.append((depth, None))
                continue
            die = {"tag": tag, "off": int(m.group(1), 16)}
            parent = stack[-1][1] if stack else None
            if tag == "DW_TAG_compile_unit":
                cu = die
                stack = []
            elif tag == "DW_TAG_formal_parameter":
                params[die["off"]] = die
                if parent is not None and parent["tag"] in FUNCS:
                    die["owner"] = parent
                    parent.setdefault("params", []).append(die)
            elif tag == "DW_TAG_unspecified_parameters":
                if parent is not None and parent["tag"] in FUNCS:
                    parent["varargs"] = True
            elif tag == "DW_TAG_member":
                if parent is not None and parent["tag"] in SCOPES:
                    parent.setdefault("members", []).append(die)
            if tag in TYPES:
                types[die["off"]] = die
            if tag == "DW_TAG_subprogram":
                die["cu"] = cu
                die["scope"] = [s.get("name", "?") for _, s in stack if s is not None and s["tag"] in SCOPES]
                subs.append(die)
            stack.append((depth, die))
            continue
        if die is None:
            continue
        m = ATTR.match(line)
        if not m:
            continue
        a, v = m.groups()
        if a == "DW_AT_name":
            die["name"] = v.strip('"')
        elif a in ("DW_AT_linkage_name", "DW_AT_MIPS_linkage_name"):
            die["link"] = v.strip('"')
        elif a == "DW_AT_type":
            r = REF.match(v)
            if r:
                die["type"] = int(r.group(1), 16)
        elif a == "DW_AT_byte_size":
            die["size"] = int(v, 0)
        elif a == "DW_AT_encoding":
            die["enc"] = v
        elif a == "DW_AT_declaration":
            die["decl"] = True
        elif a == "DW_AT_low_pc":
            die["pc"] = True
        elif a in ("DW_AT_specification", "DW_AT_abstract_origin"):
            r = REF.match(v)
            if r:
                die["origin"] = int(r.group(1), 16)
        elif a == "DW_AT_artificial":
            die["artificial"] = True
        elif a == "DW_AT_external":
            die["external"] = True
        elif a == "DW_AT_decl_file":
            die["file"] = v.strip('"')
        elif a == "DW_AT_data_member_location":
            try:
                die["at"] = int(v, 0)
            except ValueError:
                pass
        elif a == "DW_AT_producer" and die["tag"] == "DW_TAG_compile_unit":
            die["producer"] = v
        elif a == "DW_AT_comp_dir":
            pass
    if p.wait() != 0:
        sys.exit("abi_platform: %s failed on %s" % (tool, binary))
    for q in params.values():
        seen = 0
        o = q
        while "type" not in q and "origin" in o and o["origin"] in params and seen < 4:
            o = params[o["origin"]]
            seen += 1
            if "type" in o:
                q["type"] = o["type"]
            if o.get("artificial"):
                q["artificial"] = True
    return types, subs


# ---- types -------------------------------------------------------------------

def strip(types, off, seen=0):
    """Follow typedefs and qualifiers -> (DIE or None, the names on the way)."""
    names = []
    t = types.get(off)
    while t is not None and seen < 32 and t["tag"] in ("DW_TAG_typedef", "DW_TAG_const_type",
                                                       "DW_TAG_volatile_type", "DW_TAG_restrict_type"):
        if t.get("name"):
            names.append(t["name"])
        t = types.get(t.get("type"))
        seen += 1
    return t, names


def kind(types, off):
    """A value's class: ("void",), ("bool",), ("int", bytes, signed, is_long),
    ("float", bytes), ("ptr", pointee offset), ("ref", pointee offset),
    ("agg", DIE), or None."""
    if off is None:
        return ("void",)
    t, _ = strip(types, off)
    if t is None:
        return ("void",) if off not in types else None
    tag = t["tag"]
    if tag == "DW_TAG_base_type":
        enc, size = t.get("enc", ""), t.get("size", 0)
        if "boolean" in enc:
            return ("bool",)
        if "float" in enc:
            return ("float", size)
        if "signed" in enc:
            return ("int", size, "unsigned" not in enc, t.get("name") in LONG_NAMES and size == 8)
        return None
    if tag == "DW_TAG_enumeration_type":
        u = kind(types, t["type"]) if t.get("type") is not None else None
        return u or ("int", t.get("size", 4), False, False)
    if tag == "DW_TAG_pointer_type":
        return ("ptr", t.get("type"))
    if tag in ("DW_TAG_reference_type", "DW_TAG_rvalue_reference_type"):
        return ("ref", t.get("type"))
    if tag == "DW_TAG_ptr_to_member_type":
        return ("memptr",)
    if tag in ("DW_TAG_class_type", "DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_array_type"):
        return ("agg", t)
    if tag == "DW_TAG_subroutine_type":
        return ("func", t)
    return None


def spell(types, off):
    if off is None:
        return "void"
    t, names = strip(types, off)
    base = t.get("name") if t is not None else None
    if t is not None and t["tag"] == "DW_TAG_pointer_type":
        return spell(types, t.get("type")) + "*"
    if t is not None and t["tag"] in ("DW_TAG_reference_type", "DW_TAG_rvalue_reference_type"):
        return spell(types, t.get("type")) + "&"
    if t is not None and t["tag"] == "DW_TAG_subroutine_type":
        return "%s(%s)" % (spell(types, t.get("type")), ", ".join(spell(types, q.get("type")) for q in t.get("params", [])))
    if names and base and names[0] != base:
        return "%s (%s)" % (names[0], base)
    return names[0] if names else (base or "?")


def complete(types, t):
    """A struct's full definition, when this DIE is only a declaration."""
    if not t.get("decl") or not t.get("name"):
        return t
    return types.get(("defn", t["tag"], t["name"]), t)


def describe(k):
    if k is None:
        return "?"
    if k[0] == "int":
        return "%s%d%s" % ("s" if k[2] else "u", k[1] * 8, " (long)" if k[3] else "")
    if k[0] == "float":
        return "f%d" % (k[1] * 8)
    return {"ptr": "pointer", "ref": "reference", "agg": "aggregate", "func": "function", "memptr": "member pointer"}.get(k[0], k[0])


class Compare:
    def __init__(self, types, long_bytes):
        self.types = types
        self.long_bytes = long_bytes
        self.seen = set()

    def value(self, a, b, where, out, depth=0):
        """Platform type offset a against game type offset b."""
        ka, kb = kind(self.types, a), kind(self.types, b)
        if ka is None or kb is None:
            return
        sa, sb = spell(self.types, a), spell(self.types, b)
        if self.long_bytes == 8:
            for k, s, side, off in ((ka, sa, "platform", a), (kb, sb, "game", b)):
                if k[0] == "int" and k[3] and not POINTER_SIZED & set(strip(self.types, off)[1]):
                    out.append("%s: %s on the %s side: a long is 8 bytes on this host, 4 under MWCC (platform %s, game %s)"
                               % (where, s, side, sa, sb))
                    return
        if ka[0] != kb[0]:
            if {ka[0], kb[0]} <= {"ptr", "ref"}:
                pass
            elif {ka[0], kb[0]} == {"bool", "int"} and (ka[0] == "int" and ka[1] == 1 or kb[0] == "int" and kb[1] == 1):
                pass
            else:
                out.append("%s: platform %s, game %s (%s against %s)" % (where, sa, sb, describe(ka), describe(kb)))
                return
        if ka[0] == "int" and kb[0] == "int":
            if ka[1] != kb[1]:
                out.append("%s: platform %s, game %s (%s against %s)" % (where, sa, sb, describe(ka), describe(kb)))
            elif ka[1] < 4 and ka[2] != kb[2]:
                out.append("%s: platform %s, game %s (signedness)" % (where, sa, sb))
            return
        if ka[0] == "float" and ka[1] != kb[1]:
            out.append("%s: platform %s, game %s" % (where, sa, sb))
            return
        if depth > 6:
            return
        if ka[0] in ("ptr", "ref") and kb[0] in ("ptr", "ref"):
            pa, pb = kind(self.types, ka[1]), kind(self.types, kb[1])
            if ka[1] is None or kb[1] is None or pa is None or pb is None:
                return  # void* on either side
            if pa[0] in ("int", "float", "bool") and pb[0] in ("int", "float", "bool"):
                self.value(ka[1], kb[1], where + " (pointee)", out, depth + 1)
            elif pa[0] == "func" and pb[0] == "func":
                self.signature(pa[1], pb[1], where + " (callback)", out, depth + 1)
            elif pa[0] == "agg" and pb[0] == "agg":
                self.struct(pa[1], pb[1], where, out, depth + 1)
            elif pa[0] in ("ptr", "ref") and pb[0] in ("ptr", "ref"):
                self.value(ka[1], kb[1], where + " (pointee)", out, depth + 1)
            return
        if ka[0] == "agg" and kb[0] == "agg":
            self.struct(ka[1], kb[1], where, out, depth + 1)
        if ka[0] == "func" and kb[0] == "func":
            self.signature(ka[1], kb[1], where, out, depth + 1)

    def struct(self, a, b, where, out, depth):
        a, b = complete(self.types, a), complete(self.types, b)
        if a.get("decl") or b.get("decl") or a["tag"] == "DW_TAG_array_type":
            return
        key = (a["off"], b["off"])
        if key in self.seen:
            return
        self.seen.add(key)
        name = a.get("name") or b.get("name") or "?"
        if a.get("size") != b.get("size"):
            out.append("%s: struct %s is %s bytes on the platform side, %s in the game" % (where, name, a.get("size"), b.get("size")))
            return
        ma = [(m.get("name"), m.get("at")) for m in a.get("members", [])]
        mb = [(m.get("name"), m.get("at")) for m in b.get("members", [])]
        if [x[1] for x in ma] != [x[1] for x in mb]:
            out.append("%s: struct %s has members at other offsets on the platform side (%s) than in the game (%s)"
                       % (where, name, ma, mb))
            return
        for x, y in zip(a.get("members", []), b.get("members", [])):
            ka, kb = kind(self.types, x.get("type")), kind(self.types, y.get("type"))
            if ka and kb and ka[0] in ("int", "float") and kb[0] in ("int", "float"):
                self.value(x.get("type"), y.get("type"), "%s: %s::%s" % (where, name, y.get("name") or x.get("name")),
                           out, depth + 1)

    def signature(self, a, b, where, out, depth=0):
        """Two function DIEs (subprogram or subroutine type): platform a, game b."""
        self.value(a.get("type"), b.get("type"), where + " result", out, depth)
        pa = [q for q in a.get("params", []) if not q.get("artificial") or a["tag"] == "DW_TAG_subroutine_type"]
        pb = [q for q in b.get("params", []) if not q.get("artificial") or b["tag"] == "DW_TAG_subroutine_type"]
        if len(pa) != len(pb) or a.get("varargs") != b.get("varargs"):
            out.append("%s: %d arguments on the platform side, %d in the game" % (where, len(pa), len(pb)))
            return
        for i, (x, y) in enumerate(zip(pa, pb)):
            self.value(x.get("type"), y.get("type"), "%s argument %d" % (where, i + 1), out, depth)


def main():
    args = sys.argv[1:]
    verbose = "-v" in args
    args = [a for a in args if a != "-v"]
    if len(args) != 1:
        sys.exit(__doc__)
    binary = args[0]
    tool = shutil.which("llvm-dwarfdump") or next(
        (shutil.which("llvm-dwarfdump-%d" % v) for v in range(30, 13, -1) if shutil.which("llvm-dwarfdump-%d" % v)), None)
    if not tool:
        print("abi_platform: no llvm-dwarfdump, not checked")
        return
    types, subs = read(binary, tool)
    for t in list(types.values()):
        if t["tag"] in SCOPES and t.get("name") and not t.get("decl") and "size" in t:
            types.setdefault(("defn", t["tag"], t["name"]), t)
    by_off = {s["off"]: s for s in subs}
    long_bytes = 4
    for t in types.values():
        if t["tag"] == "DW_TAG_base_type" and t.get("name") == "long int":
            long_bytes = t.get("size", 4)
            break
    # Each subprogram's symbol, side, and whether it defines the function.
    defs, decls = {}, {}
    for s in subs:
        o = s
        seen = 0
        while "origin" in o and o["origin"] in by_off and seen < 4:
            o = by_off[o["origin"]]
            seen += 1
            for k in ("link", "name", "type", "external", "file", "varargs"):
                if k not in s and k in o:
                    s[k] = o[k]
            if "params" not in s and "params" in o:
                s["params"] = o["params"]
            if not s["scope"] and o["scope"]:
                s["scope"] = o["scope"]
        key = s.get("link") or (s.get("name") if s.get("external") and not s["scope"] else None)
        if not key or key.startswith(HOST_SYMBOLS):
            continue
        if s.get("decl") and not s.get("file", ROOT).startswith(ROOT + os.sep):
            continue  # a host library's declaration (<new>, libc)
        cu = s["cu"].get("name", "").strip('"') if s["cu"] else ""
        s["side"] = side_of(cu)
        s["cuname"] = cu
        if s["side"] is None:
            continue
        if s.get("pc"):
            defs.setdefault(key, []).append(s)
        elif s.get("decl"):
            decls.setdefault(key, []).append(s)
    cmp = Compare(types, long_bytes)
    bad = []
    pairs = 0
    funcs = set()
    for key in sorted(defs):
        for d in defs[key]:
            other = "game" if d["side"] == "platform" else "platform"
            sigs = {}
            for e in decls.get(key, []):
                if e["side"] != other:
                    continue
                sigs.setdefault((spell(types, e.get("type")),
                                 tuple(spell(types, q.get("type")) for q in e.get("params", []))), e)
            for e in sigs.values():
                pairs += 1
                funcs.add(key)
                label = "::".join(d["scope"] + [d.get("name", key)])
                if d["side"] == "platform":
                    where = "%s (defined in %s, declared for the game in %s)" % (
                        label, os.path.relpath(d["cuname"], ROOT), os.path.relpath(e.get("file", "?"), ROOT))
                    plat, game = d, e
                else:
                    where = "%s (defined by the game in %s, declared in %s for %s)" % (
                        label, os.path.relpath(d["cuname"], ROOT), os.path.relpath(e.get("file", "?"), ROOT),
                        os.path.relpath(e["cuname"], ROOT))
                    plat, game = e, d
                out = []
                cmp.signature(plat, game, where, out)
                for o in out:
                    if o not in bad:
                        bad.append(o)
    if not pairs:
        print("abi_platform: no declarations to compare in %s (no DWARF from both sides), not checked" % binary)
        return
    what = "%d functions one side defines and the other declares, %d declarations" % (len(funcs), pairs)
    if verbose:
        print("abi_platform: compared %s" % what)
    if bad:
        print("abi_platform: %d mismatches between the platform layer and the game (%s):" % (len(bad), what))
        for b in bad:
            print("  " + b)
        print("Define the platform's side with the game's 32-bit types (u32, s32, int), and spell a long in the "
              "game's declaration s32/u32 with a decomp-patch.")
        sys.exit(1)
    print("abi_platform: the platform layer and the game agree on result and argument types (%s)" % what)


if __name__ == "__main__":
    main()
