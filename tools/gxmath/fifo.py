#!/usr/bin/env python3
"""fifo.py FUNCTION < lifted.s > out.s

Points a lifted GX function's writes to the write-gather pipe at a buffer, for
tools/gxmath/check.sh: the DOL's GXDrawSphere loads 0xCC010000 into r30
(lis r30,0xCC01) and stores each vertex component with stfs fN,-0x8000(r30),
all to the pipe's one address 0xCC008000. Here r30 becomes gxstream's address
(gxstream is 64 KiB aligned) and each store an stfsu fN,4(r30), so the floats
land one after another from gxstream + 4, in the order the console sends them.
No other instruction changes."""
import re, sys

fn = sys.argv[1]
cur, off, out = None, 0, []
for line in sys.stdin.read().splitlines():
    m = re.match(r'(\w+):$', line)
    if m:
        cur, off = m.group(1), 0
    m = re.match(r'\t\.4byte 0x([0-9a-f]{8})$', line)
    if m and cur == fn:
        w = int(m.group(1), 16)
        if w == 0x3fc0cc01:  # lis r30,-13311
            out.append('\t.reloc %s+%d, R_PPC_ADDR16_HA, gxstream' % (fn, off + 2))
            w = 0x3fc00000
        elif w >> 26 == 52 and (w >> 16) & 31 == 30 and w & 0xffff == 0x8000:  # stfs fN,-32768(r30)
            w = (53 << 26) | (w & (31 << 21)) | (30 << 16) | 4  # stfsu fN,4(r30)
        line = '\t.4byte 0x%08x' % w
        off += 4
    out.append(line)
print('\n'.join(out))
