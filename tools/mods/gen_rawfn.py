#!/usr/bin/env python3
"""Generate the game-function trampolines code mods call by retail name
(platform/mods/eclipse/rawfn_trampolines.cpp).

SunshineHeaderInterface's SMS/raw_fn.hxx names every retail function by its
CodeWarrior symbol, as a cast of its retail address:

    #define getNameRef__Q26JDrama11TNameRefGenCFPCc ((int (*)(...))0x802FAF0C)

The port has no code at those addresses. fixup_sources.py points each macro
at sms_rawfn_<symbol> instead; this writes those functions: each takes the
arguments as a variadic call passes them (float promoted to double, small
integers to int, references as the pointers a mod passes) and calls the
decomp's function of that name, returning what the macro's type says.

Only the symbols the mods use get a trampoline; a use this cannot decode
gets one that reports the symbol and stops, so it fails loudly.

SunshineHeaderInterface cuts a symbol short at its first template argument
list, so each is decoded from the retail map line at the macro's address.

  gen_rawfn.py SHI_ROOT RETAIL_MAP MOD_SOURCE_ROOT... > platform/mods/eclipse/rawfn_trampolines.cpp

RETAIL_MAP is a name=0xaddress list of the NTSCU symbols (BetterSunshineEngine's
maps/us.map).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_hooks as gh  # noqa: E402

SMALL_INTS = {"bool", "char", "signed char", "unsigned char", "short", "unsigned short"}


def raw_fn_types(shi_root):
    """symbol -> (the return type its NTSCU macro gives, retail address)."""
    text = open(os.path.join(shi_root, "include/SMS/raw_fn.hxx"), encoding="utf-8", errors="replace").read()
    m = re.search(r"#ifdef NTSCU\n(.*?)\n#(?:elif|else|endif)", text, re.S)
    out = {}
    for d in re.finditer(r"#define\s+(\w+)\s+\(\((\w+) \(\*\)\(\.\.\.\)\)(?:sms_rawfn_\w+\) /\* )?(0x[0-9A-Fa-f]+)", m.group(1)):
        ret = d.group(2)
        # int-returning macros return a pointer-sized integer (fixup_sources.py),
        # and f32 and f64 ones the float in f1, as float and double do
        ret = {"int": "intptr_t", "__INTPTR_TYPE__": "intptr_t", "f32": "float", "f64": "double"}.get(ret, ret)
        out[d.group(1)] = (ret, int(d.group(3), 16))
    return out


def retail_map(path):
    """retail address -> full symbol."""
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        name, _, addr = line.strip().rpartition("=")
        if name and addr.startswith("0x"):
            out.setdefault(int(addr, 16), name)
    return out


def used_symbols(roots, known):
    used = set()
    for root in roots:
        for dp, _, fs in os.walk(root):
            if "/.git" in dp:
                continue
            for f in fs:
                if f.endswith((".cpp", ".hxx", ".hpp", ".h", ".c")) and f != "raw_fn.hxx":
                    t = open(os.path.join(dp, f), encoding="utf-8", errors="replace").read()
                    for w in re.findall(r"\b[A-Za-z_]\w*\b", t):
                        if w in known:
                            used.add(w)
    return sorted(used)


def param(t, k, typed=False):
    """-> (declaration of trampoline parameter k, expression passing it on).
    typed: called through a prototype, not variadically."""
    t = t.strip()
    if typed and "(*)" not in t and "(&)" not in t:
        return "%s p%d" % (t, k), "p%d" % k
    if "(*)" in t or "(&)" in t:
        # function or array pointer: the name goes inside the declarator
        return t.replace("(*)", "(*p%d)" % k, 1).replace("(&)", "(*p%d)" % k, 1), (
            "*p%d" % k if "(&)" in t else "p%d" % k)
    if t == "float":
        return "double p%d" % k, "(float)p%d" % k
    if t in SMALL_INTS:
        return "int p%d" % k, "(%s)p%d" % (t, k)
    if t.endswith("&"):
        base = t[:-1].strip()
        return "%s* p%d" % (base, k), "*p%d" % k
    return "%s p%d" % (t, k), "p%d" % k


_hdr_files = None


def header_for(quals, name):
    """The decomp header that defines the class quals[0] names, or declares
    the free function name."""
    global _hdr_files
    if _hdr_files is None:
        _hdr_files = []
        for inc in gh.include_roots():
            for dp, _, fs in os.walk(inc):
                for f in sorted(fs):
                    if f.endswith((".hpp", ".h")):
                        path = os.path.join(dp, f)
                        _hdr_files.append((os.path.relpath(path, inc),
                                           gh.blank_comments_strings(open(path, encoding="utf-8", errors="replace").read())))
    if quals and not gh.is_namespace(quals):
        cls = re.sub(r"<.*", "", quals[-1])
        pat = r"\b(?:class|struct)\s+%s\b[^;{()]*\{" % re.escape(cls)
    else:
        pat = r"[\s*&]%s\s*\(" % re.escape(name)
    for rel, t in _hdr_files:
        if re.search(pat, t):
            return rel
    return None


def c_params(name):
    """The parameter types a decomp header declares the C function name with."""
    header_for([], name)
    for rel, t in _hdr_files:
        m = re.search(r"[\s*&]%s\s*\(([^()]*)\)\s*;" % re.escape(name), t)
        if m:
            HEADERS.add(rel)
            ps = [p.strip() for p in m.group(1).split(",") if p.strip() and p.strip() != "void"]
            # drop parameter names: the last identifier of a declaration
            return [re.sub(r"\s*\b[A-Za-z_]\w*$", "", p) if re.search(r"[\w*&]\s+[A-Za-z_]\w*$", p) else p
                    for p in ps]
    return None


def trampoline(sym, ret, full, typed=False):
    """ret None: typed (a prototyped call through a retail address), the
    function's own return type."""
    fixed = full or sym
    if "__" not in fixed:
        # a C function
        params = c_params(fixed)
        if params is None:
            return None, "C function without a declaration"
        decls, args = [], []
        for k, t in enumerate(params):
            d, a = param(t, k)
            decls.append(d)
            args.append(a)
        call = "%s(%s)" % (fixed, ", ".join(args))
        body = ("return (%s)(%s);" % (ret, call) if ret in ("float", "double") else
                "return rawfn_int([&]() -> decltype(%s) { return %s; });" % (call, call))
        return "extern \"C\" %s sms_rawfn_%s(%s)\n{\n\t%s\n}\n" % (ret, sym, ", ".join(decls), body), None
    dm = gh.cw_demangle(fixed)
    sig = gh.cw_signature(fixed) if dm else None
    if not dm or sig is None:
        return None, "cannot decode"
    quals, name, is_const = dm
    if "<" in name:
        # a function template instance: CLBPalFrame<l> is CLBPalFrame<s32>
        name = gh.decode_part(name)
    hdr = header_for(quals, re.sub(r"<.*", "", name))
    if hdr and not hdr.startswith("PowerPC_EABI_Support/"):
        HEADERS.add(hdr)
    params = [p for p in sig[0] if p != "void"]
    for ident in set(re.findall(r"\b[A-Z]\w+", " ".join(quals + params))):
        h = header_for([ident], ident)
        if h and not h.startswith("PowerPC_EABI_Support/"):
            HEADERS.add(h)
    if "..." in params:
        return None, "variadic"
    decls, args = [], []
    for k, t in enumerate(params):
        d, a = param(t, k, typed)
        decls.append(d)
        args.append(a)
    qual = "::".join(quals)
    member = bool(quals) and not gh.is_namespace(quals) and not (
        name not in ("ct", "dt") and not is_const and gh.is_static_member(None, quals, name))
    if member:
        decls.insert(0, "void* self")
    if name == "ct":
        body = "new (self) %s(%s); return (intptr_t)self;" % (qual, ", ".join(args))
        return "extern \"C\" intptr_t sms_rawfn_%s(%s)\n{\n\t%s\n}\n" % (sym, ", ".join(decls), body), None
    if name == "dt":
        return None, "destructor"
    if member:
        obj = "((%s%s*)self)" % ("const " if is_const else "", qual)
        call = "%s->%s::%s(%s)" % (obj, qual, name, ", ".join(args))
    elif qual == "std":
        # MSL's float math: port_compat.h routes std::fmodf and the rest to
        # the console's (platform/misc/msl_math.c)
        call = "std::%s(%s)" % (name, ", ".join(args))
    else:
        call = "%s%s(%s)" % (qual + "::" if qual else "", name, ", ".join(args))
    if typed:
        return "extern \"C\" auto sms_rawaddr_%s(%s) -> decltype(%s)\n{\n\treturn %s;\n}\n" % (
            sym, ", ".join(decls), call, call), None
    if ret in ("float", "double"):
        body = "return (%s)(%s);" % (ret, call)
    else:
        body = "return rawfn_int([&]() -> decltype(%s) { return %s; });" % (call, call)
    return "extern \"C\" %s sms_rawfn_%s(%s)\n{\n\t%s\n}\n" % (ret, sym, ", ".join(decls), body), None


