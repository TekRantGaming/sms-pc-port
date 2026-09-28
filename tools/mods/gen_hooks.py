#!/usr/bin/env python3
"""Generate code-mod call hooks in the decomp source (decomp-patches/zz-modhook-50-calls.patch).

A code mod's SMS_PATCH_BL(addr, fn) redirects the retail `bl` at addr. For
each such patch this finds, in the decomp source, the call it redirects and
rewrites it as SMS_MOD_CALLM/SMS_MOD_CALLF (src/port_include/sms_modhook.h):

- the retail function containing addr and the function it calls come from
  the decomp's disassembly (build/GMSE01/asm);
- the containing function's definition is looked up in its .cpp by name;
- a call is rewritten only when the source calls that function as many
  times as the retail code does, taking the n-th call for the n-th `bl`;
  everything else is listed for a hand-written hook.

  gen_hooks.py PATCHES_JSON DECOMP_ASM_DIR [--exclude SRC_GLOB ...] > report.txt

PATCHES_JSON lists the patches ({kind, addr, where, fn, ins, asmfile}).
Writes the patch next to this repository's other decomp patches.
"""
import fnmatch
import json
import os
import re
import subprocess
import sys
import tempfile
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DECOMP = os.path.join(ROOT, "decomp")
PATCHES = os.path.join(ROOT, "decomp-patches")
OUT_NAME = "zz-modhook-50-calls.patch"  # after every other patch: its edits are made on their result


# --- CodeWarrior demangling (just enough: qualified name, ctor/dtor) ---------
def parse_qual(s, i):
    """Parse a qualified name at s[i:]; return (list of parts, index after)."""
    if i < len(s) and s[i] == "Q" and i + 1 < len(s) and s[i + 1].isdigit():
        n = int(s[i + 1])
        i += 2
        parts = []
        for _ in range(n):
            m = re.match(r"\d+", s[i:])
            if not m:
                return None, i
            ln = int(m.group(0))
            i += len(m.group(0))
            parts.append(s[i:i + ln])
            i += ln
        return parts, i
    m = re.match(r"\d+", s[i:])
    if not m:
        return None, i
    ln = int(m.group(0))
    i += len(m.group(0))
    return [s[i:i + ln]], i + ln


def decode_part(raw):
    """A class name part, with its template arguments decoded (TFlagT<Us> -> TFlagT<unsigned short>)."""
    if "<" not in raw:
        return raw
    n, _ = cw_name(str(len(raw)) + raw, 0)
    return n or raw


def cw_demangle(sym):
    """-> (qualifier list, base name, is_const) or None."""
    special = None
    body = sym
    for pre in ("__ct__", "__dt__"):
        if sym.startswith(pre):
            special = pre[2:4]
            body = sym[len(pre):]
            quals, j = parse_qual(body, 0)
            if quals is None:
                return None
            return [decode_part(q) for q in quals], special, False
    pos = 1
    while True:
        k = sym.find("__", pos)
        if k < 0:
            return None
        name, rest = sym[:k], sym[k + 2:]
        if rest.startswith("F"):
            return [], name, False
        quals, j = parse_qual(rest, 0)
        if quals is not None and j <= len(rest):
            tail = rest[j:]
            if tail.startswith("CF") or tail.startswith("F"):
                return [decode_part(q) for q in quals], name, tail.startswith("CF")
        pos = k + 1


# CodeWarrior's long is 32 bits: the decomp spells it s32/u32, which the
# 64-bit port keeps 32 bits wide.
BASIC = {"v": "void", "b": "bool", "c": "char", "s": "short", "i": "int", "l": "s32",
         "x": "long long", "f": "float", "d": "double", "r": "long double", "w": "wchar_t"}


def cw_name(s, i):
    """<len><name, possibly with template arguments> -> (C++ name, index)."""
    m = re.match(r"\d+", s[i:])
    if not m:
        return None, i
    ln = int(m.group(0))
    i += len(m.group(0))
    raw = s[i:i + ln]
    i += ln
    lt = raw.find("<")
    if lt < 0:
        return raw, i
    args, j, out = raw[lt + 1:-1], 0, []
    while j < len(args):
        if args[j].isdigit() and not re.match(r"\d+[A-Za-z_]", args[j:]):
            m2 = re.match(r"\d+", args[j:])
            out.append(m2.group(0))
            j += len(m2.group(0))
        else:
            t, j = cw_type(args, j)
            if t is None:
                return None, i
            out.append(t)
        if j < len(args) and args[j] == ",":
            j += 1
    return raw[:lt] + "<" + ", ".join(out) + (" >" if out and out[-1].endswith(">") else ">"), i


