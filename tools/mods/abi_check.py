#!/usr/bin/env python3
"""Check that the game and the code mods agree on the integer and float types
of results and arguments wherever one calls the other.

  abi_check.py [-v] SMS_BINARY MODS_LIBRARY

The mods are compiled against SunshineHeaderInterface's declarations of the
game's functions and classes, the game against the decomp's, and a C++ symbol
does not carry its return type: a function SunshineHeaderInterface declares
u8 and the decomp BOOL links and runs. On the PowerPC that mostly does not
matter: a callee extends a bool, u8, s8, u16 or s16 result to the whole of r3
(clrlwi, extsh), callers extend arguments the same way, and floats of either
width are doubles in the FPRs. On x86 the callee leaves the bits above a
narrow result undefined (a bool is the low byte alone, setcc %al) and each
caller extends what it reads itself; clang trusts a narrow argument to be
extended to 32 bits on x86-64 but re-extends it on i386; and an f32 and an
f64 are different bits in xmm0. So natively the reading side sees garbage, or
another value than the console's, wherever the two sides disagree:

- a result: the reader must not read more bits than the callee defines, and
  where the reader is a mod (which, built by clang for the PowerPC, took r3 as
  already extended for its own type) it must read the type the game returns;
- an argument: every value of the passer's type must be one of the receiver's
  (a u8 into a u16, s16 or word; a word into a u8 is not);
- floats: f32 against f64, or a float against an integer, is a mismatch.

It compares, from the binary's DWARF (the game's declarations from g++'s
units, the mods' from clang's; the mods are built with -fstandalone-debug so
that every class they use is described):

- every game function the mods call or take the address of (a symbol their
  library leaves undefined), and every game virtual function the mods could
  call through a game object (SunshineHeaderInterface's declaration of the
  same symbol): the game is the callee;
- every game virtual function a mod class overrides, against the declaration
  in the nearest game class it derives from: the mod is the callee;
- every patch target (SMS_PATCH_B/BL, registered through the shim's
  __sms_mod_word_abi, kuribo_sdk.h): the mod function's type, from that
  template's instantiation, against the type the port's hook at the patched
  address calls it with (decomp-patches/*). The shim gives every bool or
  narrow integer result of a patch target a whole word, extended as the
  PowerPC callee extends it, and reads bool arguments from the low byte, so
  only arguments and float/integer results are compared there.

It fails with the list of mismatches. Fix the mods' side in
platform/mods/eclipse/fixup_sources.py (declare it as the game does, or as
retail's code reads it) or the port's hook. Without llvm-dwarfdump it says so
and passes. -v also lists the patch sites it could not compare.
"""
import glob
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TAG = re.compile(r"^0x([0-9a-f]+):(\s+)(DW_TAG_\w+|NULL)")
ATTR = re.compile(r"^\s+(DW_AT_\w+)\s+\((.*)\)\s*$")
REF = re.compile(r'^0x([0-9a-f]+)(?: "(.*)")?$')
TYPES = {"DW_TAG_base_type", "DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type",
         "DW_TAG_enumeration_type", "DW_TAG_pointer_type", "DW_TAG_reference_type",
         "DW_TAG_rvalue_reference_type", "DW_TAG_ptr_to_member_type", "DW_TAG_subroutine_type",
         "DW_TAG_class_type", "DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_array_type"}
SCOPES = {"DW_TAG_class_type", "DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_namespace"}
FUNCS = {"DW_TAG_subprogram", "DW_TAG_subroutine_type"}
SHIM = re.compile(r"^__sms_mod_word_abi<\(lambda at (.+):(\d+):\d+\), ")


