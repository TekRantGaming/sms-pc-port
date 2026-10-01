#!/usr/bin/env python3
"""Check that the game and the code mods agree on bool and whole-word results
and arguments where they call each other by name.

  abi_check.py SMS_BINARY MODS_LIBRARY

The mods are compiled against SunshineHeaderInterface's declarations of the
game's functions and classes, the game against the decomp's, and a C++ symbol
does not carry its return type: a function SunshineHeaderInterface declares
bool and the decomp BOOL (or int, u32...) links and runs. On the PowerPC a bool
fills r3 with 0 or 1, so it does not matter there; on x86 a bool result is the
low byte alone (setcc %al), the rest of the register holding whatever it held,
and a caller that tests the whole word reads that too (Dark Zhine's nerves,
which SunshineHeaderInterface declares bool and the game's TSpineBase tests as
a word). Calls through the code-mod registry (SMS_PATCH_B/BL) are covered by
the mods' shim, which gives their bool results and arguments a whole word
(platform/mods/eclipse/shim/Kuribo/sdk/kuribo_sdk.h); this covers the rest:

- a game function the mods call or take the address of (a symbol their
  library leaves undefined): its result and arguments as each side declares it;
- a game virtual function a mod class overrides: the override against the
  game's declaration in the nearest game class it derives from.

Virtual calls from the mods to the game are not seen (they name no symbol).

It reads the declarations from the binary's DWARF (the game's from g++'s
units, the mods' from clang's; the mods are built with -fstandalone-debug so
that every class they use is described) and fails with the list of the
functions where one side says bool and the other a wider integer. Fix those in
platform/mods/eclipse/fixup_sources.py (SHI_WORD_RET). Without llvm-dwarfdump
it says so and passes.
"""
import re
import shutil
import subprocess
import sys

TAG = re.compile(r"^0x([0-9a-f]+):(\s+)(DW_TAG_\w+|NULL)")
ATTR = re.compile(r"^\s+(DW_AT_\w+)\s+\((.*)\)\s*$")
REF = re.compile(r'^0x([0-9a-f]+)(?: "(.*)")?$')
TYPES = {"DW_TAG_base_type", "DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type",
         "DW_TAG_enumeration_type"}
SCOPES = {"DW_TAG_class_type", "DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_namespace"}


def read(binary, tool):
    """-> (types, {"gcc": decls, "clang": decls}); decls: key -> declaration."""
    p = subprocess.Popen([tool, "--debug-info", binary], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         text=True, errors="replace", bufsize=1 << 20)
    types = {}  # offset -> [tag, name, byte_size, encoding, referenced type offset]
    decls = {"gcc": {}, "clang": {}}
    bases = {}  # clang class -> its base classes (names without scope)
    side = "gcc"
    stack = []  # open DIEs: (depth, die)
    die = None

    def close(d):
        if d["tag"] != "DW_TAG_subprogram" or d.get("spec"):
            return
        key = d.get("link")
        top = not any(s["tag"] in SCOPES for _, s in stack if s is not d)
        if not key and top and d.get("name"):
            key = d["name"]  # a C function
        if not key:
            return
        scopes = [s.get("name", "?") for _, s in stack if s["tag"] in SCOPES and s is not d]
        scope = "::".join(scopes)
        e = decls[side].setdefault(key, {"name": (scope + "::" if scope else "") + d.get("name", "?"),
                                         "method": d.get("name"), "cls": scopes[-1] if scopes else None,
                                         "ret": d.get("type"), "params": d["params"], "virt": False})
        e["virt"] = e["virt"] or d.get("virt", False)
        if not e["params"] and d["params"]:
            e["params"] = d["params"]

    for line in p.stdout:
        m = TAG.match(line)
        if m:
            depth = len(m.group(2))
            while stack and stack[-1][0] >= depth:
                close(stack.pop()[1])
            if m.group(3) == "NULL":
                die = None
                continue
            die = {"tag": m.group(3), "off": int(m.group(1), 16), "params": []}
            if die["tag"] == "DW_TAG_formal_parameter" and stack and stack[-1][1]["tag"] == "DW_TAG_subprogram":
                die["owner"] = stack[-1][1]
            elif die["tag"] in TYPES:
                types[die["off"]] = die
            elif die["tag"] == "DW_TAG_inheritance" and side == "clang" and stack and stack[-1][1]["tag"] in SCOPES:
                die["derived"] = stack[-1][1]
            stack.append((depth, die))
            continue
        if die is None:
            continue
        m = ATTR.match(line)
        if not m:
            continue
        a, v = m.groups()
        if a == "DW_AT_producer":
            side = "clang" if "clang" in v else "gcc"
        elif a in ("DW_AT_linkage_name", "DW_AT_MIPS_linkage_name"):
            die["link"] = v.strip('"')
        elif a == "DW_AT_name":
            die["name"] = v.strip('"')
        elif a == "DW_AT_type":
            r = REF.match(v)
            if r:
                die["type"] = int(r.group(1), 16)
                if "derived" in die and die["derived"].get("name") and r.group(2):
                    bases.setdefault(die["derived"]["name"], set()).add(r.group(2).split("::")[-1])
                if "owner" in die and not die.get("artificial"):
                    die["owner"]["params"].append(die["type"])
        elif a == "DW_AT_artificial" and "owner" in die and die["owner"]["params"] and die.get("type") == die["owner"]["params"][-1]:
            die["owner"]["params"].pop()  # `this`
        elif a == "DW_AT_artificial":
            die["artificial"] = True
        elif a in ("DW_AT_specification", "DW_AT_abstract_origin"):
            die["spec"] = True
        elif a == "DW_AT_virtuality":
            die["virt"] = True
        elif a == "DW_AT_byte_size":
            die["size"] = int(v, 0)
        elif a == "DW_AT_encoding":
            die["enc"] = v
    while stack:
        close(stack.pop()[1])
    if p.wait() != 0:
        sys.exit("abi_check: %s failed on %s" % (tool, binary))
    return types, decls, bases


