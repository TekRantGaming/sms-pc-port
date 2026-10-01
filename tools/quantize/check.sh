#!/bin/sh
# tools/quantize/check.sh: checks port_gekko_quantize (src/port_fpu.h), the
# Gekko's quantised integer store that OSf32tos8 makes (decomp-patches/fpu-07),
# against a separate statement of the model on every float bit pattern (s8,
# scale 0) and on samples of every type and scale, built for 32 and 64-bit x86
# with SSE maths (the port's flags) and with x87 maths. The s8 checksums must
# agree. See docs/64-BIT.md, item 18.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cc=${CC:-cc}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
status=0
for v in "-m32 -msse2 -mfpmath=sse" "-m64" "-m32 -mfpmath=387"; do
	$cc $v -O2 -ffp-contract=off -I"$root/src" -o "$out/t" "$root/tools/quantize/test.c" -lm
	printf '%-26s ' "$v:"
	"$out/t" all | tee -a "$out/sums" || status=1
done
[ "$(sort -u "$out/sums" | wc -l)" = 1 ] || { echo "checksums differ"; status=1; }
"$out/t" table
exit $status