def cw_type(s, i):
    """Decode one CodeWarrior-mangled type at s[i:] -> (C++ type, index)."""
    if i >= len(s):
        return None, i
    c = s[i]
    if c == "C":
        t, j = cw_type(s, i + 1)
        return (("const " + t) if t else None), j
    if c == "V":
        t, j = cw_type(s, i + 1)
        return (("volatile " + t) if t else None), j
    if c in "PR":
        if c == "P" and s[i + 1:i + 2] == "F":
            params, j = cw_params_at(s, i + 2)
            if params is None or s[j:j + 1] != "_":
                return None, i
            ret, j = cw_type(s, j + 1)
            if ret is None:
                return None, i
            return "%s (*)(%s)" % (ret, ", ".join(p for p in params if p != "void")), j
        m = re.match(r"A(\d+)_", s[i + 1:])
        if m:
            # pointer or reference to an array: PA4_f is float (*)[4]
            t, j = cw_type(s, i + 1 + m.end())
            return (("%s (%s)[%s]" % (t, "*" if c == "P" else "&", m.group(1))) if t else None), j
        t, j = cw_type(s, i + 1)
        return ((t + ("*" if c == "P" else "&")) if t else None), j
    if c == "U":
        t, j = cw_type(s, i + 1)
        if t == "s32":
            return "u32", j
        return (("unsigned " + t) if t else None), j
    if c == "S":
        t, j = cw_type(s, i + 1)
        return (("signed " + t) if t else None), j
    if c in BASIC:
        return BASIC[c], i + 1
    if c == "e":
        return "...", i + 1
    if c == "Q":
        parts, j = parse_qual(s, i)
        if parts is None:
            return None, i
        names = []
        k = i + 2
        for _ in range(int(s[i + 1])):
            n, k = cw_name(s, k)
            if n is None:
                return None, i
            names.append(n)
        return "::".join(names), k
    if c.isdigit():
        return cw_name(s, i)
    return None, i


def cw_params_at(s, i):
    out = []
    while i < len(s) and s[i] != "_":
        t, j = cw_type(s, i)
        if t is None:
            return None, i
        out.append(t)
        i = j
    return out, i


def cw_signature(sym):
    """-> (list of C++ parameter types, is_const) of a mangled function, or None."""
    k = sym.find("F", sym.rfind("__") + 2) if not sym.startswith("__") else -1
    # find the 'F' that starts the parameter list: after the qualifier
    dm = cw_demangle(sym)
    if not dm:
        return None
    quals, name, is_const = dm
    body = sym
    if name in ("ct", "dt"):
        body = sym[6:]
        _, j = parse_qual(body, 0)
    else:
        pos = 1
        while True:
            k = sym.find("__", pos)
            if k < 0:
                return None
            rest = sym[k + 2:]
            if rest.startswith("F"):
                body, j = rest, 0
                break
            q, j = parse_qual(rest, 0)
            if q is not None and (rest[j:].startswith("F") or rest[j:].startswith("CF")):
                body = rest
                break
            pos = k + 1
    if body[j:j + 1] == "C":
        j += 1
    if body[j:j + 1] != "F":
        return None
    params, e = cw_params_at(body, j + 1)
    if params is None or e != len(body):
        return None
    return [p for p in params if p != "void"], is_const