def read(binary, tool):
    """-> (types, {"gcc": decls, "clang": decls}, bases, patch targets)."""
    p = subprocess.Popen([tool, "--debug-info", binary], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         text=True, errors="replace", bufsize=1 << 20)
    types = {}  # offset -> DIE: tag, name, size, enc, type (the type it refers to), params
    decls = {"gcc": {}, "clang": {}}
    bases = {}  # clang class -> its base classes (names without scope)
    shims = {}  # (mod source file, line) -> offset of the patch target's function pointer type
    side = "gcc"
    stack = []  # open DIEs: (depth, die)
    die = None

    def close(d):
        if d.get("shim") and d.get("F") is not None:
            shims[d["shim"]] = d["F"]
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
            parent = stack[-1][1] if stack else None
            if die["tag"] == "DW_TAG_formal_parameter" and parent and parent["tag"] in FUNCS:
                die["owner"] = parent
            elif die["tag"] == "DW_TAG_template_type_parameter" and parent and parent.get("shim"):
                die["tparam_of"] = parent
            elif die["tag"] == "DW_TAG_inheritance" and side == "clang" and parent and parent["tag"] in SCOPES:
                die["derived"] = parent
            if die["tag"] in TYPES:
                types[die["off"]] = die
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
            if side == "clang" and die["tag"] == "DW_TAG_structure_type":
                s = SHIM.match(die["name"])
                if s:
                    die["shim"] = (s.group(1), int(s.group(2)))
            elif die["name"] == "F" and "tparam_of" in die:
                die["tparam_of"]["F"] = die.get("type")
        elif a == "DW_AT_type":
            r = REF.match(v)
            if r:
                die["type"] = int(r.group(1), 16)
                if "derived" in die and die["derived"].get("name") and r.group(2):
                    bases.setdefault(die["derived"]["name"], set()).add(r.group(2).split("::")[-1])
                if "owner" in die and not die.get("artificial"):
                    die["owner"]["params"].append(die["type"])
        elif a == "DW_AT_artificial" and "owner" in die and die["owner"]["tag"] == "DW_TAG_subroutine_type":
            pass  # a member function pointer's `this`: an argument like the others
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
        elif a == "DW_AT_declaration":
            die["decl"] = True
    while stack:
        close(stack.pop()[1])
    if p.wait() != 0:
        sys.exit("abi_check: %s failed on %s" % (tool, binary))
    return types, decls, bases, shims


# A value's class: ("bool",), ("int", bytes, signed) with bytes 1, 2 or 4 (a
# word: what a PowerPC register holds), ("float", bytes), ("ptr",) for
# pointers, references and aggregates (an address in a register on the
# PowerPC), ("void",), or None when not known.
WORD = 4


def kind(types, off, seen=0):
    if off is None:
        return ("void",)
    t = types.get(off)
    if t is None or seen > 16:
        return None
    tag = t["tag"]
    if tag == "DW_TAG_base_type":
        enc, size = t.get("enc", ""), t.get("size", 0)
        if "boolean" in enc:
            return ("bool",)
        if "float" in enc:
            return ("float", size)
        if "signed" in enc:  # signed, unsigned, signed_char, unsigned_char
            return ("int", min(size, WORD), "unsigned" not in enc)
        return None
    if tag == "DW_TAG_enumeration_type":
        if t.get("type") is not None:
            u = kind(types, t["type"], seen + 1)
            if u:
                return u
        return ("int", min(t.get("size", WORD), WORD), False)
    if tag in ("DW_TAG_pointer_type", "DW_TAG_reference_type", "DW_TAG_rvalue_reference_type",
               "DW_TAG_ptr_to_member_type", "DW_TAG_class_type", "DW_TAG_structure_type",
               "DW_TAG_union_type", "DW_TAG_array_type"):
        return ("ptr",)
    if tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
        return kind(types, t.get("type"), seen + 1)
    return None


def name(k):
    if k is None:
        return "?"
    if k[0] in ("bool", "void", "ptr"):
        return {"ptr": "pointer"}.get(k[0], k[0])
    if k[0] == "float":
        return "f%d" % (k[1] * 8)
    if k[1] >= WORD:
        return "word"
    return "%s%d" % ("s" if k[2] else "u", k[1] * 8)


def within(a, b):
    """Every value of integer class a is one of b (passed extended to a word)."""
    if a[0] == "bool":
        return b[0] in ("bool", "int")
    if b[0] != "int":
        return False
    if b[1] >= WORD:
        return True
    if a[1] >= WORD:
        return False
    if a[2] == b[2]:
        return a[1] <= b[1]
    return not a[2] and b[2] and a[1] < b[1]