def kind(types, off):
    """'bool', 'word' (an integer or enum of 2 bytes or more), or None."""
    seen = 0
    while off is not None and seen < 16:
        t = types.get(off)
        if t is None:
            return None
        if t["tag"] == "DW_TAG_enumeration_type":
            return "word"
        if t["tag"] == "DW_TAG_base_type":
            if "boolean" in t.get("enc", ""):
                return "bool"
            if t.get("size", 0) >= 2 and ("signed" in t.get("enc", "") or "unsigned" in t.get("enc", "")):
                return "word"
            return None
        off = t.get("type")
        seen += 1
    return None


def compare(types, g, c, what):
    out = []
    gk, ck = kind(types, g["ret"]), kind(types, c["ret"])
    if gk and ck and gk != ck:
        out.append("%s %s returns %s in the game, %s in the mods" % (what, c["name"], gk, ck))
    if len(g["params"]) == len(c["params"]):
        for i, (x, y) in enumerate(zip(g["params"], c["params"])):
            xk, yk = kind(types, x), kind(types, y)
            if xk and yk and xk != yk:
                out.append("%s %s: argument %d is %s in the game, %s in the mods" % (what, c["name"], i + 1, xk, yk))
    return out


def referenced(lib):
    """The symbols the mods' library uses from outside it (nm -u)."""
    nm = shutil.which("nm") or shutil.which("llvm-nm")
    out = subprocess.run([nm, "-u", lib], capture_output=True, text=True).stdout if nm else ""
    return set(line.split()[-1] for line in out.splitlines() if line.strip() and not line.endswith(":"))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    binary, modlib = sys.argv[1:]
    tool = shutil.which("llvm-dwarfdump") or next(
        (shutil.which("llvm-dwarfdump-%d" % v) for v in range(30, 13, -1) if shutil.which("llvm-dwarfdump-%d" % v)), None)
    if not tool:
        print("abi_check: no llvm-dwarfdump, not checked")
        return
    used = referenced(modlib)
    types, decls, bases = read(binary, tool)
    g, c = decls["gcc"], decls["clang"]
    bad = []
    # Game functions the mods call or take the address of, by symbol.
    calls = sorted(k for k in set(g) & set(c) if k in used)
    for key in calls:
        bad += compare(types, g[key], c[key], "function")
    # Game virtual functions the mods' own classes override: the declaration
    # in the nearest game class each derives from (the override's symbol is
    # the mod class's, and SunshineHeaderInterface may even give the base
    # other argument types).
    gclasses = set(e["cls"] for e in g.values() if e["cls"])
    gvirt = {}
    for e in g.values():
        if e["virt"] and e["cls"]:
            gvirt.setdefault((e["cls"], e["method"]), []).append(e)
    overrides = 0
    for key, e in sorted(c.items()):
        if not e["virt"] or not e["cls"] or e["cls"] in gclasses:
            continue
        todo, seen = list(bases.get(e["cls"], ())), set()
        while todo:
            b = todo.pop(0)
            if b in seen:
                continue
            seen.add(b)
            ge = [x for x in gvirt.get((b, e["method"]), []) if len(x["params"]) == len(e["params"])]
            if ge:
                overrides += 1
                bad += compare(types, ge[0], e, "override")
                break
            todo += sorted(bases.get(b, ()))
    if not calls and not overrides:
        print("abi_check: no declarations to compare in %s (no DWARF from both compilers), not checked" % binary)
        return
    what = "%d game functions the mods call and %d overrides" % (len(calls), overrides)
    if bad:
        print("abi_check: %d mismatches between the game and the mods (%s):" % (len(bad), what))
        for b in bad:
            print("  " + b)
        print("Declare the mods' side as the game does (platform/mods/eclipse/fixup_sources.py, SHI_WORD_RET).")
        sys.exit(1)
    print("abi_check: the game and the mods agree on bool and word results and arguments (%s)" % what)


if __name__ == "__main__":
    main()
