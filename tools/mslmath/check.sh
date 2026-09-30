#!/bin/sh
# tools/mslmath/check.sh [N]: checks platform/misc/msl_math.c (MSL's
# trigonometry for the host) on N random inputs per function (default 200000).
#
# 1. 32-bit against 64-bit: links harness.c with the msl_math.c.o objects that
#    went into build/linux-32/sms and build/linux-64/sms. Results must be
#    bit-identical.
# 2. Against the console's machine code, when qemu-ppc and a decomp build are
#    present (SMS_DECOMP_BUILD, default decomp/build: its binutils/ and the
#    split objects in GMSE01/obj): runs the DOL's own MSL objects under qemu-ppc
#    and compares a test build of msl_math.c that uses qemu's frsqrte (exact
#    1/sqrt). Results must be bit-identical. The port's build differs from it
#    only there, where it follows the Gekko's estimate (see msl_math.c).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/tools/mslmath
n=${1:-200000}
cc=${CC:-cc}
flags="-O2 -msse2 -mfpmath=sse -ffp-contract=off -I$root/src"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
status=0

$cc $flags -o "$out/gen" "$here/harness.c" "$root/platform/misc/msl_math.c" -lm
"$out/gen" gen "$n" 0x5eed > "$out/in.bin"
"$out/gen" gen "$n" 0x5eed 8 > "$out/inq.bin" # also _inv_sqrtf, through a test hook

for arch in 32 64; do
	obj=$root/build/linux-$arch/CMakeFiles/sms.dir/platform/misc/msl_math.c.o
	if [ -f "$obj" ]; then
		$cc -m$arch $flags -no-pie -o "$out/h$arch" "$here/harness.c" "$obj" -lm
		"$out/h$arch" run < "$out/in.bin" > "$out/out$arch"
	fi
done
if [ -f "$out/out32" ] && [ -f "$out/out64" ]; then
	echo "== build/linux-32 against build/linux-64"
	python3 "$here/compare.py" "$out/in.bin" "$out/out32" "$out/out64" || status=1
else
	echo "== skipped 32 against 64: build both build/linux-32 and build/linux-64 first"
fi

dbuild=${SMS_DECOMP_BUILD:-$root/decomp/build}
objs=$dbuild/GMSE01/obj/PowerPC_EABI_Support/Msl/MSL_C
bin=$dbuild/binutils
if command -v qemu-ppc > /dev/null && [ -d "$objs" ] && [ -x "$bin/powerpc-eabi-ld" ]; then
	m=$objs/MSL_Common_Embedded/Math
	"$bin/powerpc-eabi-as" -mregnames -o "$out/driver.o" "$here/ppc_driver.s"
	"$bin/powerpc-eabi-ld" -T "$here/ppc.ld" -o "$out/msl.elf" "$out/driver.o" \
		"$m/Single_precision/trigf.o" "$m/Single_precision/inverse_trig.o" \
		"$m/Single_precision/hyperbolicsf.o" "$m/Single_precision/common_float_tables.o" \
		"$m/Double_precision/s_atan.o" "$m/Double_precision/e_atan2.o" \
		"$m/Double_precision/w_atan2.o" "$m/Double_precision/e_asin.o" \
		"$objs/MSL_Common/float.o"
	$cc $flags -DMSL_MATH_TEST_HOOKS -DMSL_MATH_TEST_FRSQRTE=qemu_frsqrte \
		-DHARNESS_QEMU -o "$out/hq" "$here/harness.c" "$root/platform/misc/msl_math.c" -lm
	qemu-ppc "$out/msl.elf" < "$out/inq.bin" > "$out/outppc"
	"$out/hq" run < "$out/inq.bin" > "$out/outq"
	echo "== the DOL's MSL objects under qemu-ppc against msl_math.c (qemu arithmetic)"
	python3 "$here/compare.py" "$out/inq.bin" "$out/outppc" "$out/outq" || status=1
else
	echo "== skipped the qemu-ppc check: needs qemu-ppc and $objs"
fi
exit $status