def floaty(a, b):
    """A float against an integer, or f32 against f64."""
    fa, fb = a[0] == "float", b[0] == "float"
    return (fa != fb and "void" not in (a[0], b[0]) and "ptr" not in (a[0], b[0])) or (fa and fb and a[1] != b[1])


def scalar(k):
    return k is not None and k[0] in ("bool", "int", "float")


def result_ok(callee, reader, reader_is_mod):
    """Can `reader` read the result `callee` returns as the console does?"""
    if not (scalar(callee) and scalar(reader)):
        return True
    if floaty(callee, reader):
        return False
    if callee[0] == "float":
        return True
    if reader[0] == "bool":
        return callee[0] == "bool"  # natively a bool reader trusts the byte to be 0 or 1
    if reader_is_mod:
        # Built for the PowerPC, the mod took r3 as already extended for its
        # own type: it must read what the game returns (a bool may be read as
        # a byte: 0 or 1 either way).
        if callee[0] == "bool":
            return reader[1] == 1
        return name(callee) == name(reader)
    # The game's code extends what it reads itself, from the bits the callee
    # defines: no more than the callee's width.
    width = 1 if callee[0] == "bool" else callee[1]
    return reader[1] <= width


def arg_ok(passer, receiver):
    if not (scalar(passer) and scalar(receiver)):
        return True
    if floaty(passer, receiver):
        return False
    if passer[0] == "float":
        return True
    return within(passer, receiver)


def compare(types, game, mod, game_is_callee, what, label):
    """A call between the game's declaration and the mods' of one function."""
    out = []
    gk, mk = kind(types, game["ret"]), kind(types, mod["ret"])
    if not (result_ok(gk, mk, True) if game_is_callee else result_ok(mk, gk, False)):
        out.append("%s %s returns %s in the game, %s in the mods" % (what, label, name(gk), name(mk)))
    if len(game["params"]) == len(mod["params"]):
        for i, (x, y) in enumerate(zip(game["params"], mod["params"])):
            xk, yk = kind(types, x), kind(types, y)
            if not (arg_ok(yk, xk) if game_is_callee else arg_ok(xk, yk)):
                out.append("%s %s: argument %d is %s in the game, %s in the mods" % (what, label, i + 1, name(xk), name(yk)))
    return out


def referenced(lib):
    """The symbols the mods' library uses from outside it (nm -u)."""
    nm = shutil.which("nm") or shutil.which("llvm-nm")
    out = subprocess.run([nm, "-u", lib], capture_output=True, text=True).stdout if nm else ""
    return set(line.split()[-1] for line in out.splitlines() if line.strip() and not line.endswith(":"))


# ---- patch targets ----------------------------------------------------------

def signature(types, off):
    """A function pointer type -> (result class, [argument classes]) or None."""
    t = types.get(off)
    seen = 0
    while t is not None and t["tag"] != "DW_TAG_subroutine_type" and seen < 8:
        t = types.get(t.get("type"))
        seen += 1
    if t is None:
        return None
    return kind(types, t.get("type")), [kind(types, p) for p in t["params"]]


PATCH_ADDR = re.compile(r"(?:PATCH_BL?|PatchBL?)\s*\(\s*(?:SMS_PORT_REGION\s*\(\s*)?(0x[0-9A-Fa-f]{8})")


def patch_address(path, line, cache={}):
    if path not in cache:
        try:
            cache[path] = open(path, encoding="utf-8", errors="replace").read().split("\n")
        except OSError:
            cache[path] = []
    text = "\n".join(cache[path][line - 1:line + 2])
    m = PATCH_ADDR.search(text)
    return int(m.group(1), 16) if m else None


