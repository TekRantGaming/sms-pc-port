#!/bin/sh
# tools/mtxmath/check.sh [N]: checks platform/mtx (the GameCube's MTX/VEC
# library, and JSystem's and the game's other paired-single routines, for the
# host) on N random inputs per case (default 200000; cases in gen.py,
# including in-place calls).
#
# 1. 32-bit against 64-bit: links harness.c with the mtx.cpp.o and msl_math.c.o
#    that went into build/linux-32/sms and build/linux-64/sms, each run with the
#    CPU's FMA instruction and with the software fused multiply-add
#    (SMS_MTX_SOFT_FMA=1). All four must give bit-identical results.
# 2. Against the console's machine code, when qemu-ppc and a decomp build are
#    present (SMS_DECOMP_BUILD, default decomp/build: its binutils/ and the
#    split objects in GMSE01/obj): qemu-ppc has no paired-single instructions,
#    so ps2scalar.py rewrites the DOL's own mtx.o, mtx44.o, mtxvec.o and vec.o
#    (and C_MTXFrustum, which the DOL lacks, from the decomp's build), and the
#    routines of J3DTransform.o, J3DModel.o, J3DAnimation.o and MathUtil.o
#    (with the J3DPSMulMtxVec copies inlined in J3DCluster.o lifted out by
#    inline.py) into scalar code, one operation per half; they run under
#    qemu-ppc with the
#    DOL's MSL trigonometry, and a test build of mtx.cpp that follows qemu's
#    arithmetic (MTX_TEST_QEMU: frsqrte 1/sqrt, fres 1/x, no 25-bit multiplier
#    input) must give bit-identical results. The port's build differs from it
#    only in those points, where it follows the Gekko (see mtx.cpp).
# Two NaNs count as equal (the Gekko's default NaN is positive, SSE's negative).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/tools/mtxmath
n=${1:-200000}
chunk=${CHUNK:-10000}
cc=${CC:-cc}
cxx=${CXX:-c++}
flags="-O2 -msse2 -mfpmath=sse -ffp-contract=off"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

python3 "$here/gen.py" "$out"
$cc $flags -no-pie -I"$out" -o "$out/gen" "$here/harness.c" -x none "$root/build/linux-64/CMakeFiles/sms.dir/platform/mtx/mtx.cpp.o" \
	"$root/build/linux-64/CMakeFiles/sms.dir/platform/misc/msl_math.c.o" -lm 2> /dev/null ||
	{ echo "build build/linux-64 first"; exit 1; }
for arch in 32 64; do
	d=$root/build/linux-$arch/CMakeFiles/sms.dir/platform
	[ -f "$d/mtx/mtx.cpp.o" ] && $cxx -m$arch $flags -I"$out" -no-pie -o "$out/h$arch" -x c "$here/harness.c" -x none \
		"$d/mtx/mtx.cpp.o" "$d/misc/msl_math.c.o" -lm
done