# --- Source scanning ----------------------------------------------------------
def blank_comments_strings(src):
    """Same length, with comments and string/char literals blanked out."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                out[k] = " "
            i = j + 1
        elif c == "#":  # preprocessor line
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def find_function_body(clean, quals, name):
    """(open brace index, close brace index) of the definition, or None."""
    if name in ("ct", "dt"):
        cls = quals[-1]
        target = (cls + "::" if True else "") + ("~" if name == "dt" else "") + cls
    elif quals:
        target = quals[-1] + "::" + name
    else:
        target = name
    hits = []
    for m in re.finditer(r"(?<![\w:])" + re.escape(target) + r"\s*\(", clean):
        # a definition: the parameter list is followed (after const/init list) by a body
        depth, j = 0, m.end() - 1
        while j < len(clean):
            if clean[j] == "(":
                depth += 1
            elif clean[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        k = j + 1
        while k < len(clean) and clean[k] in " \t\n":
            k += 1
        rest = clean[k:k + 400]
        if rest.startswith(";") or rest.startswith(","):
            continue
        # skip qualifiers and a constructor initialiser list up to '{'
        b = clean.find("{", k)
        semi = clean.find(";", k)
        if b < 0 or (semi >= 0 and semi < b and not rest.lstrip().startswith(":")):
            continue
        # the definition must start a line (return type or class qualifier before)
        line_start = clean.rfind("\n", 0, m.start()) + 1
        before = clean[line_start:m.start()]
        if re.search(r"[=({,;]\s*$", before) or "return" in before:
            continue
        depth, e = 0, b
        while e < len(clean):
            if clean[e] == "{":
                depth += 1
            elif clean[e] == "}":
                depth -= 1
                if depth == 0:
                    break
            e += 1
        hits.append((b, e))
    return hits[0] if len(hits) == 1 else None


def call_sites(clean, start, end, name):
    return [m.start() for m in re.finditer(r"(?<![\w~])" + re.escape(name) + r"\s*\(", clean[start:end])]


def match_paren(clean, i):
    depth = 0
    while i < len(clean):
        if clean[i] == "(":
            depth += 1
        elif clean[i] == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "else", "do", "case", "new", "delete"}


def expr_start(clean, j):
    """Start of the postfix-expression that ends at clean[j] (inclusive)."""
    while True:
        while j >= 0 and clean[j] in " \t\n":
            j -= 1
        if j < 0:
            return None
        if clean[j] in ")]":
            close, openc = clean[j], "(" if clean[j] == ")" else "["
            depth = 0
            while j >= 0:
                if clean[j] == close:
                    depth += 1
                elif clean[j] == openc:
                    depth -= 1
                    if depth == 0:
                        break
                j -= 1
            opener = j
            k = j - 1
            while k >= 0 and clean[k] in " \t\n":
                k -= 1
            m = re.search(r"[A-Za-z_]\w*$", clean[:k + 1]) if k >= 0 else None
            if k >= 0 and (clean[k] in ")]>" or (m and m.group(0) not in KEYWORDS and m.end() == k + 1)):
                j = k
                continue
            return opener
        m = re.search(r"[A-Za-z_]\w*$", clean[:j + 1])
        if not m or m.group(0) in KEYWORDS:
            return None
        s0 = m.start()
        k = s0 - 1
        while k >= 0 and clean[k] in " \t\n":
            k -= 1
        if k >= 1 and clean[k - 1:k + 1] == "->":
            j = k - 2
            continue
        if k >= 0 and clean[k] == "." and not clean[k - 1].isdigit():
            j = k - 1
            continue
        if k >= 1 and clean[k - 1:k + 1] == "::":
            j = k - 2
            continue
        return s0


def receiver_before(clean, i):
    """Given the index of a method name preceded by -> or ., return (start of
    the receiver expression, operator)."""
    j = i - 1
    while clean[j] in " \t\n":
        j -= 1
    if clean[j - 1:j + 1] == "->":
        op, j = "->", j - 2
    elif clean[j] == ".":
        op, j = ".", j - 1
    else:
        return None, None
    st = expr_start(clean, j)
    return (st, op) if st is not None else (None, None)


def is_statement(clean, start, close):
    j = start - 1
    while j >= 0 and clean[j] in " \t\n":
        j -= 1
    prev_ok = j < 0 or clean[j] in ";{}" or clean[max(0, j - 3):j + 1] == "else"
    if not prev_ok and clean[j] == ")":
        # the end of an if/while/for condition, not of a cast
        depth, i = 0, j
        while i >= 0:
            if clean[i] == ")":
                depth += 1
            elif clean[i] == "(":
                depth -= 1
                if depth == 0:
                    break
            i -= 1
        prev_ok = bool(re.search(r"\b(if|while|for)\s*$", clean[:max(i, 0)]))
    k = close + 1
    while k < len(clean) and clean[k] in " \t\n":
        k += 1
    return prev_ok and k < len(clean) and clean[k] == ";"


_static_cache = {}
_ns_cache = {}
_header_text = None


def headers_text():
    global _header_text
    if _header_text is None:
        parts = []
        for root, _, fs in os.walk(os.path.join(DECOMP, "include")):
            for f in fs:
                if f.endswith((".hpp", ".h")):
                    parts.append(open(os.path.join(root, f), encoding="utf-8", errors="replace").read())
        _header_text = blank_comments_strings("\n".join(parts))
    return _header_text


def is_namespace(quals):
    key = quals[-1]
    if key not in _ns_cache:
        _ns_cache[key] = bool(re.search(r"\bnamespace\s+%s\b" % re.escape(key), headers_text()))
    return _ns_cache[key]


def class_bodies(quals):
    """The bodies of every definition of the class quals names, in the headers."""
    cls = re.sub(r"<.*", "", quals[-1])
    h = headers_text()
    out = []
    for m in re.finditer(r"\b(?:class|struct)\s+%s\b[^;{]*\{" % re.escape(cls), h):
        depth, e = 0, m.end() - 1
        while e < len(h):
            if h[e] == "{":
                depth += 1
            elif h[e] == "}":
                depth -= 1
                if depth == 0:
                    break
            e += 1
        out.append(h[m.end():e])
    return out


def is_static_member(texts, quals, name):
    """Whether Class::name is declared static inside the class's definition."""
    key = ("::".join(quals), name)
    if key not in _static_cache:
        _static_cache[key] = any(re.search(r"\bstatic\b[^;{}()]*\b%s\s*\(" % re.escape(name), b)
                                 for b in class_bodies(quals))
    return _static_cache[key]


def declares_member(quals, name):
    """Whether the class quals names declares a function called name itself."""
    return any(re.search(r"[\s*&~]%s\s*\(" % re.escape(name), b) for b in class_bodies(quals))


def count_args(text):
    """The number of top-level arguments in a call's argument text."""
    if not text.strip():
        return 0
    depth, n = 0, 1
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            n += 1
    return n


CTX = []  # register context for the hook being written: [(reg, expression)]