# What the port's hooks spell types as, for the hand-written ones.
TEXT_TYPES = {
    "bool": ("bool",), "void": ("void",),
    "BOOL": ("int", 4, True), "int": ("int", 4, True), "s32": ("int", 4, True), "long": ("int", 4, True),
    "u32": ("int", 4, False), "unsigned int": ("int", 4, False), "unsigned long": ("int", 4, False),
    "size_t": ("int", 4, False), "uintptr_t": ("int", 4, False), "intptr_t": ("int", 4, True),
    "unsigned": ("int", 4, False),
    "s16": ("int", 2, True), "short": ("int", 2, True), "u16": ("int", 2, False), "unsigned short": ("int", 2, False),
    "s8": ("int", 1, True), "signed char": ("int", 1, True), "char": ("int", 1, True),
    "u8": ("int", 1, False), "unsigned char": ("int", 1, False),
    "f32": ("float", 4), "float": ("float", 4), "f64": ("float", 8), "double": ("float", 8),
}


def text_kind(t, named):
    t = re.sub(r"\b(const|volatile|struct|class|enum)\b", " ", t)
    t = " ".join(t.split())
    if not t:
        return None
    if "*" in t or "&" in t or "[" in t or "<" in t or t == "MtxPtr":
        return ("ptr",)
    if t in TEXT_TYPES:
        return TEXT_TYPES[t]
    return named.get(t.split("::")[-1])


def split_args(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "<([":
            depth += 1
        elif ch in ">)]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip() and cur.strip() != "void":
        out.append(cur)
    return [a.strip() for a in out]


GEN = re.compile(r"static_cast<sms_mod_r_ \((\*|[\w:]+::\*)\)\((.*?)\)( const)?>\(&([\w:]+)\)\)")
CAST = re.compile(r"\(\(\s*([\w:<>\s\*&]+?)\s*\(\*\)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*\)\s*"
                  r"(\w+|SMS_MOD_SITE\(0x[0-9A-Fa-f]{8}\))\s*\)")


def hook_sites():
    """retail address -> [(hook file, result text or None, [argument texts], game function name)].

    A hook calls the mod's function through a cast of what SMS_MOD_SITE
    gives: generated ones through the game function's type (GEN), the others
    through a cast of the variable holding it, of SMS_MOD_SITE itself, or
    inside a helper the variable is passed to."""
    sites = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "decomp-patches", "*.patch"))):
        lines = [l[1:] for l in open(path, encoding="utf-8", errors="replace").read().split("\n")
                 if l[:1] in ("+", " ") and not l.startswith("+++")]
        text = "\n".join(lines)
        base = os.path.basename(path)

        def add(addr, ret, args, fname=None):
            sites.setdefault(addr, []).append((base, ret, split_args(args) if isinstance(args, str) else args, fname))

        for m in re.finditer(r"(?:(\w+)\s*=\s*)?SMS_MOD_SITE\((0x[0-9A-Fa-f]{8})\)", text):
            addr, var = int(m.group(2), 16), m.group(1)
            rest = text[m.end():m.end() + 4000]
            if var:
                nxt = re.search(r"\b%s\s*=\s*SMS_MOD_SITE" % re.escape(var), rest)
                if nxt:
                    rest = rest[:nxt.start()]
            g = GEN.search(rest[:1500])
            c = next((c for c in CAST.finditer(rest) if c.group(3) == var), None) if var else None
            if g and (not c or g.start() < c.start()) and (not var or var == "sms_mod_t_"):
                args = split_args(g.group(2))
                if g.group(1) != "*":
                    args = ["%s*" % g.group(1)[:-3]] + args
                add(addr, None, args, g.group(4))
                continue
            if c:
                add(addr, c.group(1), c.group(2))
                continue
            t = re.search(r"\(\(__typeof__\(&([\w:]+)\)\)%s\)" % re.escape(var or "sms_mod_t_"), rest[:1500])
            if t:
                add(addr, None, None, t.group(1))
                continue
            # A cast of SMS_MOD_SITE(addr) itself, next to the test.
            d = re.compile(CAST.pattern.replace(r"(\w+|SMS_MOD_SITE\(0x[0-9A-Fa-f]{8}\))",
                                                r"(SMS_MOD_SITE\(%s\))" % re.escape(m.group(2)))).search(rest[:600])
            if not var and d:
                add(addr, d.group(1), d.group(2))
                continue
            if not var:
                continue
            # A variable cast elsewhere in the file (a static the call site sets).
            c = next((c for c in CAST.finditer(text) if c.group(3) == var), None)
            if c and var != "sms_mod_t_":
                add(addr, c.group(1), c.group(2))
                continue
            # The variable passed to a helper defined in the file, which casts it.
            h = re.search(r"\b(\w+)\s*\(([^;{}]*?\b%s\b[^;{}]*?)\)" % re.escape(var), rest[:600])
            if h:
                pos = h.group(2).count(",", 0, h.group(2).find(var))  # the argument's index
                df = re.search(r"\b%s\s*\(([^)]*)\)\s*\{" % re.escape(h.group(1)), text)
                if df:
                    params = split_args(df.group(1))
                    pname = re.findall(r"(\w+)\s*$", params[pos])[0] if pos < len(params) else None
                    body = text[df.end():df.end() + 2000]
                    c = next((c for c in CAST.finditer(body) if c.group(3) == pname), None)
                    if c:
                        add(addr, c.group(1), c.group(2))
        for m in re.finditer(r"SMS_MOD_CALLF(?:_R)?\((0x[0-9A-Fa-f]{8}),\s*([\w:]+)", text):
            add(int(m.group(1), 16), None, None, m.group(2))
        for m in re.finditer(r"SMS_MOD_CALLM(?:_R)?\((0x[0-9A-Fa-f]{8}),\s*([\w:]+),\s*(\w+)", text):
            add(int(m.group(1), 16), None, None, "this:" + m.group(2) + "::" + m.group(3))
        for m in re.finditer(r"SMS_MOD_CALL(?:_R)?\((0x[0-9A-Fa-f]{8}),\s*([\w:<>\s\*&]+?)\s*\(\*\)\s*\(([^()]*)\)", text):
            add(int(m.group(1), 16), m.group(2), m.group(3))
        for m in re.finditer(r"SMS_MOD_LOAD_PARTICLE\((0x[0-9A-Fa-f]{8}),", text):
            add(int(m.group(1), 16), "void", ["JPAResourceManager*", "const char*", "u16"])
        for m in re.finditer(r"SMS_MOD_MOVE_STAGE\((0x[0-9A-Fa-f]{8})\)", text):
            add(int(m.group(1), 16), "void", ["TMarDirector*"])
    return sites


