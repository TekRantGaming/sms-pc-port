#!/usr/bin/env python3
"""How much of the code mods' patching the port covers.

  port_status.py PATCHES_JSON [--list] [--registered MODLIST]

A patch counts as ported when a decomp patch (decomp-patches/*.patch) names
its retail address in a hook (SMS_MOD_*), or when it is listed in
tools/mods/not_ported.txt with the reason it needs no port (the feature is
the port's own). With --registered (the output of a run with SMS_MOD_LIST=1),
patches the mods never register (their code is compiled out) are inactive.
"""
import json
import os
import re
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def hooked_addresses():
    out = set()
    d = os.path.join(ROOT, "decomp-patches")
    for name in os.listdir(d):
        if name.endswith(".patch"):
            t = open(os.path.join(d, name), encoding="utf-8", errors="replace").read()
            for line in t.splitlines():
                if line.startswith("+"):
                    out.update(int(a, 16) for a in re.findall(r"\b0x(8[0-3][0-9A-Fa-f]{6})\b", line))
    return out


def waived():
    out = {}
    p = os.path.join(ROOT, "tools/mods/not_ported.txt")
    if os.path.exists(p):
        for line in open(p):
            line = line.split("#", 1)[0].strip()
            if line:
                glob_, _, why = line.partition(" ")
                out[glob_] = why.strip()
    return out


def main():
    patches = json.load(open(sys.argv[1]))
    hooked = hooked_addresses()
    wv = waived()
    registered = None
    if "--registered" in sys.argv:
        registered = set()
        for line in open(sys.argv[sys.argv.index("--registered") + 1]):
            m = re.match(r"\[mod\] (?:bl|b |w ) ([0-9a-f]{8}) ", line)
            if m:
                registered.add(int(m.group(1), 16))
    import fnmatch
    rows = []
    for p in patches:
        kind = p["kind"].replace("SMS_", "").replace("PATCH_", "")
        if kind == "BL" and not p.get("ins", "").startswith("bl"):
            kind = "BL-insn"
        where = p["where"]
        w = next((why for g, why in wv.items() if fnmatch.fnmatch(where, g)), None)
        if p["addr"] in hooked:
            state = "ported"
        elif w:
            state = "waived"
        elif registered is not None and p["addr"] not in registered:
            state = "inactive"
        else:
            state = "todo"
        rows.append((state, kind, p))
    c = Counter((k, s) for s, k, _ in rows)
    kinds = sorted({k for _, k, _ in rows})
    print("%-8s %6s %6s %8s %6s" % ("kind", "ported", "waived", "inactive", "todo"))
    for k in kinds:
        print("%-8s %6d %6d %8d %6d" % (k, c[(k, "ported")], c[(k, "waived")], c[(k, "inactive")], c[(k, "todo")]))
    if "--list" in sys.argv:
        for s, k, p in sorted(rows, key=lambda r: (r[2]["where"], r[2]["addr"])):
            if s == "todo":
                print("%-7s %08X %-45s %s  %s" % (k, p["addr"], (p.get("fn") or "?")[:45], p.get("value", "")[:30], p["where"]))


if __name__ == "__main__":
    main()