def hook_expr(addr, stmt, original, fntype_expr, call_args):
    """The hook, written out (a statement expression): the mod's function when
    one is registered at addr, else the original call. sms_mod_r_ is the
    original call's type. CTX: the retail registers the mod function reads."""
    # The PowerPC passes integers and pointers in r3..., floats in f1...,
    # each in order: a mod function may declare them interleaved otherwise
    # than the function it stands in for (TMap::checkGround(x, y, z, water)
    # on `this` as (x, y, z, map, water)). Called with its own order here.
    types, mod = CALL["types"], CALL["mod"]
    if types is not None and mod is not None:
        args = [a for c in call_args for a in split_args(c)]
        if len(args) == len(types) and [is_fpr(t) for t in types][:len(mod)] != [is_fpr(t) for t in mod]:
            gprs = [(a, t) for a, t in zip(args, types) if not is_fpr(t)]
            fprs = [(a, t) for a, t in zip(args, types) if is_fpr(t)]
            picked = []
            for mt in mod:
                pool = fprs if is_fpr(mt) else gprs
                if not pool:
                    picked = None
                    break
                picked.append(pool.pop(0))
            if picked is not None:
                fntype_expr = "sms_mod_r_ (*)(%s)" % ", ".join(t for _, t in picked)
                call_args = [a for a, _ in picked]
    call = "((%s)sms_mod_t_)(%s)" % (fntype_expr, ", ".join(call_args))
    if CTX:
        call = "(%s, %s)" % (", ".join("SMS_MOD_GPR(%d, %s)" % (r, e) for r, e in CTX), call)
    head = "typedef __typeof__(%s) sms_mod_r_; void* sms_mod_t_ = SMS_MOD_SITE(0x%08X);" % (original, addr)
    if stmt:
        return "({ %s if (sms_mod_t_) %s; else %s; })" % (head, call, original)
    return "({ %s sms_mod_t_ ? %s : %s; })" % (head, call, original)


def member_fntype(original, qual, name, sig):
    params, is_const = sig
    pmf = "sms_mod_r_ (%s::*)(%s)%s" % (qual, ", ".join(params), " const" if is_const else "")
    return "__typeof__(sms_mod_as_free(static_cast<%s>(&%s::%s)))" % (pmf, qual, name)


# Registers the walk cannot name, read off the retail code by hand:
# (site, register) -> the source expression it holds there.
REGISTER_OVERRIDES = {
    (0x8021B144, 31): "actor",  # TLiveManager::clipActorsAux: the actor being clipped
    (0x8024C3A8, 30): "this",   # TMario::checkBackTrig: its callers keep this Mario in r30
}

# Where the mods' sources are (for the registers their functions read).
MOD_ROOTS = ["/home/user/ecl-src/bse/src", "/home/user/ecl-src/moveset/src", "/home/user/ecl-src/eclipse/src"]


def mod_register_reads(roots):
    """mod function name -> the GPRs it reads from its caller (SMS_FROM_GPR)."""
    out = {}
    for root in roots:
        for dp, _, fs in os.walk(root):
            for f in fs:
                if not f.endswith((".cpp", ".c")):
                    continue
                t = blank_comments_strings(open(os.path.join(dp, f), encoding="utf-8", errors="replace").read())
                for m in re.finditer(r"SMS_FROM_GPR\s*\(\s*(\d+)", t):
                    # the enclosing function: the last definition header before it at depth 0
                    depth, i, start = 0, m.start(), None
                    while i > 0:
                        i -= 1
                        if t[i] == "}":
                            depth += 1
                        elif t[i] == "{":
                            if depth == 0:
                                start = i
                                break
                            depth -= 1
                    if start is None:
                        continue
                    h = re.search(r"(\w+)\s*\([^;{}]*\)\s*(?:const\s*)?$", t[:start])
                    if h:
                        out.setdefault(h.group(1), set()).add(int(m.group(1)))
    return out


def mod_signatures(roots):
    """mod function name -> its parameter types, as declared at its definition."""
    out = {}
    for root in roots:
        for dp, _, fs in os.walk(root):
            for f in fs:
                if not f.endswith((".cpp", ".c")):
                    continue
                t = blank_comments_strings(open(os.path.join(dp, f), encoding="utf-8", errors="replace").read())
                for m in re.finditer(r"\b(\w+)\s*\(([^;{}()]*(?:\([^;{}()]*\)[^;{}()]*)*)\)\s*(?:const\s*)?\{", t):
                    name, plist = m.group(1), m.group(2).strip()
                    if name in ("if", "while", "for", "switch", "catch") or name in out:
                        continue
                    types = []
                    for part in plist.split(",") if plist and plist != "void" else []:
                        part = re.sub(r"=.*", "", part).strip()
                        mm = re.match(r"(.*?)\s*\b\w+\s*(\[[^\]]*\])?$", part)
                        types.append((mm.group(1) if mm and mm.group(1) else part).strip())
                    out[name] = types
    return out


def is_fpr(t):
    return re.sub(r"\bconst\b", "", t).strip() in ("f32", "float", "f64", "double")


def split_args(text):
    """A call's argument text split at its top-level commas."""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


# The call being hooked, for hook_expr: its parameter types (the object
# first for a member), and the mod function's declared parameter types.
CALL = {"types": None, "mod": None}


NOT_WRITES = ("st", "cmp", "b", "mt", "tw", "dcb", "icb", "sync", "isync", "nop", "crclr", "crset", "crxor")


