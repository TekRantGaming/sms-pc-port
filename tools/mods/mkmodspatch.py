#!/usr/bin/env python3
"""Write platform/mods/eclipse/mods-port.patch: the port's hand edits to the mods' sources.

mkmodspatch.py FIXED_UP_PRISTINE EDITED

Both are directories holding bse/, eclipse/ and moveset/: FIXED_UP_PRISTINE fixed up by
fixup_sources.py without mods-port.patch, EDITED the same with the port's edits made
(SMS_OFFSET for retail offsets, named statics for retail data addresses...).
"""
import difflib, glob, os, sys

base, edited = sys.argv[1], sys.argv[2]
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../platform/mods/eclipse/mods-port.patch')
parts = []; n = 0
for mod in ('bse', 'eclipse', 'moveset'):
    for sub in ('src', 'include'):
        for f in sorted(glob.glob('%s/%s/%s/**/*' % (edited, mod, sub), recursive=True)):
            if os.path.isdir(f): continue
            rel = os.path.relpath(f, edited); g = os.path.join(base, rel)
            a = open(g, 'rb').read().replace(b'\r\n', b'\n').decode('utf-8', 'surrogateescape')
            b = open(f, 'rb').read().replace(b'\r\n', b'\n').decode('utf-8', 'surrogateescape')
            if a != b:
                n += 1
                parts += [l if l.endswith('\n') else l + '\n\\ No newline at end of file\n'
                          for l in difflib.unified_diff(a.splitlines(True), b.splitlines(True), 'a/' + rel, 'b/' + rel, n=3)]
open(out, 'w', encoding='utf-8', errors='surrogateescape').write(''.join(parts))
print('mods-port.patch: %d files' % n)
