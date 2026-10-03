#!/bin/sh
# tools/gxmath/check.sh [N]: checks platform/gx/src/gx_sdk_math.h, the SDK's
# GX float arithmetic the port computes (GXInitLightDistAttn,
# GXInitSpecularDir, GXProject, GXDrawSphere), on N random inputs per function
# (default 200000) and 517 sphere sizes.
#
# 1. 32-bit against 64-bit: harness.cpp built with sms_gx's float flags in
#    each word size, linked with the msl_math.c.o objects of build/linux-32 and
#    build/linux-64 (sinf and cosf), and again with -mfma (an FMA instruction
#    available, as on hosts other than x86: -ffp-contract=off must keep every
#    product rounded). Results must be bit-identical.
# 2. Against the console's machine code, when qemu-ppc and a decomp build are
#    present (SMS_DECOMP_BUILD, default decomp/build: its binutils/ and the
#    split objects in GMSE01/obj): the DOL's GXInitLightDistAttn,
#    GXInitSpecularDir, GXProject and GXDrawSphere, lifted out of GXLight.o,
#    GXTransform.o and GXDraw.o by tools/mslmath/lift.py (GXDrawSphere's
#    write-gather pipe pointed at a buffer by fifo.py) and linked with the DOL's
#    trigf.o (and the weak fabsf it calls, in hyperbolicsf.o), run under
#    qemu-ppc. Results must be bit-identical (two NaNs count
#    as equal).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/tools/gxmath
n=${1:-200000}
cc=${CC:-cc}
cxx=${CXX:-c++}
mslflags="-O2 -msse2 -mfpmath=sse -ffp-contract=off -I$root/src"
gxflags="-O2 -std=c++17 -fno-strict-aliasing -msse2 -mfpmath=sse -ffp-contract=off -I$root/platform/gx/src -I$root/src"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
status=0

for arch in 32 64; do
	obj=$root/build/linux-$arch/CMakeFiles/sms.dir/platform/misc/msl_math.c.o
	if [ ! -f "$obj" ]; then
		$cc -m$arch $mslflags -c -o "$out/msl$arch.o" "$root/platform/misc/msl_math.c"
		obj=$out/msl$arch.o
	fi
	$cxx -m$arch $gxflags -no-pie -o "$out/h$arch" "$here/harness.cpp" "$obj" -lm
	$cxx -m$arch $gxflags -mfma -no-pie -o "$out/f$arch" "$here/harness.cpp" "$obj" -lm
done
"$out/h64" gen "$n" 0x5eed > "$out/in.bin"
for b in h32 h64 f32 f64; do
	"$out/$b" run < "$out/in.bin" > "$out/$b.out"
done
echo "== 32-bit against 64-bit"
python3 "$here/compare.py" "$out/in.bin" "$out/h32.out" "$out/h64.out" || status=1
for arch in 32 64; do
	echo "== 32-bit against the $arch-bit -mfma build"
	python3 "$here/compare.py" "$out/in.bin" "$out/h32.out" "$out/f$arch.out" || status=1
done

dbuild=${SMS_DECOMP_BUILD:-$root/decomp/build}
objs=$dbuild/GMSE01/obj
bin=$dbuild/binutils
if command -v qemu-ppc > /dev/null && [ -d "$objs/dolphin/gx" ] && [ -x "$bin/powerpc-eabi-ld" ]; then
	m=$objs/PowerPC_EABI_Support/Msl/MSL_C/MSL_Common_Embedded/Math/Single_precision
	"$bin/powerpc-eabi-as" -mregnames -o "$out/driver.o" "$here/ppc_driver.s"
	python3 "$root/tools/mslmath/lift.py" "$bin" \
		"$objs/dolphin/gx/GXLight.o" GXInitLightDistAttn dol_GXInitLightDistAttn \
		"$objs/dolphin/gx/GXLight.o" GXInitSpecularDir dol_GXInitSpecularDir \
		"$objs/dolphin/gx/GXTransform.o" GXProject dol_GXProject \
		"$objs/dolphin/gx/GXDraw.o" GXDrawSphere dol_GXDrawSphere |
		python3 "$here/fifo.py" dol_GXDrawSphere > "$out/lifted.s"
	"$bin/powerpc-eabi-as" -mregnames -o "$out/lifted.o" "$out/lifted.s"
	"$bin/powerpc-eabi-ld" -T "$root/tools/mslmath/ppc.ld" -o "$out/gx.elf" "$out/driver.o" \
		"$out/lifted.o" "$m/trigf.o" "$m/common_float_tables.o" "$m/hyperbolicsf.o"
	qemu-ppc "$out/gx.elf" < "$out/in.bin" > "$out/ppc.out"
	echo "== the DOL's objects under qemu-ppc against the 64-bit build"
	python3 "$here/compare.py" "$out/in.bin" "$out/ppc.out" "$out/h64.out" || status=1
else
	echo "== skipped the qemu-ppc check: needs qemu-ppc and $objs"
fi
exit $status