def register_at(ins, site, reg, params):
    """A C++ expression for what retail register r`reg` holds at the
    instruction at `site`, from a straight-line walk of the function up to it
    (params: the expressions r3, r4... hold on entry). It follows copies,
    loads through a known pointer (the port keeps the retail layouts),
    address arithmetic, constants and small-data globals. None when that
    cannot be told."""
    held = {3 + i: e for i, e in enumerate(params) if e}
    for addr, mn, ops in ins:
        if addr >= site:
            break
        if mn in ("bl", "blrl", "bctrl"):
            # a call leaves the volatile registers undefined
            for r in [0] + list(range(3, 13)):
                held.pop(r, None)
            continue
        if mn.startswith(NOT_WRITES):
            continue
        parts = [x.strip() for x in ops.split(",")]
        m = re.fullmatch(r"r(\d+)", parts[0]) if parts else None
        if not m:
            continue  # writes a float or condition register
        dst = int(m.group(1))
        val = None
        if mn == "mr" and len(parts) > 1 and re.fullmatch(r"r\d+", parts[1]):
            val = held.get(int(parts[1][1:]))
        elif mn == "li" and len(parts) > 1 and re.fullmatch(r"-?(0x[0-9a-fA-F]+|\d+)", parts[1]):
            val = parts[1]
        elif mn == "lwz" and len(parts) > 1:
            mm = re.fullmatch(r"(-?(?:0x[0-9a-fA-F]+|\d+))\(r(\d+)\)", parts[1])
            ms = re.fullmatch(r"([A-Za-z_]\w*)@sda21\(r[02]\)", parts[1])
            if mm and held.get(int(mm.group(2))):
                val = "*(uintptr_t*)((char*)(%s) + %s)" % (held[int(mm.group(2))], mm.group(1))
            elif ms and "__" not in ms.group(1):
                val = "(uintptr_t)%s" % ms.group(1)
        elif mn == "addi" and len(parts) > 2 and re.fullmatch(r"-?(0x[0-9a-fA-F]+|\d+)", parts[2]):
            if held.get(int(parts[1][1:])) if re.fullmatch(r"r\d+", parts[1]) else None:
                val = "((char*)(%s) + %s)" % (held[int(parts[1][1:])], parts[2])
        if val is None:
            held.pop(dst, None)
        else:
            held[dst] = val
    return held.get(reg)


def param_names(clean, text, body_start):
    """The parameter names of the function whose body opens at body_start."""
    i = clean.rfind(")", 0, body_start)
    j = i
    depth = 0
    while j > 0:
        if clean[j] == ")":
            depth += 1
        elif clean[j] == "(":
            depth -= 1
            if depth == 0:
                break
        j -= 1
    names = []
    for part in text[j + 1:i].split(","):
        m = re.search(r"(\w+)\s*(?:\[[^\]]*\])?\s*(?:=.*)?$", part.strip())
        names.append(m.group(1) if m and part.strip() not in ("", "void") else None)
    return names


# Inline wrappers in the decomp that pass their arguments straight on to the
# function a retail call site calls: callee name -> wrapper names.
INLINE_WRAPPERS = {
    "getGlbResource": ("JKRGetResource",),
}

VIRTUAL_CALLS = {
    0x802A616C: "direct__Q26JDrama9TDirectorFv",                  # TApplication::gameLoop
    0x80276C94: "initValues__6TMarioFv",                          # TMario::load
    0x8024D3A0: "playerControl__6TMarioFPQ26JDrama9TGraphics",    # TMario::perform
    0x8003F8E8: "playerControl__6TMarioFPQ26JDrama9TGraphics",    # TEnemyMario::perform
    0x80222584: "initUserBuiltin__10TSpcBinaryFv",                # TSpcBinary::init
    0x802D0BEC: "makeMatrix__7J2DPaneFii",                        # J2DTextBox::draw
    0x802CEC2C: "drawChar_scale__7JUTFontFffffib",                # J2DPrint::parse
}


def entry_hook(p, fnq, clean, text, brace, modsig):
    """The statement at the top of a function a mod replaces (SMS_PATCH_B at
    its first instruction): the mod's function, with this function's
    arguments, when one is registered."""
    quals, name, is_const = fnq
    sig = cw_signature(p["fn"])
    if sig is None:
        return None
    ptypes = [t for t in sig[0] if t != "void"]
    names = param_names(clean, text, brace)
    names = [n for n in names if n] if ptypes else []
    if len(names) != len(ptypes):
        return None
    # the return type: what precedes the qualified name on the definition
    i = clean.rfind(")", 0, brace)
    depth, j = 0, i
    while j > 0:
        if clean[j] == ")":
            depth += 1
        elif clean[j] == "(":
            depth -= 1
            if depth == 0:
                break
        j -= 1
    line_start = clean.rfind("\n", 0, j) + 1
    head = text[line_start:j].strip()
    qual = "::".join(quals)
    mret = re.match(r"(.*?)\s*\b%s\s*$" % re.escape((qual + "::" if qual else "") + name), head)
    if not mret:
        return None
    ret = re.sub(r"\b(static|inline|virtual|extern)\b", "", mret.group(1)).strip() or "void"
    member = bool(quals) and not is_namespace(quals) and not is_static_member(None, quals, name)
    types = (["%s%s*" % ("const " if is_const else "", qual)] if member else []) + ptypes
    args = (["this"] if member else []) + names
    if modsig is not None and len(modsig) <= len(types):
        gprs = [(a, t) for a, t in zip(args, types) if not is_fpr(t)]
        fprs = [(a, t) for a, t in zip(args, types) if is_fpr(t)]
        picked = []
        for mt in modsig:
            pool = fprs if is_fpr(mt) else gprs
            if not pool:
                picked = None
                break
            picked.append(pool.pop(0))
        if picked is not None:
            args, types = [a for a, _ in picked], [t for _, t in picked]
    call = "((%s (*)(%s))sms_mod_t_)(%s)" % (ret, ", ".join(types), ", ".join(args))
    if ret == "void":
        act = "{ %s; return; }" % call
    else:
        act = "return %s;" % call
    return ("\n#ifdef TARGET_PC\n\t{ // replaced by a code mod (retail 0x%08X)\n"
            "\t\tvoid* sms_mod_t_ = SMS_MOD_SITE(0x%08X);\n\t\tif (sms_mod_t_)\n\t\t\t%s\n\t}\n#endif\n"
            % (p["addr"], p["addr"], act))