def gpr_fpr(classes):
    """The PowerPC passes integers and pointers in r3..., floats in f1...: the
    n-th of each kind on one side meets the n-th on the other."""
    return ([k for k in classes if k is None or k[0] != "float"], [k for k in classes if k is not None and k[0] == "float"])


def check_patches(types, decls, shims, named, verbose):
    by_name = {}
    for e in decls["gcc"].values():
        by_name.setdefault(e["name"], []).append(e)
    sites = hook_sites()
    bad, compared, skipped = [], 0, []
    for (src, line), F in sorted(shims.items()):
        addr = patch_address(src, line)
        sig = signature(types, F)
        where = "%s:%d" % (os.path.relpath(src, os.path.dirname(os.path.dirname(src))), line)
        if sig is None:
            skipped.append("%s: a branch to retail code, not to a function" % where)
            continue
        if addr is None:
            skipped.append("%s: no literal address" % where)
            continue
        if addr not in sites:
            skipped.append("%s: no hook at 0x%08X" % (where, addr))
            continue
        mret, margs = sig
        for hook, rtext, atexts, fname in sites[addr]:
            if atexts is None:
                # A hook calling through the game function's own type.
                member = fname.startswith("this:")
                fname = fname[5:] if member else fname
                cands = by_name.get(fname, [])
                sigs = set((name(kind(types, e["ret"])), tuple(name(kind(types, x)) for x in e["params"])) for e in cands)
                if len(sigs) != 1:
                    skipped.append("%s: the game's %s is not one function" % (where, fname))
                    continue
                gargs = ([("ptr",)] if member else []) + [kind(types, x) for x in cands[0]["params"]]
                gret = kind(types, cands[0]["ret"])
            elif rtext is not None:
                # A cast spelling the whole type.
                gargs = [text_kind(a, named) for a in atexts if a != "..."]
                gret = text_kind(rtext, named) if rtext != "sms_mod_r_" else None
            else:
                # A generated hook: the game function's argument types, its
                # result that of the call it replaces.
                gargs = [text_kind(a, named) for a in atexts]
                member = atexts and fname.rsplit("::", 1)[0] + "*" == atexts[0]
                n = len(gargs) - (1 if member else 0)
                cands = [e for e in by_name.get(fname, []) if len(e["params"]) == n]
                rets = set(name(kind(types, e["ret"])) for e in cands)
                gret = kind(types, cands[0]["ret"]) if len(rets) == 1 else None
            compared += 1
            label = "0x%08X (%s, hook in %s)" % (addr, where, hook)
            # Results: the shim's thunk gives bool and narrow integers a whole
            # word; only floats against integers remain.
            if scalar(mret) and scalar(gret) and floaty(mret, gret):
                bad.append("patch %s: the mod returns %s, the game reads %s" % (label, name(mret), name(gret)))
            mg, mf = gpr_fpr(margs)
            gg, gf = gpr_fpr(gargs)
            for i, (x, y) in enumerate(zip(gg, mg)):
                if y is not None and y[0] == "bool":
                    continue  # the thunk reads the low byte
                if x is not None and y is not None and not arg_ok(x, y):
                    bad.append("patch %s: integer argument %d is %s in the game, %s in the mod" % (label, i + 1, name(x), name(y)))
            for i, (x, y) in enumerate(zip(gf, mf)):
                if x[1] != y[1]:
                    bad.append("patch %s: float argument %d is %s in the game, %s in the mod" % (label, i + 1, name(x), name(y)))
    if verbose:
        for s in skipped:
            print("abi_check: patch target not compared: " + s)
    return bad, compared, len(skipped)