HEADERS = set()

# fixup_sources.py matches the same casts
# (before or after it, when the address is already sms_mod_rawaddr(...))
RAWADDR_CALL = (r"\(\s*\([^;{}()]*\(\s*\*\s*\)\s*\([^;{}()]*\)\s*\)\s*(?:sms_mod_rawaddr\()?"
                r"(0x8[0-3][0-9A-Fa-f]{6})\)?\s*\)\s*\(")

RAWADDR_TABLE = """static const struct {
	unsigned int addr;
	void* fn;
} sRawAddrs[] = {
%s
};

// The port's function for a retail address a mod calls through a cast.
extern "C" void* sms_mod_rawaddr(unsigned int addr)
{
	for (unsigned i = 0; i < sizeof(sRawAddrs) / sizeof(sRawAddrs[0]); i++)
		if (sRawAddrs[i].addr == addr)
			return sRawAddrs[i].fn;
	fprintf(stderr, "[mod] %%08x: no port function for this retail address\\n", addr);
	abort();
}
"""

HEAD = """// Generated by tools/mods/gen_rawfn.py: the game functions code mods call
// through SunshineHeaderInterface's SMS/raw_fn.hxx, by their retail names.
// Each takes its arguments as a variadic call passes them and calls the
// decomp's function; see the generator for the details.
#include <new>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <type_traits>

%s

namespace {
// An int-returning macro's value (pointer-sized: fixup_sources.py makes the
// macros return intptr_t, so pointers survive on 64-bit hosts): integers as
// they are, pointers and references as addresses, nothing for void.
template <class T, bool = std::is_void<T>::value> struct RawInt {
	template <class F> static intptr_t get(F f) { return conv(f()); }
	template <class U> static intptr_t conv(U* p) { return (intptr_t)p; }
	template <class U>
	static typename std::enable_if<std::is_integral<U>::value || std::is_enum<U>::value, intptr_t>::type conv(U v)
	{
		return (intptr_t)v;
	}
	template <class U> static typename std::enable_if<std::is_class<U>::value, intptr_t>::type conv(U& v)
	{
		return (intptr_t)&v;
	}
};
template <class T> struct RawInt<T, true> {
	template <class F> static intptr_t get(F f)
	{
		f();
		return 0;
	}
};
template <class F> intptr_t rawfn_int(F f) { return RawInt<decltype(f())>::get(f); }

void rawfn_missing(const char* sym)
{
	fprintf(stderr, "[mod] %%s: no port function for this retail call\\n", sym);
	abort();
}
} // namespace

"""


