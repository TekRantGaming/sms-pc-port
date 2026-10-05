#!/usr/bin/env python3
"""Complete-object constructors and destructors the mods call that the game only has as base-object ones.

    structor_aliases.py NM libsms_game.a libsms_eclipse.a OUT {gnu|apple}

The mods call a game class's complete-object constructor or destructor (C1,
D1). GCC on ELF emits those as aliases of the base-object ones (C2, D2) when
the class has no virtual bases; clang on Mach-O emits only the variants the
game itself uses. For each C1/D1 the mods need and nothing defines, where the
game defines the C2/D2 of the same signature (the same code, for a class with
no virtual bases), this writes an alias: a GNU ld script of PROVIDE()s, or an
ld64 -alias_list.
"""
import re
import subprocess
import sys

STRUCTOR = re.compile(r"([CD])1(E|I)")


def symbols(nm, lib, prefix):
    defined, undefined = set(), set()
    out = subprocess.run([nm, "-g", lib], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2 or line.endswith(":"):
            continue
        name, kind = parts[-1], parts[-2]
        if prefix and name.startswith(prefix):
            name = name[len(prefix):]
        (undefined if kind in "Uw" else defined).add(name)
    return defined, undefined


def main(nm, game, mods, out, style):
    prefix = "_" if style == "apple" else ""
    game_defs, _ = symbols(nm, game, prefix)
    mod_defs, mod_needs = symbols(nm, mods, prefix)
    aliases = []
    for name in sorted(mod_needs - mod_defs - game_defs):
        for m in STRUCTOR.finditer(name):
            base = name[:m.start()] + m.group(1) + "2" + name[m.start() + 2:]
            if base in game_defs:
                aliases.append((base, name))
                break
    with open(out, "w") as f:
        for target, alias in aliases:
            if style == "apple":
                f.write("_%s _%s\n" % (target, alias))
            else:
                f.write("PROVIDE(%s = %s);\n" % (alias, target))
    print("structor_aliases: %d complete-object structors from their base-object ones" % len(aliases))


if __name__ == "__main__":
    if len(sys.argv) != 6 or sys.argv[5] not in ("gnu", "apple"):
        sys.exit(__doc__)
    main(*sys.argv[1:])