def main():
    args = sys.argv[1:]
    verbose = "-v" in args
    args = [a for a in args if a != "-v"]
    if len(args) != 2:
        sys.exit(__doc__)
    binary, modlib = args
    tool = shutil.which("llvm-dwarfdump") or next(
        (shutil.which("llvm-dwarfdump-%d" % v) for v in range(30, 13, -1) if shutil.which("llvm-dwarfdump-%d" % v)), None)
    if not tool:
        print("abi_check: no llvm-dwarfdump, not checked")
        return
    used = referenced(modlib)
    types, decls, bases, shims = read(binary, tool)
    g, c = decls["gcc"], decls["clang"]
    bad = []
    # Game functions the mods call or take the address of, by symbol, and game
    # virtual functions the mods may call through a game object (by the same
    # symbol: SunshineHeaderInterface declares the same signature).
    calls = sorted(k for k in set(g) & set(c) if k in used)
    for key in calls:
        bad += compare(types, g[key], c[key], True, "function", c[key]["name"])
    vcalls = sorted(k for k in set(g) & set(c) if k not in used and g[k]["virt"])
    for key in vcalls:
        bad += compare(types, g[key], c[key], True, "virtual function", c[key]["name"])
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
                bad += compare(types, ge[0], e, False, "override", e["name"])
                break
            todo += sorted(bases.get(b, ()))
    if not calls and not overrides:
        print("abi_check: no declarations to compare in %s (no DWARF from both compilers), not checked" % binary)
        return
    # The game's typedef and enum names, for the types the hooks spell.
    named = {}
    for t in types.values():
        if t["tag"] in ("DW_TAG_typedef", "DW_TAG_enumeration_type") and t.get("name") and not t.get("decl"):
            k = kind(types, t["off"])
            if k and t["name"] not in named:
                named[t["name"]] = k
    pbad, patches, unchecked = check_patches(types, decls, shims, named, verbose)
    bad += pbad
    what = "%d game functions the mods call, %d game virtual functions, %d overrides, %d patch sites (%d not compared)" % (
        len(calls), len(vcalls), overrides, patches, unchecked)
    if bad:
        print("abi_check: %d mismatches between the game and the mods (%s):" % (len(bad), what))
        for b in bad:
            print("  " + b)
        print("Declare the mods' side as the game does (platform/mods/eclipse/fixup_sources.py), or fix the port's hook.")
        sys.exit(1)
    print("abi_check: the game and the mods agree on integer and float results and arguments (%s)" % what)


if __name__ == "__main__":
    main()
