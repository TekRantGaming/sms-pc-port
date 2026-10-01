# 64-bit build

Goal: a native 64-bit build (`SMS_ARCH=64`) alongside the existing 32-bit one, which must keep behaving exactly as it does today.
Build and run it with `SMS_ARCH=64 ./build.sh` and `SMS_ARCH=64 ./run.sh` (output in `build/linux-64/`); the macOS build (`build/macos-64/`) is always this 64-bit build.

Why it is worth it: no multilib or i386 driver packages on Linux (the 32-bit NVIDIA userspace is a common failure point), macOS and ARM64 hosts only run 64-bit code, and distributions keep dropping i386.

## Status (branch `port-64bit`, decomp branch `port-64bit` in sms-english)

Decisions: option B (pointer-size neutral spellings in the decomp, byte-identical under MWCC) for the recurring patterns, `ptr64-*` port patches for one-off adaptations, and game memory stays at `0x80000000`.

Done:

1. **Links**: the two declaration/definition mismatches are fixed in the decomp.
2. **Boots**: every host thread (and the boot thread running `SMS_main`) gets a stack below 2 GiB (`port_low_alloc`, `MAP_32BIT` on x86-64 Linux); `PTR32` in the port's `dolphin/types.h` is a 4-byte pointer slot on 64-bit hosts and traps on an address above 4 GiB; `sms_gx` now compiles against the port's `types.h`, so its `GXTexObj`/`GXTlutObj` overlays match the game's.
3. **Heaps**: `ptr64-01-heap-sizes` doubles the heaps the game sizes with GameCube constants (`PORT_HEAP64`), and 64-bit MEM1 defaults to 64 MiB.
4. **Decomp (all byte-identical, DOL unchanged)**: `PTR32` on the pointer fields of structs laid over file data (RARC file entries, JAudio init-data tables and sequence archive header, J3D loader blocks, vertex-colour animation index data, collision groups, pollution layer records) and on word-indexed runtime records (JAudio port args); `sizeof` instead of byte counts (JAudio DVD task records, message buffers, particle heap headers); `u32` instead of signed ints where an int becomes a pointer (script VM pops, `JSUConvertOffsetToPtr`, JKRDvdArchive, JUTTexture); the J3D material and material-packet ID flags spelled as bits 31/30 of a `u32`.
5. **Result**: the 64-bit build boots, plays the opening movie (frames byte-identical to 32-bit), loads Delfino Plaza and renders it like the 32-bit build (the scripted plaza frames differ in at most 2 pixels, from x87 against SSE float rounding).
   The 32-bit plaza and beach reference runs are byte-identical to before every step.
6. **Coverage**: all 19 movies of the `SMS_WARP_MOVIE` sweep play in 64-bit with no fault (69 of their 76 captured frames byte-identical to 32-bit, the rest within 3 pixels), and the scripted beach run matches the 32-bit one within 9 pixels per frame, with gameplay in step.
7. **Stages**: warping to episode 0 of the airstrip, Bianco Hills, Ricco Harbor, Gelato Beach, Pinna Park, Sirena Beach, the Delfino Hotel, Noki Bay and Pianta Village runs in 64-bit with frames matching 32-bit (under 15 differing pixels, except Ricco and Gelato at 100-300 scattered edge pixels).
   Pinna Park needed `ptr64-02-atan-table-wrap`: a NaN ratio makes `GetAtanTable` index `atntable[INT_MIN]`, which 32-bit addresses wrap back to entry 0 and a 64-bit host does not.
8. **Upstream merge** (decomp `0085b21c`): upstream's conformance edits cast `J3DAnmVtxColorIndexData::mpData` (a `PTR32(void)`) straight to `s32`, which a 64-bit `PTR32` cannot do; `ptr64-04-J3DAnmLoader-vtx-color-index-offsets` goes through `void*` as the former spelling did.
9. **Float parity**: the remaining 32/64-bit pixel differences (0 to 504 scattered edge pixels per plaza frame, up to 114 levels; 1 or 2 levels on the title screen) came from `platform/mtx/mtx.cpp`.
   The platform layer is built without the game's `-mfpmath=sse`, so the 32-bit build evaluates the MTX/VEC sums of products in x87 extended precision, while the 64-bit build rounded each operation to `f32`; the resulting last-bit differences in model and view matrices moved triangle edges by a pixel.
   `mtx.cpp` then spelled that evaluation out in `long double` (x87 on every x86 host), with the few `f32` roundings the 32-bit build made (`spill()`), so both builds compute the same matrices: every function is bit-identical to the old 32-bit object over 200,000 random inputs each, the 32-bit frames are unchanged, and the scripted title and plaza frames are byte-identical between 32 and 64-bit.
   Still host-dependent then: `tanf` in `C_MTXPerspective`/`C_MTXLightPerspective`, and the game's calls to `atan2f`, `atanf`, `acosf` and `tanf`, whose i386 glibc results (x87 `fpatan` for the arc functions) differ in the last bit from x86-64 glibc for 3 to 16% of inputs; item 10 replaced them.
10. **MSL trigonometry**: the game's trigonometry is now the console's own, so it no longer depends on the host's libm at all.
   The DOL calls MSL, not a host library: `sinf`, `cosf`, `tanf` (`trigf.c`), `atanf`, `atan2f`, `acosf` (`inverse_trig.c`) and fdlibm's `atan2` and `atan` (`w_atan2.c`, `e_atan2.c`, `s_atan.c`), all byte-matching in the decomp.
   Those results differ from glibc's for 22 to 55% of inputs in the game's ranges (MSL's `sinf` and `cosf` are cheaper, less exact polynomials), besides the i386 against x86-64 differences above.
   `platform/misc/msl_math.c` is those functions for the host, and `port_compat.h` routes the game's calls to them with MSL's overloads (`sin(float)`, `cos(float)` and `atan2(float, float)` are the float functions; `std::atan2f` is fdlibm's `atan2` in double, as MSL's header spells it).
   `platform/mtx` calls them where the SDK's `mtx.c` and `mtx44.c` do (`MTXRotRad`, `MTXRotAxisRad`, the two perspective functions).
   The C sources could not simply be compiled for the host: MWCC made their polynomials Gekko fused multiply-adds, so `msl_math.c` follows the matched objects' instructions one for one.
   A single-precision `fmadds` becomes `(float)((double)a * c + b)`, which is the Gekko's rounding to double and then to single (the model Dolphin uses): the product of two singles is exact in double.
   Double `fmadd` becomes `fma()`, `fctiwz` saturates as the Gekko does, `frsqrte` is `port_gekko_frsqrte`, and the constants are the DOL's bit patterns.
   The file is built with SSE maths and `-ffp-contract=off` on every x86 host (x87 extended precision would change the roundings).
   `tools/mslmath/check.sh` checks it:
   the `msl_math.c.o` of `build/linux-32` and of `build/linux-64` give bit-identical results on 1,000,000 random inputs per function;
   and the DOL's own MSL objects (the decomp's split of the original), run under `qemu-ppc`, give bit-identical results to a test build of `msl_math.c` that uses qemu's arithmetic (fused multiply-adds rounded once to single, an exact `frsqrte`), on 1,000,000 inputs per function.
   Rounding `fmadds` once or twice never gave a different result in 9,000,000 inputs, so the rounding model is not a practical risk.
   The scripted title and plaza frames stay byte-identical between 32 and 64-bit.
   Against the previous 32-bit frames, they change where the game's trigonometry moves something: on the title screen a seagull's flight path (up to 1,497 pixels, field 1200), and in the plaza the idle animation of the NPCs and fruit in the background (8,700 to 16,500 pixels per frame; Mario, the camera and the HUD are unchanged).
   The 60 fps plaza gate run still captures Mario after 100 frames with the same `setNextStage`.
   Still host libm then: `powf` and `expf` (thought not yet matching in the decomp; `exponentialsf.c` was in fact matched), `sqrtf`/`sqrt` (MSL's are inlines of `frsqrte` with Newton-Raphson steps that MWCC expanded at each call site), and `std::fmodf` (an inline in MSL's header, `x - y * (float)(long long)(x / y)`); i386 and x86-64 glibc give identical `powf`, `expf`, `fmodf` and `sqrtf` results on 3,000,000 random inputs in the game's ranges.
   Item 15 replaced them.