def main():
    shi, mapfile, roots = sys.argv[1], sys.argv[2], sys.argv[3:]
    types = raw_fn_types(shi)
    names = retail_map(mapfile)
    used = used_symbols(roots, types)
    bodies, missing, headers = [], [], set()
    for sym in used:
        ret, addr = types[sym]
        code, why = trampoline(sym, ret, names.get(addr))
        if code is None:
            missing.append((sym, why))
            code = "extern \"C\" %s sms_rawfn_%s()\n{\n\trawfn_missing(\"%s\");\n\treturn 0;\n}\n" % (
                ret, sym, sym)
        bodies.append(code)
    # Calls through a literal retail address, ((void (*)(TMarDirector *))0x8029C6F8)(d):
    # fixup_sources.py turns the address into sms_mod_rawaddr(0x8029C6F8),
    # which looks it up in this table.
    addrs = set()
    for root in roots:
        for dp, _, fs in os.walk(root):
            if "/.git" in dp or "/libs/dolphin" in dp:
                continue
            for f in fs:
                if f.endswith((".cpp", ".hxx", ".hpp", ".h", ".c")):
                    t = open(os.path.join(dp, f), encoding="utf-8", errors="replace").read()
                    addrs.update(int(a, 16) for a in re.findall(RAWADDR_CALL, t))
    table = []
    for addr in sorted(addrs):
        tag = "%08X" % addr
        code, why = (None, "not in the map") if addr not in names else trampoline(tag, None, names[addr], True)
        if code is None:
            missing.append((names.get(addr, tag), why))
            code = "extern \"C\" void sms_rawaddr_%s()\n{\n\trawfn_missing(\"%s\");\n}\n" % (tag, tag)
        bodies.append(code)
        table.append("\t{0x%s, (void*)&sms_rawaddr_%s}," % (tag, tag))
    bodies.append(RAWADDR_TABLE % "\n".join(table))
    for sym, why in missing:
        print("gen_rawfn: %s: %s" % (sym, why), file=sys.stderr)
    print("gen_rawfn: %d trampolines, %d stop with a message" % (len(used), len(missing)), file=sys.stderr)
    sys.stdout.write(HEAD % "\n".join("#include <%s>" % h for h in sorted(HEADERS)))
    sys.stdout.write("\n".join(bodies))


if __name__ == "__main__":
    main()
