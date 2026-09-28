#!/usr/bin/env python3
"""List the retail-address patches of a Kuribo module (BetterSunshineEngine,
Super Mario Eclipse) by the game function they land in.

  tools/mods/patch_inventory.py [--elf mario.elf --objdump powerpc-eabi-objdump]
                                SYMBOLS.TXT SRC_DIR [SRC_DIR...] > inventory.md

SYMBOLS.TXT is the decomp's config/GMSE01/symbols.txt. Every
SMS_PATCH_BL/SMS_PATCH_B/SMS_WRITE_32 whose address is written as
SMS_PORT_REGION(us, ...) or a bare 0x8... literal is resolved to
function+offset; a native port has to re-express each one at that spot in
the source (a hook, a changed constant, a replaced call). With the decomp's
linked mario.elf and a PowerPC objdump, each row also shows the retail
instruction it replaces (for a bl, the function whose call is redirected).
"""
import bisect
import os
import re
import subprocess
import sys
from collections import defaultdict

sym_re = re.compile(r"^(\S+) = \.(\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")
patch_re = re.compile(
    r"(SMS_PATCH_BL|SMS_PATCH_B|SMS_WRITE_32)\s*\(\s*"
    r"(?:SMS_PORT_REGION\s*\(\s*(0x[0-9A-Fa-f]+)\s*,[^)]*\)|(0x[0-9A-Fa-f]+))\s*,\s*(.*?)\)\s*;",
    re.S)


def load_symbols(path):
    funcs = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = sym_re.match(line)
        if not m or m.group(4) != "function":
            continue
        addr = int(m.group(3), 16)
        size = int(m.group(5), 16) if m.group(5) else 4
        funcs.append((addr, size, m.group(1)))
    funcs.sort()
    return funcs


def resolve(funcs, starts, addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0:
        a, size, name = funcs[i]
        if a <= addr < a + size:
            return name, addr - a
    return None, None


def load_code(elf, objdump):
    """address -> retail instruction text, from a full disassembly."""
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf], capture_output=True, text=True).stdout
    code = {}
    for line in out.splitlines():
        m = re.match(r"^\s*([0-9a-f]{8}):\s+(.*)$", line)
        if m:
            code[int(m.group(1), 16)] = " ".join(m.group(2).split())
    return code


def main():
    args = sys.argv[1:]
    code = {}
    if args and args[0] == "--elf":
        elf, objdump = args[1], args[3]
        args = args[4:]
        code = load_code(elf, objdump)
    funcs = load_symbols(args[0])
    starts = [f[0] for f in funcs]
    rows = []
    for root in args[1:]:
        for dp, _, fns in os.walk(root):
            for fn in fns:
                if not fn.endswith((".cpp", ".hxx", ".hpp", ".h", ".c")):
                    continue
                path = os.path.join(dp, fn)
                text = open(path, encoding="utf-8", errors="replace").read()
                for m in patch_re.finditer(text):
                    kind = m.group(1)
                    addr = int(m.group(2) or m.group(3), 16)
                    arg = " ".join(m.group(4).split())
                    line = text.count("\n", 0, m.start()) + 1
                    name, off = resolve(funcs, starts, addr)
                    rows.append((name or "?", off or 0, addr, kind, arg, os.path.relpath(path, root), line))
    by_func = defaultdict(list)
    for r in rows:
        by_func[r[0]].append(r)
    print(f"{len(rows)} patches in {len(by_func)} game functions\n")
    retail = " retail instruction |" if code else ""
    print(f"| game function | offset | address | kind |{retail} replacement / value | source |")
    print("| --- | --- | --- | --- |" + (" --- |" if code else "") + " --- | --- |")
    for name in sorted(by_func, key=lambda n: (-len(by_func[n]), n)):
        for r in sorted(by_func[name], key=lambda r: r[1]):
            ins = f" `{code.get(r[2], '?')[:70]}` |" if code else ""
            print(f"| `{r[0]}` | +0x{r[1]:X} | 0x{r[2]:08X} | {r[3]} |{ins} `{r[4][:60]}` | {r[5]}:{r[6]} |")


if __name__ == "__main__":
    main()