11. **MTX/VEC as the console computes them**: `platform/mtx` no longer reproduces the old 32-bit x87 build (item 9) but the GameCube's own matrix library.
   The DOL links the SDK's paired-single routines of `mtx.c`, `mtxvec.c` and `vec.c` (`PSMTXIdentity`, `Copy`, `Concat`, `Inverse`, `RotRad`, `RotTrig`, `RotAxisRad`, `Trans`, `TransApply`, `Scale`, `ScaleApply`, `Quat`, `PSMTXMultVec`, `MultVecArray`, `MultVecSR`, `PSVECAdd`, `Subtract`, `Scale`, `Normalize`, `Mag`, `DotProduct`, `CrossProduct`, `SquareDistance`, `Distance`), and C routines only where the SDK has no paired-single version (`C_MTXLookAt`, `C_MTXLightFrustum`, `C_MTXLightPerspective`, `C_MTXLightOrtho` in `mtx.c`, `C_MTXPerspective`, `C_MTXOrtho` in `mtx44.c`, built with `-fp_contract off`): `marioUS.MAP` and the decomp's byte-matching objects.
   Those are exactly the routines the game's objects call; the other `C_` names are not in the DOL and now forward to the paired-single routine, and `PSMTXTranspose`, `PSVECSquareMag` and `C_MTXFrustum` (not in the DOL, never called) stay for the link.
   `platform/mtx/mtx_ps.inc` follows each routine's machine code instruction for instruction, one paired-single register as two floats: every operation rounds to single, each fused multiply-add (`ps_madd`, `ps_madds0`, `fnmsubs`, ...) rounds once, and the order, the in-place behaviour and `ps_sum`/`ps_merge` halves are the DOL's.
   `frsqrte` and `fres` are the Gekko's estimates (`port_fpu.h`); the Newton step after `frsqrte` in `PSVECNormalize`, `PSVECMag` and `PSVECDistance` multiplies by the estimate as `fmuls`'s frC operand, which the Gekko rounds to 25 significant bits first (Dolphin's `Force25Bit`; the estimate has 27).
   The fused multiply-adds are the CPU's FMA instruction where it has one (`__builtin_cpu_supports("fma")`, both word sizes), otherwise `fma_soft`, an exact single-rounding fallback (the double sum, redone with its TwoSum error and rounded to odd when it lands on a single-precision midpoint or in the subnormal range; identical to glibc's `fmaf` on 200,000,000 random triples in each word size); `SMS_MTX_SOFT_FMA=1` forces it.
   GCC folds `-fma(a, c, -b)` into `fnmadd`, which gives +0 where `fnmsubs` gives -0, so negations of fused results go through an optimiser barrier.
   `mtx.cpp` is built with SSE maths and no contraction on every x86 host, as `msl_math.c` is.
   `tools/mtxmath/check.sh` checks it on 40 cases (every routine, and the in-place calls the game makes):
   the `mtx.cpp.o` of `build/linux-32` and of `build/linux-64`, each with the FMA instruction and with `fma_soft`, give bit-identical results on 1,000,000 random inputs per case;
   and the DOL's own objects run under `qemu-ppc` give bit-identical results to a test build of `mtx.cpp` that uses qemu's arithmetic, on 1,000,000 inputs per case (two NaNs count as equal: the Gekko's default NaN is positive, SSE's negative).
   qemu has no paired-single instructions, so `tools/mtxmath/ps2scalar.py` rewrites the DOL's `mtx.o`, `mtx44.o`, `mtxvec.o` and `vec.o` into scalar code first: each instruction in order, a paired-single one as the same scalar operation on both halves, with ps1 in a shadow array, and single-precision scalar results written to both halves as on the Gekko.
   qemu's own `frsqrte` is 1/sqrt, its `fres` 1/x (and 1/2 for 0), and it does not round frC to 25 bits; the test build follows it there, which is the only place the port's build differs from it.
   So the Gekko's estimates and its 25-bit frC are as `port_fpu.h` measured them in Dolphin and as Dolphin models them, not verified on hardware; `fmadds` rounds once here, as the PowerPC architecture defines it, while `msl_math.c` follows Dolphin's rounding to double and then to single (item 10 found no input where the two differ).
   The console hangs in `PSMTXMultVecArray` for a count of 0 or 1 (its loop counter wraps); the port converts the 0 or 1 vector.
   The scripted title and plaza frames stay byte-identical between 32 and 64-bit, and with the FMA instruction or `fma_soft`.
   Against item 10's frames, the title is unchanged to field 600 and differs in 43 to 137 scattered pixels per frame from field 900 (at most 2 levels), and the plaza in 0 to 1,314 scattered edge pixels per frame (up to 121 levels, most in the upper right from field 4600): last-bit matrix differences moving triangle edges.
   The 60 fps plaza gate run still captures Mario after 100 frames with the same `setNextStage`.
   Cost, sampled over the scripted plaza run: the library was 0.25% (32-bit) and 0.33% (64-bit) of the game thread's CPU time, and is 0.24% and 0.29% with the FMA instruction, 0.66% and 0.69% with `fma_soft`.
   Not covered then: paired-single code outside the SDK library (JSystem's `J3DPSMtx*` and the like); item 12 covers it.
12. **JSystem's paired-single routines, and MSL's `fmadds` rounded once**: the rest of the game's paired-single maths is now the console's as well.
   Scanning every object of the decomp's split of the DOL (`build/GMSE01/obj`) for paired-single instructions finds, besides the SDK library of item 11, these routines that compute something:
   `J3DPSCalcInverseTranspose`, `J3DMtxProjConcat`, `J3DMTXConcatArrayIndexedSrc` (every joint's view matrix) and `J3DPSMtxArrayConcat` in `J3DTransform.cpp`;
   `J3DPSMulMtxVec` in `J3DTransform.hpp`, inline assembly that the DOL has only inside `J3DSkinDeform::deform` (the 3x4 form for positions, the 3x3 form for normals);
   `J3DModel::calcWeightEnvelopeMtx`; `J3DHermiteInterpolationS` in `J3DAnimation.cpp` (scalar fused multiply-adds on keys loaded as s16 through GQR5);
   and `MsVECMag2` and `MsVECNormalize` in `MathUtil.cpp`, which use the `frsqrte` estimate with no Newton step (good to about 1/4096).
   The port compiled the decomp's portable `#else` branches for them: unfused products and sums in another order, `1.0f / det` for the `fres` estimate and its two Newton steps, and an exact `1.0f / std::sqrt` for the two `MsVEC` routines.
   The rest need nothing: the copies (`J3DPSMtx33Copy`, `J3DPSMtx33CopyFrom34`, `J3DPSMtxArrayCopy`, JGeometry's `gekko_ps_copy12`) and `J3DScaleNrmMtx33` (products of two singles, one rounding either way) are exact in C, `J3DCalcZValue` is C in the DOL too, and the rest is not maths (`JUTException::getFpscr`, the GX FIFO writes `WriteMTXPS*` that `platform/gx` replaces, and `JASTrack`'s `OSf32tos8`, a GQR4 store in the audio code).
   The JMath helpers of later JSystem versions (`JMAVECScaleAdd`, `JMAFastSqrt`, `JMAMTXApplyScale`, ...) are not in this DOL (`marioUS.MAP`); `J3DPSMtx23Copy` and `J3DPSMTXConcatArray` are UNUSED there, and the s16 forms of `J3DPSMulMtxVec` have no caller.
   `platform/mtx/jsys_ps.inc` follows each routine's machine code instruction for instruction with `mtx_ps.inc`'s helpers and rounding (it is compiled into the same FMA-instruction and software namespaces of `mtx.cpp`), and `decomp-patches/fpu-01-paired-single-routines.patch` makes the `#else` branches call it through `src/port_ps.h`.
   `calcWeightEnvelopeMtx` keeps its C loop and calls `port_J3DWeightEnvelopeMix` for each mix matrix, which adds weight times the product into the DOL's accumulators with one fused multiply-add per element.
   The console runs the two array concatenations 2^32 times for a count of 0; the port does one matrix, the one the console writes first.
   `msl_math.c` now rounds `fmadds` once, as `mtx.cpp` does and as the PowerPC architecture defines it, through `port_fmas_soft` (item 11's `fma_soft`, moved to `src/port_fma.h` and shared with `mtx.cpp`; `fmaf` on hosts that are not x86), and `fnmsubs` and `fnmadds` negate the rounded result (-0 where the product equals the addend).
   So `tools/mslmath/check.sh` no longer needs a test-only rounding switch: its qemu test build differs from the port's build only in `frsqrte`.
   `tools/mtxmath/check.sh` has 14 more cases (the routines above, the in-place `J3DMtxProjConcat` and `MsVECNormalize` calls, counts of 1 and 2, one and two mix matrices):
   the `linux-32` and `linux-64` objects, each with the FMA instruction and with `port_fmas_soft`, give bit-identical results on 1,000,000 random inputs per case;
   and the DOL's own objects under `qemu-ppc` give bit-identical results to the qemu-arithmetic test build, on 1,000,000 inputs per case.
   For that, `ps2scalar.py` handles `psq_lx`, `lfsu`, s16 loads through GQR5 and a scratch register other than r12 (`calcWeightEnvelopeMtx` uses r12), `inline.py` lifts the two `J3DPSMulMtxVec` copies out of `J3DSkinDeform::deform`'s instruction words (and checks the DOL's `PSMulUnit01`), and the driver builds the `J3DModel` and `J3DModelData` fields `calcWeightEnvelopeMtx` reads.
   A mutated `jsys_ps.inc` (a Newton step dropped, a sum unfused, an exact square root, `fnmsubs` spelled as a subtraction, ...) fails the matching cases.
   `tools/mslmath/check.sh` gives bit-identical results between `linux-32` and `linux-64` and against the DOL's MSL objects under `qemu-ppc`, on 1,000,000 inputs per function.
   The scripted title and plaza frames stay byte-identical between 32 and 64-bit, and with the FMA instruction or `port_fmas_soft`.
   Against item 11's frames, the title is unchanged to field 600 and differs in 7,054 to 16,506 pixels per frame from field 900 (at most 10 levels: the lens flare's rings and halos, and scattered cloud and seagull pixels), and the plaza in 0 to 597 scattered edge pixels per frame (at most 3 levels, except 44 pixels of up to 19 levels at field 4100).
   The 60 fps plaza gate run still captures Mario after 100 frames with the same `setNextStage`.
   Cost, sampled over the scripted plaza run: the maths (MTX/VEC, MSL's trigonometry, these routines and the two J3D functions that call them) was 0.84% (32-bit) and 1.07% (64-bit) of the game thread's CPU time, and is 1.03% and 1.16%; most of the increase is `sinf`, whose fused multiply-adds now take `port_fmas_soft`'s exactness check.
   Still host C: the game's own C code, which MWCC compiled with fused multiply-adds wherever `fp_contract` allowed (JSystem and game units alike, `__frsqrte` refinements such as `JGeometry`'s included); the port builds it with `-ffp-contract=off`.
13. **Uninitialised light directions**: the game builds light objects on the stack and never sets their direction, so GX_LIGHT0 and GX_LIGHT1 carried stack garbage that differed between the word sizes (a NaN and values near 1e14 in the 32-bit build, zeros or small values in the 64-bit build).
   The frames matched only because those lights' angular coefficients are zero and llvmpipe's `max(NaN, 0.0)` gives 0; `uninit-01` zeroes the objects, so both builds load the same registers.
   This and the rest of a sanitizer and memcheck audit are in [DEVELOPMENT.md](DEVELOPMENT.md#memory-and-undefined-behaviour-checks).
   The scripted title and plaza frames are unchanged and byte-identical between 32 and 64-bit, and the 60 fps plaza gate run still captures Mario after 100 frames with the same `setNextStage`.
   Still word-size dependent: `platform/gx`'s `GXInitSpecularDir` normalises the half-angle vector with the host's `1.0f / sqrtf`, and the specular light's direction differs in the last bit between the builds for some inputs (`3f7970c0` against `3f7970c1`), without a pixel difference in these runs.
14. **Float-to-unsigned conversions**: the host's conversion of an out-of-range float to `u32` differs between the word sizes as well as from the console (5e9 gives 0 in the 32-bit build and 0x2A05F200 in the 64-bit build, -3e9 gives 0x80000000 and 0x4D2FA200, NaN 0x80000000 and 0), where MWCC's runtime gives 0xFFFFFFFF, 0 and 0.
   Every site where the DOL calls `__cvt_fp2unsigned` (70 calls; 44 source lines in the game, JSystem and `platform/gx`, plus `OSDumpContext`, which the port does not have) or `__cvt_dbl_usll` (`TUtil<f32>::mod`) now converts with the runtime's rule (`port_cvt_fp2unsigned`, `port_cvt_dbl_usll` in `port_fpu.h`; `fpu-02..04`), so both builds give the console's result for any input and the host's own for in-range values.
   Details are in [DEVELOPMENT.md](DEVELOPMENT.md#float-to-integer-conversions).
   The scripted title and plaza frames are unchanged and byte-identical between 32 and 64-bit, and the 60 fps plaza gate run still captures Mario after 100 frames with the same `setNextStage`.

15. **MSL's exponentials, `fmodf` and `sqrtf`**: the rest of the MSL maths the game calls is the console's too.
   `expf` and `powf` are MSL's `exponentialsf.c`, which is byte-matching in the decomp (the DOL's `powf` at 0x8033C9B8, `expf` at 0x8033D0F8).
   `expf` splits off the integer part and evaluates a degree-8 polynomial with fused multiply-adds, scaled by `__two_to_log2e_m1_tI`; `powf` is `__exp2f(y * __log2f(x))`, two inlines expanded three times (x > 0, and x < 0 with an odd or even integer y): a table-driven `log2` (`__log2_F`, `__one_over_F`) and a degree-9 `exp2` polynomial, all in single precision.
   They are less exact than glibc's: `powf(10, 2)` is 99.9999924 on the console, and glibc differs for 93% of `powf(d, 2)` with d in [-20000, 20000], 58% of `powf(r, k)` with r in [0, 1) and k in [0, 5], and 41% of `expf` in [-10, 10].
   Their special cases are MSL's own: `powf` of a negative base with a fractional exponent, or one above 2^31, is a NaN, `powf(+-0, y)` is -inf for every negative finite y, and `expf` is +inf above 88.72284 and 0 below -87.33655.
   The callers are JAudio's `JALCalc` (7 `powf`, 4 `expf`), the sound code's distances and volumes (`MAnmSound`, `MSHandle`, `MSound`: 9 `powf`) and `TEnemyMario`'s choice of path node (1 `powf`, the one gameplay caller).
   `std::fmodf` is an inline in MSL's header that the DOL never expands: the game calls its weak copy in `wireTrap.cpp` 21 times (`Koopa.cpp` 7, `BathtubPeach.cpp` 6, `koopajr.cpp` 4, `MapObjCorona.cpp` 3, `wireTrap.cpp` 1).
   It returns x when `|y| > |x|`, and otherwise converts `x / y` to `s64` through the runtime's `__cvt_dbl_usll`, back through `__cvt_sll_flt` (to double, then `frsp`), and subtracts with one `fnmsubs`, `-(y * n - x)` rounded once.
   So it gives -0 for an exact multiple (`fmodf(720, 360)`), where the host's exact `fmodf` gives +0, a different result when `x / y` rounds up to the next integer or is large, and -0 for `fmodf(0, 0)` (the Gekko's default NaN is positive, so the quotient saturates to 2^63).
   `JGeometry::TUtil<f32>::mod`, weak in `koopajr.cpp` and called 4 times each from `Koopa.cpp` and `koopajr.cpp`, is the same machine code, so `fpu-05` makes it call the same function.
   `std::sqrtf` (C's `sqrtf` in `hx_wiper.c` is the same inline) is `frsqrte` and three Newton-Raphson steps in double precision, `g = .5 * g * (3 - g * g * x)` with `3 - g * g * x` one `fnmsub`, then `(float)(x * g)`, for x > 0, and x itself otherwise.
   The DOL expands it at 45 call sites and calls the weak copy in `MAnmSound.cpp` from 4 more; it has no `fsqrt` (the Gekko has none), and nothing calls MSL's double `sqrt` (four steps) or `fake_tgmath.h`'s float `sqrt`.
   For every positive finite float the sequence gives the correctly rounded square root: with the Gekko's estimate (`port_gekko_frsqrte`) or an exact one, and with the `fnmsub` fused or not, it equals IEEE `sqrtf` on all 2^31 positive finite bit patterns.
   So `sms_msl_sqrtf` uses the host's `sqrtf` there, and gives the console's result elsewhere: zero, negative numbers and NaN come back unchanged (`sqrtf(-1)` is -1, where the host gives a NaN that would spread through a position), and +inf gives a NaN (`frsqrte(+inf)` is +0).
   The DOL's other 211 `frsqrte`s are not MSL's: `JGeometry::TUtil<f32>::sqrt` and `inv_sqrt` (one single-precision step), `MsSqrtf` (one double step), `JPASqrtf` (none), the game's own `__frsqrte` uses, the paired-single routines of items 11 and 12, and `GXInitSpecularDir`.
   The first four are game and JSystem code, which the port compiles with the Gekko's estimate but unfused, as item 12 left them.
   `port_compat.h` routes `expf`, `powf`, `fmodf` and `sqrtf`, and their `std::` spellings, to `sms_msl_expf`, `sms_msl_powf`, `sms_msl_fmodf` and `sms_msl_sqrtf` in `platform/misc/msl_math.c`, which follows the DOL's instructions for the first three as for the trigonometry (single-rounding `fmadds`, the DOL's table and constant bit patterns); Eclipse's `fmodf__3stdFff` trampoline (`tools/mods/gen_rawfn.py`) goes there too.
   No game object references the host's `sqrtf`, `powf`, `expf` or `fmodf` or contains a host square-root instruction any more.
   `tools/mslmath/check.sh` checks them, besides the trigonometry:
   the `msl_math.c.o` of `build/linux-32` and of `build/linux-64`, and a build of `msl_math.c` in each word size that uses `fmaf` and `fma` as FMA instructions (`-mfma`, the path hosts other than x86 take), give bit-identical results on 1,000,000 random inputs per function;
   MSL's `sqrtf` sequence with the Gekko's estimate equals `sms_msl_sqrtf` on all 2^32 bit patterns;
   and the DOL's `exponentialsf.o`, `runtime.o`, and the weak `std::fmodf`, `std::sqrtf` and `TUtil<f32>::mod` (their instruction words lifted out of `wireTrap.o`, `MAnmSound.o` and `koopajr.o` by `tools/mslmath/lift.py`) run under `qemu-ppc` give bit-identical results to the qemu-arithmetic test build on 1,000,000 inputs per function, the `sqrtf` sequence included.
   A mutated `msl_math.c` fails it (an `expf` or `powf` polynomial term unfused, the `fmodf` subtraction unfused, the host's `sqrtf` for negative numbers, the host's NaN sign in `fmodf`).
   The scripted title and plaza frames are unchanged against `1923ffd` in both word sizes and byte-identical between 32 and 64-bit: the new functions change no value those runs draw.
   The 60 fps plaza gate run still captures Mario after 100 frames with the same `setNextStage` in both builds.
   Cost, sampled over the scripted plaza run: the four functions are 0.12% (32-bit) and 0.14% (64-bit) of the game thread's CPU time, almost all in `powf`'s `exp2` and `log2`, where the host's functions took too few samples to register; all of `msl_math.c` went from 0.43% to 0.53% (32-bit) and from 0.50% to 0.67% (64-bit).
16. **The game's own fused multiply-adds, measured; the `frsqrte` helpers made exact**: MWCC fused `a * b + c` into `fmadds` and its kin throughout the game's and JSystem's C++ (`-fp_contract on`; 4,205 scalar fused instructions in the DOL outside the SDK and MSL), and the port compiles that code without contraction.
   No host compiler setting reproduces MWCC's choices closely enough to switch contraction on, so the game code stays uncontracted; the helpers whose Newton step MWCC fused now follow the DOL.
   MWCC's rule, from test cases compiled with the decomp's MWCC 1.2.5 and flags and checked against the matched objects:
   an addition or subtraction fuses a multiplication that is its direct operand within the same expression (`fmadds`, `fmsubs`; `c - a * b`, `c -= a * b` and `-(a * b - c)` give `fnmsubs`, `-(a * b + c)` gives `fnmadds`), as ISO C's `FP_CONTRACT ON` allows;
   never across statements (`t = a * b; t + c`), across an inlined call (`mul(a, b) + c`), through an explicit cast (`(float)(a * b) + c`), or in `-(a * b) - c`;
   in `a * b + c * d` the left product fuses and the right one is rounded first, unless the left one cannot fuse;
   and a product that MWCC's common-subexpression elimination shares is computed once and never fused: a product used again in the expression or in a later statement (`x * c` in `o[0] = x * c + y; o[1] = x * c + s`; `JPADrawExecRotBillBoard::exec`, whose four rotated corners share their products, has no fused instruction at all), and a square of anything but a register variable (`m->x * m->x`, `(x - o.x) * (x - o.x)` in `TVec3::distance`, `mtx[0][0] * mtx[0][0]`, `r * r` for a reference), where a square of a local or parameter (`a * a + c`) fuses.
   Double precision follows the same rule (`fmadd`; `(double)a * b + c` fuses and then rounds with `frsp`, `a * b + (double)c` with a float product does not). `J3DTransform.cpp` switches contraction off for part of the file (`#pragma fp_contract off`), as the SDK's MTX library is built.
   `tools/fmacount/fmacount.py` counts the fused instructions per function in the DOL's objects and in a host compilation of the same units (FMA instructions with `-mfma`, or calls to a software fma), matching functions by demangled name, over the functions the decomp matches; with `--mwcc`, it recompiles the decomp's source with its own MWCC command and extra flags instead.
   Two comparisons, over the units the decomp and the port share (the 41 whose source changed since the port's decomp commit left out):
   against the DOL, with each compiler's own inlining (542 units, 9,098 matching functions), the functions with a fused site on either side have the same count in 56.5% of cases with Clang 18 `-ffp-contract=on` (195 of 345; sites, as the sum of the smaller count over the sum of the larger, 59.5%), 49.5% with Clang `=fast` (52.9%) and 46.5% with GCC 13 `=fast` (47.9%); GCC 13 treats `=on` as off and fuses nothing.
   Most of those differences are inlining: `TCameraKindParam::inbetweenData` calls the out-of-line `CLBChaseConstantSpecifyFrame<f32>` 36 times in the DOL, where Clang inlines it and its fused site 36 times, and MWCC's `-inline auto` and Clang's heuristics pick different callees throughout.
   So the second comparison takes inlining out on both sides (MWCC `-inline off`, host `-fno-inline`), which measures the contraction rule alone on the same source (524 units; 20 do not compile with `-inline off`; 29,681 functions):
   Clang `-ffp-contract=on` agrees on 89.8% of the functions with a fused site (690 of 768) and 91.9% of the sites (2,099 of 2,283), Clang `=fast` on 67.5% (83.7%), GCC `=fast` on 79.1% (83.2%).
   Of Clang `=on`'s 184 differing sites, 138 are sites Clang fuses and MWCC does not (the shared products and squares above), and 46 sites MWCC fuses and Clang does not, mostly loops that MWCC unrolls (at `-O2` to `-O4` alike; each copy has its sites).
   `=fast` is worse because it fuses across statements and inlined calls; Clang's `=fast` leaves a shared product unfused, as MWCC does, while Clang's `=on` and GCC's `=fast` fuse it into each of its sums.
   A wrong choice costs as much as a missing one: a fused `a * b + c` differs from the twice-rounded one for about 22% of random inputs (positions up to 1000 times a factor up to 1, plus a position), whichever of the two the console computed.
   No setting reaches the 95% that would make contracting the game code clearly closer to the console, the port's compiler on Linux and Windows (GCC) cannot contract within expressions only, and a build that contracts would also need every x86 CPU without an FMA instruction to get the same results, through a software fma at each site; so nothing changed there.
   What would reproduce the rule: a source-to-source pass (Clang tooling) or a compiler plugin that applies MWCC's rule, the common-subexpression exclusions included, and writes each fused site as an explicit call to `port_fmas_soft` or the FMA instruction, as `platform/mtx` and `msl_math.c` do.
   The `frsqrte` helpers are the exception, since their fused step is known exactly: `JGeometry::TUtil<f32>::sqrt` and `inv_sqrt` (153 inlined copies in the DOL, weak copies in `boid.o`) compute `root = frsp(frsqrte(mag))`, then `0.5 * root * fnmsubs(mag, root * root, 3)` (times `mag` for `sqrt`), and `MsSqrtf` (39 inlined copies, a weak copy in `cameragc.o`, and the same sequence spelled out in `TMapObjBase::getDistanceXZ`) one step in double precision, `(float)(x * (0.5 * g * fnmsub(x, g * g, 3)))`.
   Unfused, `TUtil`'s results differ for 3.4% (`sqrt`) and 3.5% (`inv_sqrt`) of the positive normal floats; `MsSqrtf`'s for none of them (the double step has enough precision), but it now follows the DOL all the same.
   `decomp-patches/fpu-06` makes them call `sms_jg_sqrtf`, `sms_jg_inv_sqrtf` and `sms_ms_sqrtf` in `platform/misc/msl_math.c`, which follow the DOL's instructions with `msl_math.c`'s single-rounding `fnmsubs` and `fma()`.
   `JPASqrtf` needs nothing: its C body, `x * (f32)__frsqrte(x)`, is already the DOL's `frsp` of the estimate and one `fmuls`.
   `tools/mslmath/check.sh` checks the four:
   the `linux-32` and `linux-64` objects and the FMA-instruction builds give bit-identical results on 1,000,000 inputs per function,
   and the DOL's weak copies of `TUtil<f32>::sqrt`, `inv_sqrt` and `MsSqrtf` and its `JPASqrtf`, lifted out of `boid.o`, `cameragc.o` and `JPAMath.o`, give bit-identical results under `qemu-ppc` to the qemu-arithmetic test build on 1,000,000 inputs per function (an unfused `TUtil` step fails with about 8,500 differences in each); `lift.py` now reads sections through a file, since `objcopy` writes nothing to a pipe.
   The scripted title and plaza frames are unchanged against `d3f1f3a` in both word sizes and byte-identical between 32 and 64-bit, and the 60 fps plaza gate run still captures Mario after 100 frames with the same `setNextStage` in both builds.
   Cost, sampled over the scripted plaza run: the three functions, which the game now calls instead of expanding inline, are 0.06% (32-bit) and 0.03% (64-bit) of the game thread's CPU time, and the run's total CPU time is within 2% of `d3f1f3a`'s (205 against 202 s in 32-bit, 193 against 189 s in 64-bit, within the runs' spread).

17. **The platform layer in SSE on 32-bit x86**: on the `fma-rewriter` branch (MWCC's contraction reproduced in the source), the scripted plaza frames differed between 32 and 64-bit in one pixel of Mario at field 5000, by one level of red.
   A GX trace of that frame (`SMS_GX_TRACE_FRAME`, with a temporary hash of each draw's vertices, matrices, lights and XF registers added, and `SMS_GX_TRACE_PROBE` at the pixel) had every value equal but the direction of GX_LIGHT2, the specular light, which differed in the last bit (`BE8538D7 3F371CD0` against `BE8538D8 3F371CD1`), and the probe showed the draw of Mario's body that channel 1 lights with it writing the pixel one level apart.
   That direction is the half-angle vector `GXInitSpecularDir` normalises with `1.0f / sqrtf(...)`: `platform/gx`, like the rest of the platform layer but `msl_math.c` and `mtx.cpp`, was built without `-mfpmath=sse`, so the 32-bit build evaluated it in x87 extended precision and rounded once, and the 64-bit build in SSE; the function compiled alone gives each build's bits from the traced light's normal.
   It is item 13's last-bit difference; `main` has it too (the same code and flags), latent: it moves no pixel of the scripted frames here.
   `sms` and `sms_gx` are now built with `-msse2 -mfpmath=sse` on x86, which are the x86-64 compiler's defaults: the 64-bit executable's loadable image is byte-identical to before but for its build ID, and the 32-bit build computes what it does.
   On `main`, the scripted title and plaza frames stay byte-identical between 32 and 64-bit and unchanged against `43cb779` in both word sizes, and the 60 fps plaza gate run still captures Mario after 100 frames with the same `setNextStage` in both builds; `tools/mtxmath/check.sh` still passes (`mtx.cpp` was already built this way).
   The console computes the vector differently again (the SDK's `GXInitSpecularDir` fuses `vx * vx + vy * vy` and takes MSL's `frsqrte` square root); following it would change both builds' frames, and is left for later.
   (Item 18 found this wrong: the DOL's `GXInitSpecularDir` has no fused instruction, and its square root equals the host's, so the SSE build already computes what the console does.)
   The trace also shows 35 draws of that frame in another order in the two builds (the same draws, whose pixels do not depend on the order), not traced.

18. **The SDK's GX arithmetic, and the audio code's quantised store**: the last two places where the port's own code stood in for console float arithmetic with the host's.
   `platform/gx` implements the SDK's GX functions itself, and called the host's `cosf`, `sinf` and `sqrtf`; on the console they are SDK code built by MWCC, calling MSL.
   Of the GX functions that compute in floats, `marioUS.MAP` links `GXInitLightDistAttn`, `GXInitSpecularDir`, `GXProject` and `GXDrawSphere`; `GXInitLightSpot`, `GXInitLightDir`, `GXInitSpecularDirHA`, `GXDrawCylinder`, `GXDrawSphere1` and `GXInitFogAdjTable` are UNUSED there and the game calls none of them, so they keep the host's maths.
   The DOL's GX objects (the decomp's split of the original) contain no fused multiply-add: the SDK was built without contraction, and each product, sum and quotient is one single-precision operation.
   - `GXInitLightDistAttn` (`LightUtil.cpp`, `GX_DA_STEEP`) makes the port's operations in the port's order: already exact.
   - `GXInitSpecularDir` (`LightUtil.cpp`, the specular light) sums the three squares in the port's order and takes MSL's inline `sqrtf` (`frsqrte`, three Newton-Raphson steps in double with the products and the difference rounded separately, then `frsp`), which equals IEEE `sqrtf` on all 2^31 positive finite floats with the Gekko's estimate (checked here, as item 15 did for the fused form) and returns 0 for 0: already exact for every input whose sum of squares is finite.
     Only a sum that overflows, a component above about 1.8e19, differs (MSL's sequence gives a NaN there, the host's `1.0f / sqrtf` 0), and the game's directions are unit vectors.
   - `GXProject` (`TMario`'s screen position in `MarioDraw.cpp`) sums the eye-space point as `m[0][3] + (m[0][2] * z + (m[0][0] * x + m[0][1] * y))` and the window coordinates as `vp[2] / 2 + (vp[0] + wc * (xc * vp[2] / 2))`, `vp[3] / 2 + (vp[1] + wc * (-yc * vp[3] / 2))` and `vp[5] + wc * (zc * (vp[5] - vp[4]))`; the port's spelling associated them differently and gave another result for 36% of random inputs.
   - `GXDrawSphere` (`TSky::perform`'s background sphere, 8 x 16) calls MSL's `sinf` and `cosf`; the host's changed 678 of the sphere's 1,632 vertex floats.
   `platform/gx/src/gx_sdk_math.h` now holds the four functions' arithmetic in the DOL's order, with `sms_msl_sinf` and `sms_msl_cosf` (`msl_math.c`), and `gx_api.cpp` calls it.
   `sms_gx` is built with `-ffp-contract=off` on every host: on x86 its code is identical (nothing fuses without `-mfma`), but GCC's default contraction where an FMA instruction exists (arm64) changes 57% of `GXProject`'s results.
   The rest of the float code in the GX functions the DOL links agrees for the game's inputs and is unchanged:
   `GXSetViewportJitter` computes the z scale as `16777215 * far - 16777215 * near` where the port computes `(far - near) * 16777215`, the same for every caller's near 0 and far 1;
   `GXSetFog`, `GXSetIndTexMtx`, `GXInitTexObjLOD`, `GXGetNumXfbLines` and `GXSetDispCopyYScale` make the same operations, and their conversions (`fctiwz`, `__cvt_fp2unsigned`, item 14) agree in range;
   `GXGetYScaleFactor` searches for the scale another way than the SDK and returns another value when the XFB and EFB heights differ, but the US game always asks for 448 lines of 448, where both give 1.
   `GXDrawSphere` still replaces a size of 0, which the SDK divides by, and leaves out the texture coordinates the SDK adds when `GX_VA_TEX0` is enabled (the sky clears the vertex descriptor first).
   `tools/gxmath/check.sh` checks the header:
   built with `sms_gx`'s flags for 32 and 64-bit, and again with `-mfma`, it gives bit-identical results on 1,000,000 random inputs per function and 517 sphere sizes;
   and the DOL's four functions, lifted out of `GXLight.o`, `GXTransform.o` and `GXDraw.o` by `tools/mslmath/lift.py` (which now relocates references to an object's `.bss`) and linked with its `trigf.o`, give bit-identical results under `qemu-ppc` (two NaNs count as equal).
   `GXDrawSphere` writes its vertices to the write-gather pipe's one address; `tools/gxmath/fifo.py` turns its `lis r30,0xCC01` into the address of a buffer and each `stfs fN,-0x8000(r30)` into `stfsu fN,4(r30)`, so the floats land in order, and the driver's stubs report no texture coordinate.
   A mutated header fails it (the host's `sinf`, the old `GXProject` association, a sum or product regrouped).
   `JASTrack`'s `OSf32tos8` is the other gap.
   A scan of every object of the DOL for quantised loads and stores through GQR1 to GQR7 (by opcode: `objdump` decodes `psq_st` as a VSX instruction) finds JASTrack's two `psq_st`s (`TTrack::updateTrack` and `updateTrackAll`), `J3DHermiteInterpolationS`'s s16 loads (item 12) and the THP decoder's GQR5 loads and GQR6 stores in its IDCT, which the port's host decoder replaces (`thp-01`); the decomp defines no other `OSf32to*` or `OS*tof32` helper.
   `OSf32tos8` is `lfs` and `psq_st f1, 0(out), 1, 4`: GQR4, which `OSInitFastCast` sets to 0x00060006 (s8, scale 0) in the audio thread (`AudioThread::audioproc`, where the sequencer runs), as `J3DSys::drawInit`, `THPInit` and `TApplication::initialize` set it in theirs.
   The track stores `128 * unk110` (timed parameter 17) as an s8, and passes a positive result as bus 1's mixer delay and a negated negative one as bus 0's (`unk5A`, the third argument of `DSPBuffer::setMixerVolume`).
   The decomp's inline has no body outside MWCC, so the host build read an uninitialised byte (GCC stored 0 whatever the value), where the console saturates: 1.0 gives 127, not the 128 a C cast would wrap to -128.
   `fpu-07` gives the inline a host body, `port_gekko_quantize` in `port_fpu.h`: the value as a single times 2^scale in single precision, saturated to the type's range (u8, u16, s8, s16) and converted towards zero, a NaN storing 0.
   That is Dolphin's interpreter's model of the quantised store (`ScaleAndClamp`); there is no console or Dolphin here to measure it, so the rounding direction and the NaN case are Dolphin's, as `port_gekko_frsqrte`'s tables are (item 11).
   `tools/quantize/check.sh` compares it with a separate statement of that model on all 2^32 float bit patterns for OSf32tos8's s8, and on 200,000 values for every type and scale, in 32-bit SSE, 64-bit and 32-bit x87 builds: no difference, and the same checksum.
   The result cannot change what the port plays: its mixer ignores the delay bytes (`platform/audio/dsp_mixer.cpp`), and in the scripted plaza run with audio `unk110` was 0 at every one of more than 440,000 `updateTrack` and `updateTrackAll` calls (counted under gdb), so the byte was 0 either way.
   Recorded with `SMS_AUDIO_WAV` over that run (title, file select and plaza, 90.8 s), the audio is byte-identical to `f290333`'s in both word sizes, and identical between them.
   The scripted title and plaza frames (with audio off, as the reference runs have it) are unchanged against `f290333` in both word sizes and byte-identical between 32 and 64-bit.
   In the plaza run `GXDrawSphere` is never called, and `GXProject` (about once a frame, 4,812 calls) only places `TMario`'s occlusion probe, which reads back the pixel at the projected point's integer coordinates, so a last-bit change moves nothing unless it crosses a pixel boundary.
   The 60 fps plaza gate run still captures Mario after 100 frames with the same `setNextStage` in both builds.
19. **The title's attract loop with `SMS_AUDIO=0`, a fault at 0x1850 that was not 64-bit**: left at the title with the regression runs' settings, the 64-bit build faulted at address 0x1850 after about 575 s, in `JAIBasic::stopSeq` from `MSound::exitStage`; the 32-bit build faults at the same place (field 23,982, the eighth time the title scene ends), only later in wall-clock time.
   `SMS_AUDIO=0` left the AI DMA engine idle, so JAudio's audio thread never ran, and with it the sequencer, which is what frees a sequence the game stops: `TTrack::stopSeq` only marks it, and `TTrack::rootCallback` closes it and frees its root slot.
   One of the eight root slots holds a sequence that never stops, and each title cycle's music (`MSD_BGM_MAIN_TITLE`) kept another; on the eighth start `setSeqData` found none free and `checkReadSeq` released the sound, which is retail's path for that case and leaves `MSBgm`'s handle pointing at the released sound.
   The next `exitStage` stopped that sound, whose parameter block pointer is null, and wrote its `mUpdateData` at offset 0x1850.
   On the console the sequencer always runs, so the slots are freed; players with `audio = off` would have met the fault after eight music changes.
   `SMS_AUDIO=0` now drops only the output (`platform/audio/ai.cpp`): the DMA engine keeps its clock (retraces under `SMS_VI_DETERMINISTIC`, the host clock otherwise), so the game's audio runs as it does with sound, and THP movies no longer stall with it.
   The slots then stay at the one permanent sequence through 36,000 fields, captures at 13 fields up to 30,000 (the title, its attract cycles and the intro that follows them) are byte-identical between the builds, and the regression checks pass unchanged.
20. **Eclipse's memory card files**: the card BSE's first boot saves differed between the builds in 138 bytes of `better_sunshine_engine.dat`, 14 of `better_sunshine_moveset.dat` and 442 of `super_mario_sunshine.dat`.
   The three modules copy their banner and icons to the card from BTI files built into their code, a fixed 0xE00 and 0xA00 bytes, but five of the six files are shorter (up to 0x1A2 bytes), so the copy read past them into whatever each build put next.
   Those bytes are palette entries no pixel uses; `fixup_sources.py` sizes the arrays to the copy, so the rest is zeros and both builds write the same files ([ECLIPSE.md](ECLIPSE.md#status-2026-10-01)).

Windows x64 uses MSYS2 MINGW64 and is the Windows launcher target. LLP64 uses 32-bit `u32`/`s32` spellings matching the other 64-bit builds; operator new uses the Windows size_t ABI, the PE image stays below 4 GiB, and each game thread enters an explicit low stack with Windows TEB stack bounds. winpthreads ignores the address passed to `pthread_attr_setstack`, so a Windows-specific entry/exit trampoline handles this. `sms_windows_stack_test` checks low locals, stack bounds, normal returns, repeated thread exit/join, and restoration of the host stack without a ROM. Actual Windows gameplay coverage is recorded separately from compilation/runtime checks.

## Where things stood before the work (measured 2026-09-24)

- `SMS_ARCH=64` already exists in `CMakeLists.txt` as a compile-only fallback (it adds `-fno-pie` so globals sit below 4 GiB); nothing claimed it ran.
- **Compile:** every translation unit compiles 64-bit.
- **Link:** fails on 24 references to two functions whose declaration and definition spell the same GameCube type differently:
  `SMS_CreatePartsModel(char*, unsigned long)` in `MarioUtil/ModelUtil.hpp` against `u32` in `ModelUtil.cpp`, and `Kernel::probeStart/probeFinish(s32, …)` in `JASProbe.hpp` against `long` in `JASProbe.cpp`.
  MWCC and the 32-bit build give both spellings one mangled name; LP64 does not (`long` is 64-bit, the port's `u32` stays 32-bit).
  With those two declarations aligned locally, the build links into an x86-64 executable.
- **Run:** it boots the platform, opens the disc and the GL context, and crashes in the first archive load (`SMSLoadArchive` → `JKRDvdRipper::loadToMainRAM` → `JKRDecomp::checkCompressed`) on address `0xc7bf9a80`: the low half of a host stack address that went through a 32-bit integer.
- **Size of the problem** (g++ `-Wpointer-to-int-cast`/`-Wint-to-pointer-cast` over every game unit, `-fpermissive` otherwise hides them):
  417 sites where a pointer passes through a 32-bit integer (290 pointer→int, 127 int→pointer).
  The largest groups: `EventWatcher.cpp` 96 and `NpcEvent.cpp` 33 (script VM slots hold pointers as `u32`), `J3DAnmLoader.cpp` 90 and the other J3D loaders, JKernel heaps and archives 39, JAudio 28, `PacketUtil.cpp` 11, `spcinterp.cpp`/`liveinterp.cpp` 10 each.
- **Resource structs laid over file data:** 191 `JSUConvertOffsetToPtr` sites (J3D model/material/shape/joint/cluster/animation loaders, JAudio bank and wave-system parsers) rewrite 32-bit file offsets into `T*` fields in place, e.g. `J3DVertexBlock`'s `void* mpVtxPosArray` at file offset `0x0C`.
  On 64-bit those fields are 8 bytes, so the struct no longer lines up with the file.
  `JKRArchive`'s `SDIFileEntry::mData` is the same pattern for RARC file entries.
- `JSUConvertOffsetToPtr` adds through `(s32)`; with game memory at `0x80000000` that sign-extends to an invalid 64-bit address.
- Hard-coded byte offsets into objects (`(u8*)this + 0x…`) are rare (5), so class layouts growing with 8-byte pointers is mostly safe.

## Approach: 32-bit game addresses in a 64-bit process

Rewriting every resource format into a 64-bit layout would touch every loader and every converter in `platform/endian`.
Instead, keep every address the game can see below 4 GiB, so a pointer that goes through a `u32` comes back unchanged, and give the few struct fields that overlay file data a 4-byte pointer type.

1. **Everything the game touches lives below 4 GiB.**
   MEM1 is already mapped at `0x80000000` and static data is below 4 GiB (`-fno-pie`).
   Still needed: host thread stacks (each `OSThread`'s host pthread and the thread running `SMS_main`) allocated with `MAP_32BIT` (Linux) or a low `VirtualAlloc` (Windows), and an audit of host allocations handed to game code (ARAM staging, DVD buffers, GX FIFO and display-list memory, THP buffers).
   A debug check in `platform/` can abort on any pointer above 4 GiB that reaches a game-visible slot.
2. **Pointer ↔ integer casts zero-extend.**
   Casts through unsigned 32-bit types already round-trip below 4 GiB.
   Casts through `s32` (as in `JSUConvertOffsetToPtr`) sign-extend game addresses at `0x80000000` and above, so each int→pointer site that goes through a signed type needs an unsigned or `uintptr_t` spelling.
3. **Resource structs keep their 4-byte fields.**
   Fields that overlay file data (the J3D loader blocks, JAudio bank/wave tables, RARC file entries, plus any other format found in the audit) become a 4-byte pointer type (`T*` on the GameCube and 32-bit builds, a 32-bit handle that converts to and from `T*` on 64-bit).
   The endian converters keep working on the same layout.
4. **Pointer-sized fields that do not overlay files may grow.**
   Ordinary classes (`TMario`, managers, J3D runtime objects) are only ever built by `new`, so 8-byte pointers in them are fine, except where code uses a hard-coded offset or size (the 5 raw-offset sites, `sizeof` checks, `memcpy` of a fixed byte count, and struct arrays read from files).
5. **Windows 64-bit is LLP64** (`long` stays 32-bit), so it needs its own pass over `long`-typed pointer casts, but not over the declaration mismatches above.

## Where each change goes

The rules in [DEVELOPMENT.md](DEVELOPMENT.md#where-a-fix-goes) decide this.

- **Decomp (`sms-english`):**
  The two declaration/definition mismatches are decomp inaccuracies (the declaration should spell the definition's type), invisible to MWCC, so they are fixed there and verified with `ninja changes_all` and the DOL hash.
  The same goes for any other mismatched spelling the 64-bit build finds.
- **The pointer-width adaptations (steps 2–3) are a decision to make.**
  They are PC-specific, which today means `decomp-patches/`.
  But they touch roughly 400 sites across ~40 files, and a patch set that size would break every time the decomp touches those files.
  - Option A, port patches: follows today's rule; large, fragile patches (`ptr64-*`).
  - Option B, neutral portability types in the decomp: a pointer-in-integer type and a 4-byte field type that are exactly `u32` and `T*` under MWCC, so the DOL stays byte-identical, and each site states its intent once.
    This changes the decomp for portability rather than correctness, so it needs an explicit exception to the current rule.
  Recommendation: B for the recurring patterns (script VM slots, `JSUConvertOffsetToPtr`, loader block structs, heap/archive arithmetic), A for one-off sites.
- **Platform (`platform/`, port-owned):**
  Low-address stacks and allocations (step 1), the 4 GiB check, and any 64-bit handling in `platform/gx`, `platform/os`, `platform/dvd`, `platform/ar` and `platform/endian`.
- **Build:** CMake keeps `-m32` as the default where multilib exists and selects 64-bit with `SMS_ARCH=64`; `build.sh` and `run.sh` take `SMS_ARCH=64` and keep each word size in its own folder (`build/linux-32/`, `build/linux-64/`); nothing changes for existing 32-bit builds.

## Keeping 32-bit unchanged

- Every change is either `#if` on pointer width or a type that is identical on 32-bit, so the 32-bit objects compile to the same code.
- Before each merge: the 32-bit build's scripted runs (plaza, beach, the 20-movie sweep) must reproduce their reference frames byte-for-byte, and the decomp's DOL hash must stay `a6782903ef79d4196c8489ecb1b57decb5b3728f`.
- The 64-bit build is compared against the 32-bit references with the same scripts (`SMS_VI_DETERMINISTIC`, `SMS_AUTOPRESS`, `SMS_SHOTS`); identical frames are the target, and every difference gets a root cause.

## Milestones

1. **Links:** fix the two declaration mismatches in the decomp.
2. **Boots to the Nintendo logo:** low-address thread stacks; fix the signed casts on the boot path (`JSUConvertOffsetToPtr`, JKRDecomp/JKRDvdRipper, JKRExpHeap).
3. **Title screen and movies:** JKR archives (RARC entries), JUT/J2D screens, BMG, THP buffers, JAudio bank/wave parsers (sound on).
4. **File select and the airstrip:** J3D loaders (block structs, animation loaders), `PacketUtil`, collision.
5. **Delfino Plaza playable:** script VM (`EventWatcher`, `NpcEvent`, `spcinterp`, `liveinterp`), pollution, NPCs; frames match the 32-bit references.
6. **Every stage and movie:** run the full stage list and the movie sweep in 64-bit.
7. **Windows 64-bit** (MSYS2 `mingw-w64-x86_64`), then macOS/ARM64 once the GL layer runs there.
8. **Make 64-bit a supported build:** docs and build scripts; 32-bit stays available.

## Open questions

- Option A or B for the pointer-width changes (see above).
- Whether game memory stays at `0x80000000` (identity with retail addresses, but needs every signed cast fixed) or moves below 2 GiB (signed casts work unchanged, but retail addresses in traces and the lockstep tracer no longer line up).
  Recommendation: keep `0x80000000`.