def c_name(sym):
    """A C function's symbol is its name: -> ([], name, False), else None."""
    if re.fullmatch(r"[A-Za-z_]\w*", sym) and "__" not in sym.lstrip("_"):
        return [], sym, False
    return None


def free_fntype(original, fnref, sig):
    params, _ = sig
    return "__typeof__(static_cast<sms_mod_r_ (*)(%s)>(&%s))" % (", ".join(params), fnref)


# --- Main -----------------------------------------------------------------
def patched_state(files, workdir):
    """Copy files with every decomp patch that sorts before OUT_NAME applied."""
    a = os.path.join(workdir, "a")
    for f in files:
        os.makedirs(os.path.dirname(os.path.join(a, f)), exist_ok=True)
        shutil.copy(os.path.join(DECOMP, f), os.path.join(a, f))
    for p in sorted(os.listdir(PATCHES)):
        if not p.endswith(".patch") or p >= OUT_NAME:
            continue
        text = open(os.path.join(PATCHES, p), encoding="utf-8", errors="surrogateescape").read()
        parts = re.split(r"(?m)^(?=--- a/)", text)
        mine = "".join(x for x in parts if x.startswith("--- a/") and
                       any(re.search(r"^\+\+\+ b/" + re.escape(f) + r"\b", x, re.M) for f in files))
        if mine:
            subprocess.run(["patch", "-s", "-p1", "-d", a], input=mine.encode("utf-8", "surrogateescape"),
                           check=True)
    b = os.path.join(workdir, "b")
    shutil.copytree(a, b)
    return a, b


