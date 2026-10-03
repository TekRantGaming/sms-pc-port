#!/usr/bin/env python3
# compare.py RECORDS RESULTS_A RESULTS_B [examples]: per-function count of
# results that differ in any bit (two NaNs count as equal).
import struct, sys
inp = open(sys.argv[1], 'rb').read()
A = open(sys.argv[2], 'rb').read()
B = open(sys.argv[3], 'rb').read()
shown = int(sys.argv[4]) if len(sys.argv) > 4 else 3
names = ("sinf cosf tanf atanf atan2f acosf atan atan2 _inv_sqrtf expf powf std::fmodf "
         "std::sqrtf sqrtf_seq TUtil::mod TUtil::sqrt TUtil::inv_sqrt MsSqrtf JPASqrtf").split()
tot, bad, ex = {}, {}, {}
for i in range(len(inp) // 24):
    fn, _, a, b = struct.unpack('>IIdd', inp[i * 24:i * 24 + 24])
    ra, rb = A[i * 8:i * 8 + 8], B[i * 8:i * 8 + 8]
    tot[fn] = tot.get(fn, 0) + 1
    if ra == rb:
        continue
    x, y = struct.unpack('>d', ra)[0], struct.unpack('>d', rb)[0]
    if x != x and y != y:
        continue
    bad[fn] = bad.get(fn, 0) + 1
    ex.setdefault(fn, []).append((a, b, ra.hex(), rb.hex()))
total_bad = 0
for fn in sorted(tot):
    print(f"{names[fn]:11s} {tot[fn]:9d} inputs, {bad.get(fn, 0)} differ")
    total_bad += bad.get(fn, 0)
    for a, b, x, y in ex.get(fn, [])[:shown]:
        print(f"    ({a!r}, {b!r}): {x} vs {y}")
sys.exit(1 if total_bad else 0)
