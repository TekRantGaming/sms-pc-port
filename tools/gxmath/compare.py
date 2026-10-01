#!/usr/bin/env python3
# compare.py RECORDS RESULTS_A RESULTS_B [examples]: per-function count of
# records whose results differ in any bit (two NaNs count as equal: the
# Gekko's default NaN is positive, SSE's negative).
import struct, sys
inp = open(sys.argv[1], 'rb').read()
A = open(sys.argv[2], 'rb').read()
B = open(sys.argv[3], 'rb').read()
shown = int(sys.argv[4]) if len(sys.argv) > 4 else 3
names = ['GXInitLightDistAttn', 'GXInitSpecularDir', 'GXProject', 'GXDrawSphere']
tot, bad, ex = {}, {}, {}
pos = 0
for i in range(len(inp) // 128):
    rec = inp[i * 128:(i + 1) * 128]
    fn, iarg = struct.unpack('>II', rec[:8])
    n = 24 if fn < 3 else (iarg >> 8) * ((iarg & 255) + 1) * 48
    ra, rb = A[pos:pos + n], B[pos:pos + n]
    pos += n
    tot[fn] = tot.get(fn, 0) + 1
    if len(ra) != n or len(rb) != n:
        sys.exit('%s: results end early at record %d' % (names[fn], i))
    if ra == rb:
        continue
    fa = struct.unpack('>%df' % (n // 4), ra)
    fb = struct.unpack('>%df' % (n // 4), rb)
    if all(x == y and ra[k * 4:k * 4 + 4] == rb[k * 4:k * 4 + 4] or (x != x and y != y)
           for k, (x, y) in enumerate(zip(fa, fb))):
        continue
    bad[fn] = bad.get(fn, 0) + 1
    args = struct.unpack('>30f', rec[8:])
    ex.setdefault(fn, []).append((iarg, args[:6], ra[:24].hex(), rb[:24].hex()))
if pos != len(A) or pos != len(B):
    sys.exit('result lengths %d and %d, expected %d' % (len(A), len(B), pos))
total_bad = 0
for fn in sorted(tot):
    print(f"{names[fn]:20s} {tot[fn]:9d} inputs, {bad.get(fn, 0)} differ")
    total_bad += bad.get(fn, 0)
    for e in ex.get(fn, [])[:shown]:
        print('    ', e)
sys.exit(1 if total_bad else 0)