def main():
    args = sys.argv[1:]
    exclude = []
    while "--exclude" in args:
        i = args.index("--exclude")
        exclude.append(args[i + 1])
        del args[i:i + 2]
    patches = json.load(open(args[0]))
    asm_dir = args[1]

    # every bl in the retail functions: fn -> [(addr, callee)]; every
    # instruction: fn -> [(addr, mnemonic, operands)]
    bls = {}
    insns = {}
    modregs = mod_register_reads(MOD_ROOTS)
    modsigs = mod_signatures(MOD_ROOTS)
    import glob
    for f in glob.glob(asm_dir + "/**/*.s", recursive=True):
        fn = None
        for line in open(f, errors="replace"):
            m = re.match(r"\.fn (\S+),", line)
            if m:
                fn = m.group(1).strip('"')
                continue
            m = re.match(r"/\* ([0-9A-F]{8}) [0-9A-F]{8}  [0-9A-F ]{11} \*/\s*(\S+)\s*(.*)", line)
            if m and fn:
                insns.setdefault(fn, []).append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
                if m.group(2) == "bl":
                    bls.setdefault(fn, []).append((int(m.group(1), 16), m.group(3).strip().strip('"')))

    # Virtual calls (blrl) a mod redirects: the function each one calls, read
    # off the retail code around it; treated as a direct call to it.
    for p in patches:
        if p["kind"] == "SMS_PATCH_BL" and p.get("ins", "").startswith("blrl") and p["addr"] in VIRTUAL_CALLS:
            callee = VIRTUAL_CALLS[p["addr"]]
            p["ins"] = "bl " + callee
            bls.setdefault(p["fn"], []).append((p["addr"], callee))
    todo = [p for p in patches if p["kind"] == "SMS_PATCH_BL" and p.get("ins", "") and p["ins"].startswith("bl ")
            and not any(fnmatch.fnmatch(p["where"], g) for g in exclude)]
    # one hook per retail address (a later registration wins at run time anyway)
    # addresses a hand-written modhook-* patch already hooks
    hand = set()
    for name in os.listdir(PATCHES):
        if name.startswith("modhook-") and name.endswith(".patch"):
            t = open(os.path.join(PATCHES, name), encoding="utf-8", errors="surrogateescape").read()
            hand.update(int(a, 16) for a in re.findall(r"^\+.*SMS_MOD_\w+\(\s*(0x[0-9A-Fa-f]{8})", t, re.M))
    todo = [p for p in todo if p["addr"] not in hand]
    seen, uniq = set(), []
    for p in todo:
        if p["addr"] not in seen:
            seen.add(p["addr"])
            uniq.append(p)

    # Function replacements (SMS_PATCH_B at a function's first instruction):
    # an entry hook that calls the mod's function and returns its result.
    entries = []
    for p in patches:
        if p["kind"] != "SMS_PATCH_B" or any(fnmatch.fnmatch(p["where"], g) for g in exclude):
            continue
        ins_ = insns.get(p.get("fn"), [])
        if ins_ and ins_[0][0] == p["addr"] and p["addr"] not in hand and all(e["addr"] != p["addr"] for e in entries):
            entries.append(p)

    srcs = {}
    for p in uniq + entries:
        base = os.path.splitext(p["asmfile"])[0]
        for ext in (".cpp", ".c", ".cp"):
            if os.path.exists(os.path.join(DECOMP, "src", base + ext)):
                srcs[p["addr"]] = "src/" + base + ext
                break

    work = tempfile.mkdtemp()
    a, b = patched_state(sorted(set(srcs.values())), work)
    texts = {f: open(os.path.join(b, f), encoding="utf-8", errors="surrogateescape").read() for f in set(srcs.values())}

    done, manual = [], []
    ctx_unknown = []  # [(patch, register)] the hook could not supply
    edits = {}  # file -> [(start, end, replacement)]
    for p in uniq:
        why = None
        f = srcs.get(p["addr"])
        fn, callee = p["fn"], p["ins"].split(None, 1)[1].strip('"')
        caller = cw_demangle(fn) or c_name(fn)
        target = cw_demangle(callee) or c_name(callee)
        if not f:
            why = "no source file"
        elif not caller or not target:
            why = "cannot demangle %s / %s" % (fn, callee)
        elif target[1] in ("ct", "dt") or target[1].startswith("__"):
            why = "constructor/destructor/operator call"
        if why:
            manual.append((p, why))
            continue
        clean = blank_comments_strings(texts[f])
        body = find_function_body(clean, caller[0], caller[1])
        if not body:
            manual.append((p, "definition of %s not found once in %s" % ("::".join(caller[0] + [caller[1]]), f)))
            continue
        sites = call_sites(clean, body[0], body[1], target[1])
        for alias in INLINE_WRAPPERS.get(target[1], ()):
            # calls through an inline wrapper that passes its arguments on
            sites = sorted(sites + call_sites(clean, body[0], body[1], alias))
        if not sites and not target[0] and target[1].startswith("PS"):
            # the SDK's paired-single matrix functions, called by their
            # generic names (MTXCopy is PSMTXCopy)
            sites = call_sites(clean, body[0], body[1], target[1][2:])
        retail = [a for a, c in bls.get(fn, []) if c == callee]
        if len(sites) != len(retail) or p["addr"] not in retail:
            manual.append((p, "%d calls to %s in the source, %d in retail" % (len(sites), target[1], len(retail))))
            continue
        name_at = body[0] + sites[retail.index(p["addr"])]
        # the retail registers the mod function reads (SMS_FROM_GPR)
        CTX[:] = []
        regs = modregs.get(p["value"].lstrip("&"), ())
        if regs:
            names = param_names(clean, texts[f], body[0])
            member = bool(caller[0]) and not is_namespace(caller[0]) and not is_static_member(
                None, caller[0], caller[1])
            exprs = (["this"] if member else []) + names
            for r in sorted(regs):
                e = REGISTER_OVERRIDES.get((p["addr"], r)) or register_at(insns.get(fn, []), p["addr"], r, exprs)
                if e is None:
                    ctx_unknown.append((p, r))
                else:
                    CTX.append((r, e))
        sig_ = cw_signature(callee)
        CALL["mod"] = modsigs.get(p["value"].lstrip("&"))
        CALL["types"] = None
        if sig_ is not None:
            ptypes = [t for t in sig_[0] if t != "void"]
            is_member = bool(target[0]) and not is_namespace(target[0]) and not (
                not sig_[1] and is_static_member(None, target[0], target[1]))
            CALL["types"] = ((["%s%s*" % ("const " if sig_[1] else "", "::".join(target[0]))] if is_member else [])
                             + ptypes)
        paren = clean.index("(", name_at)
        close = match_paren(clean, paren)
        argtext = texts[f][paren + 1:close].strip()
        tq, tname = target[0], target[1]
        qual = "::".join(tq)
        sig0 = cw_signature(callee)
        if sig0 is not None and "..." not in sig0[0] and count_args(clean[paren + 1:close]) > len(sig0[0]):
            manual.append((p, "the source call has more arguments than %s: an inline wrapper" % callee))
            continue
        rstart, op = receiver_before(clean, name_at)
        stmt = None
        if op:
            obj = texts[f][rstart:name_at].rstrip()
            obj = obj[:-2] if obj.endswith("->") else obj[:-1]
            obj = obj.strip()
            objptr = obj if op == "->" else "&(%s)" % obj
            start = rstart
            original = texts[f][start:close + 1]
            if not tq:
                manual.append((p, "method call to an unqualified function"))
                continue
            stmt = is_statement(clean, start, close)
            sig = cw_signature(callee)
            if sig is None:
                manual.append((p, "cannot decode the signature of %s" % callee))
                continue
            rep = hook_expr(p["addr"], stmt, original, member_fntype(original, qual, tname, sig),
                            [objptr] + ([argtext] if argtext else []))
        else:
            # qualified static call, free function, or member via implicit this
            j = name_at
            q = re.search(r"((?:[A-Za-z_]\w*\s*::\s*)+)$", clean[:name_at])
            start = q.start(1) if q else name_at
            original = texts[f][start:close + 1]
            stmt = is_statement(clean, start, close)
            if tq and not q and not caller[0]:
                manual.append((p, "unqualified call to a member from a free function"))
                continue
            if tq and not q and caller[0] != tq and not is_namespace(tq) and declares_member(caller[0], tname):
                manual.append((p, "called through %s::%s, an inline wrapper" % ("::".join(caller[0]), tname)))
                continue
            sig = cw_signature(callee)
            args_ = [argtext] if argtext else []
            if sig is None and not tq and c_name(callee):
                # a C function: its own type (a mod's replacement is called
                # through it, variadic ones included, as the retail bl was)
                rep = hook_expr(p["addr"], stmt, original, "__typeof__(&%s)" % tname, args_)
                edits.setdefault(f, []).append((start, close + 1, rep, p))
                done.append(p)
                continue
            if sig is None:
                manual.append((p, "cannot decode the signature of %s" % callee))
                continue
            if tq and not q and is_namespace(tq):
                rep = hook_expr(p["addr"], stmt, original, free_fntype(original, qual + "::" + tname, sig), args_)
            elif tq and not q and not sig[1] and is_static_member(texts, tq, tname):
                rep = hook_expr(p["addr"], stmt, original, free_fntype(original, qual + "::" + tname, sig), args_)
            elif tq and not q:
                rep = hook_expr(p["addr"], stmt, original, member_fntype(original, qual, tname, sig),
                                ["this"] + args_)
            elif tq and (is_namespace(tq) or (not sig[1] and is_static_member(texts, tq, tname))):
                rep = hook_expr(p["addr"], stmt, original, free_fntype(original, qual + "::" + tname, sig), args_)
            elif tq:
                # a qualified call to a member function (Base::method(...)) on this
                rep = hook_expr(p["addr"], stmt, original, member_fntype(original, qual, tname, sig),
                                ["this"] + args_)
            else:
                rep = hook_expr(p["addr"], stmt, original, free_fntype(original, tname, sig), args_)
        edits.setdefault(f, []).append((start, close + 1, rep, p))
        done.append(p)

    for p in entries:
        f = srcs.get(p["addr"])
        fnq = cw_demangle(p["fn"]) or c_name(p["fn"])
        if not f or not fnq:
            manual.append((p, "function replacement: no source or name"))
            continue
        clean = blank_comments_strings(texts[f])
        body = find_function_body(clean, fnq[0], fnq[1])
        if not body:
            manual.append((p, "function replacement: definition not found once in %s" % f))
            continue
        rep = entry_hook(p, fnq, clean, texts[f], body[0], modsigs.get(p["value"].lstrip("&")))
        if rep is None:
            manual.append((p, "function replacement: cannot write the hook"))
            continue
        edits.setdefault(f, []).append((body[0] + 1, body[0] + 1, rep, p))
        done.append(p)

    for f, es in edits.items():
        t = texts[f]
        for start, end, rep, p in sorted(es, key=lambda e: -e[0]):
            t = t[:start] + rep + t[end:]
        lines = t.split("\n")
        last = max(i for i, l in enumerate(lines) if l.startswith("#include"))
        lines.insert(last + 1, "#include <sms_modhook.h>")
        open(os.path.join(b, f), "w", encoding="utf-8", errors="surrogateescape").write("\n".join(lines))

    diff = subprocess.run(["diff", "-ru", "a", "b"], cwd=work, capture_output=True, text=True,
                          errors="surrogateescape").stdout
    diff = re.sub(r"^diff -ru .*\n", "", diff, flags=re.M)
    diff = re.sub(r"^--- a/(\S+).*$", r"--- a/\1", diff, flags=re.M)
    diff = re.sub(r"^\+\+\+ b/(\S+).*$", r"+++ b/\1", diff, flags=re.M)
    reason = ("Reason: port: code-mod call hooks (sms_modhook.h), generated by tools/mods/gen_hooks.py: each "
              "call a code mod redirects with SMS_PATCH_BL at a retail address goes to the mod's function "
              "when one is registered, and to the original otherwise")
    open(os.path.join(PATCHES, OUT_NAME), "w", encoding="utf-8", errors="surrogateescape").write(
        reason + "\n" + diff)
    print("hooked %d of %d redirected calls; %d need a hand-written hook" % (len(done), len(uniq), len(manual)))
    for p, r in ctx_unknown:
        print("  %08X reads r%d, which the hook cannot supply  [%s]" % (p["addr"], r, p["where"]))
    for p, why in manual:
        print("  %08X %-40s %s  [%s]" % (p["addr"], (p.get("fn") or "?")[:40], why, p["where"]))
    shutil.rmtree(work)


if __name__ == "__main__":
    main()