dbuild=${SMS_DECOMP_BUILD:-$root/decomp/build}
objs=$dbuild/GMSE01/obj
bin=$dbuild/binutils
qemu=
if command -v qemu-ppc > /dev/null && [ -f "$objs/dolphin/mtx/mtx.o" ] && [ -x "$bin/powerpc-eabi-ld" ]; then
	qemu=1
	m=$objs/PowerPC_EABI_Support/Msl/MSL_C/MSL_Common_Embedded/Math
	j3d=$objs/JSystem/J3D
	python3 "$here/inline.py" "$bin" "$j3d/J3DGraphAnimator/J3DCluster.o" "$j3d/J3DGraphBase/J3DTransform.o" > "$out/inline.s"
	"$bin/powerpc-eabi-as" -mregnames -o "$out/inline.o" "$out/inline.s"
	python3 "$here/ps2scalar.py" "$bin" m="$objs/dolphin/mtx/mtx.o" m44="$objs/dolphin/mtx/mtx44.o" \
		mv="$objs/dolphin/mtx/mtxvec.o" v="$objs/dolphin/mtx/vec.o" \
		d44="$dbuild/GMSE01/src/dolphin/mtx/mtx44.o:C_MTXFrustum" x="$out/inline.o" \
		j="$j3d/J3DGraphBase/J3DTransform.o:J3DPSCalcInverseTranspose__FPA4_fPA3_f,J3DMtxProjConcat__FPA4_fPA4_fPA4_f,J3DMTXConcatArrayIndexedSrc__FPA4_CfPA3_A4_CfPCUsPA3_A4_fUl,J3DPSMtxArrayConcat__FPA4_fPA4_fPA4_fUl" \
		a="$j3d/J3DGraphAnimator/J3DAnimation.o:J3DHermiteInterpolationS__FfPsPsPsPsPsPs" \
		u="$objs/MarioUtil/MathUtil.o:MsVECMag2__FP3Vec,MsVECNormalize__FP3VecP3Vec" \
		wm@r25="$j3d/J3DGraphAnimator/J3DModel.o:calcWeightEnvelopeMtx__8J3DModelFv" > "$out/mtxps.s"
	"$bin/powerpc-eabi-as" -mregnames -o "$out/mtxps.o" "$out/mtxps.s"
	"$bin/powerpc-eabi-as" -mregnames -o "$out/driver.o" "$out/driver.s"
	"$bin/powerpc-eabi-ld" -T "$root/tools/mslmath/ppc.ld" -o "$out/mtx.elf" "$out/driver.o" "$out/mtxps.o" \
		"$m/Single_precision/trigf.o" "$m/Single_precision/inverse_trig.o" \
		"$m/Single_precision/hyperbolicsf.o" "$m/Single_precision/common_float_tables.o" \
		"$m/Double_precision/s_atan.o" "$m/Double_precision/e_atan2.o" \
		"$m/Double_precision/w_atan2.o" "$m/Double_precision/e_asin.o" \
		"$objs/PowerPC_EABI_Support/Msl/MSL_C/MSL_Common/float.o"
	# the same include paths as the build
	inc=$(python3 -c "import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['file'].endswith('platform/mtx/mtx.cpp'):
        print(' '.join(x for x in shlex.split(e['command']) if x[:2] in ('-I', '-D') or x == '-isystem' or x.startswith('/usr/include')))" \
		"$root/build/linux-64/compile_commands.json")
	$cxx $flags $inc -std=gnu++11 -DMTX_TEST_QEMU -c -o "$out/mtxq.o" "$root/platform/mtx/mtx.cpp"
	$cc $flags -I"$root/src" -DMSL_MATH_TEST_FRSQRTE=qemu_frsqrte \
		-c -o "$out/mslq.o" "$root/platform/misc/msl_math.c"
	$cxx $flags -no-pie -I"$out" -DHARNESS_QEMU -o "$out/hq" -x c "$here/harness.c" -x none "$out/mtxq.o" "$out/mslq.o" -lm
else
	echo "== skipped the qemu-ppc check: needs qemu-ppc and $objs"
fi

# chunks of $chunk records per case, results summed per case
: > "$out/sum"
i=0
while [ $((i * chunk)) -lt "$n" ]; do
	c=$chunk
	[ $(((i + 1) * chunk)) -gt "$n" ] && c=$((n - i * chunk))
	"$out/gen" gen "$c" $((0x5eed + i)) > "$out/in.bin"
	ref=
	for arch in 32 64; do
		[ -x "$out/h$arch" ] || continue
		"$out/h$arch" run < "$out/in.bin" > "$out/r$arch.hw"
		SMS_MTX_SOFT_FMA=1 "$out/h$arch" run < "$out/in.bin" > "$out/r$arch.sw"
		[ -z "$ref" ] && ref=$out/r$arch.hw
		for r in "$out/r$arch.hw" "$out/r$arch.sw"; do
			"$out/gen" cmp "$out/in.bin" "$ref" "$r" | sed "s/^/host $(basename "$r") /" >> "$out/sum"
		done
	done
	if [ -n "$qemu" ]; then
		qemu-ppc "$out/mtx.elf" < "$out/in.bin" > "$out/rppc"
		"$out/hq" run < "$out/in.bin" > "$out/rq"
		"$out/gen" cmp "$out/in.bin" "$out/rppc" "$out/rq" | sed 's/^/qemu ppc /' >> "$out/sum"
	fi
	i=$((i + 1))
done
echo "== linux-32 and linux-64, FMA instruction (hw) and software (sw), against linux-32 hw"
echo "== the DOL's objects under qemu-ppc against mtx.cpp with qemu's arithmetic"
awk '/ inputs, / { k = $1 " " $2 " " $3; t[k] += $(NF - 3); d[k] += $(NF - 1); if (!(k in o)) { o[k] = ++m; key[m] = k } next }
	{ if (ex[$1 $2]++ < 4) print }
	END { for (j = 1; j <= m; j++) { printf "%-40s %9d inputs, %d differ\n", key[j], t[key[j]], d[key[j]]; bad += d[key[j]] }
		exit bad > 0 }' "$out/sum"
