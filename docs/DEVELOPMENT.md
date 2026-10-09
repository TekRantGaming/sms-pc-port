# Developer guide

How the port is put together and where changes go. To build and play, see the [README](../README.md) and [BUILD.md](../BUILD.md).

## Where a fix goes

- **Decompilation bugs go in the decomp** (`sms-english`, the `decomp/` submodule): source that does not do what retail does (a wrong member, a swapped argument, a wrong constant) is fixed there, verified with `ninja changes_all` and the DOL hash, and picked up here by bumping the submodule.
  A decomp bug can show up only on PC (MWCC and g++ read the same wrong source differently), and it is still a decomp bug.
- **Never patch around incomplete decompilation.**
  A bug that comes from the decomp not doing what retail does (a non-matching function, a missing branch, a wrong reconstruction) is fixed only in the decomp, never in `decomp-patches/` or `platform/`, even as a stopgap.
- **Decomp matching progress comes first.**
  A decomp fix lands in `sms-english` only when it keeps every function's match (no regression in `ninja changes_all`, and the DOL hash unchanged).
  When the only correct form found so far costs match percentage, the decomp keeps its matching form with a `TODO` naming the behaviour difference, and the port lives with that difference until a matching form is found.
  Example: `TConductor::isBossDefeated` (98.8%) lacks retail's `default:` arm, so maps other than 2 and 3 fall off the end; g++ then runs the Gesso check for them where retail runs the Hinokuri one.
- **Pointer-size neutral spellings go in the decomp** (the one kind of PC-motivated change the decomp takes): where the game keeps pointers in 4-byte slots, the decomp spells that so MWCC's output is unchanged and a 64-bit host keeps the layout: `PTR32(T)` (exactly `T*` in the decomp's `dolphin/types.h`) for pointer fields of structs laid over file data, `sizeof` instead of byte counts, and `u32` instead of signed ints in int-to-pointer casts.
  Each such commit keeps the DOL hash and every function's match; one-off 64-bit adaptations that cannot be spelled neutrally are `ptr64-*` patches here (see [64-BIT.md](64-BIT.md)).
- **PC-specific fixes go in `decomp-patches/`**: byte order (`endian-*`), host compiler leniency (`0001`–`0010`, `0015`, `0018`, `ret-02..03`), host services (`thp-*`, `audio-*`), port-only features (`port-*`, `SMS_*` switches). Each patch starts with a `Reason:` line saying why it cannot live in the decomp.
  A patch never corrects the decomp's behaviour; it only adapts retail's behaviour to the PC.
- **Emulation of the hardware goes in `platform/`** (GX, DVD, OS, audio), never in game source.
- Example: the sun-glass tint that stopped part-way down the screen was a decomp bug (`TOrthoProj`'s reconstructed constructor stored its last two edges swapped), so it was fixed in `sms-english` and verified against retail, not patched here.

## How the build works

`build.sh` configures `build/<os>-<arch>/` with CMake ([BUILD.md](../BUILD.md#manual-cmake-build) has the options); everything it generates stays in that folder.

- `-DSMS_ARCH=32` compiles with `-m32` (Linux: `gcc-multilib g++-multilib`; Windows: MINGW32).
  `-DSMS_ARCH=64` is a native 64-bit build that keeps game memory, static data and thread stacks below 4 GiB ([64-BIT.md](64-BIT.md)); it is what macOS builds.
- Default build type `RelWithDebInfo` = `-O2 -g` for all targets (the fall-off-the-end functions got explicit returns, `ret-*` patches).
- `SMS_COMPILER_CACHE=/absolute/path/to/sccache` (or `-DSMS_COMPILER_CACHE=...`)
  enables compiler caching without changing the selected compiler or build flags.
  Windows CI uses sccache's GitHub Actions backend and reports cache statistics.
  The private 32-bit Windows cross compiler uses relative paths that both native
  sccache and MSYS GCC can read; response-file compilations and paths on another
  drive bypass caching. Link commands keep their existing path conversion.
- The 32-bit Linux build compiles against the amd64 SDL2/EGL headers and links the i386 runtime libraries (`/usr/lib/i386-linux-gnu/libSDL2-2.0.so.0`, `libEGL.so.1`) directly, so no `:i386` `-dev` packages are needed.
- The decomp keeps the game in `src/` and `include/`, and each library in `libs/<name>/src` and `libs/<name>/include` (`dolphin`, `JSystem`, `THPPlayer`, `PowerPC_EABI_Support`, `TRK_MINNOW_DOLPHIN`, `OdemuExi2`), as upstream `doldecomp/sms` does.
  The game sees the headers through the same roots in `configure.py`'s order (`include`, then each `libs/<name>/include`), except MSL's C and C++ headers: the port uses the host's.
- Game units: every `.c`/`.cpp` in `decomp/src`, `decomp/libs/JSystem/src` and `decomp/libs/THPPlayer/src` (588 units, listed by `tools/gen_sources.py`, which reads `configure.py` for the ones that get `SMS.pch`), plus the SDK's pad clamp and the decomp's THP decoder (`platform/thp/thp.cmake`); the rest of the SDK, MSL, the runtime, MetroTRK and OdemuExi2 are left out.
- `decomp-patches/` are applied in name order to copies under `build/<os>-<arch>/patched/`, at the same paths as in the decomp (`patched/src/...`, `patched/libs/JSystem/include/...`); `decomp/` itself is never modified.
- Flags: `-std=gnu++03 -fno-gnu-keywords -fpermissive -fno-strict-aliasing -fwrapv -finput-charset=UTF-8 -fexec-charset=CP932 -DGEKKO -DTARGET_PC -DVERSION_GMSE01 -DBUILD_VERSION=2 -DNDEBUG=1`, host libc/libstdc++ instead of MSL, `-include src/port_compat.h`.
  String literals are Shift-JIS, as in the MWCC build (archive object names are matched against them); clang has no CP932 execution charset, so on macOS the sources are mirrored as CP932 first (`tools/darwin_cp932_mirror.py`).
- The game's global `operator new/delete` (JKRHeap) are renamed in `libsms_game.a` with `objcopy --redefine-syms` (`llvm-objcopy` on macOS), so only game code allocates from JKR heaps; libstdc++ and `platform/` use the host allocator.
- `rand()` is MSL's (RAND_MAX 32767, same LCG) via `port_compat.h`; glibc's 2^31 range overflows the game's `1.f / (RAND_MAX + 1)`.
- The game's MSL maths is the console's (`sinf`, `cosf`, `tanf`, `atanf`, `atan2f`, `acosf`, `expf`, `powf`, fdlibm's `atan2`, and the header inlines `std::fmodf` and `sqrtf`) via `port_compat.h` and `platform/misc/msl_math.c`, which follows the DOL's matched objects instruction for instruction; `tools/mslmath/check.sh` compares it with those objects under `qemu-ppc` and between the 32 and 64-bit builds ([64-BIT.md](64-BIT.md), items 10 and 15).
- The SDK's matrix library (`PSMTX*`, `PSVEC*`, `C_MTX*`) is `platform/mtx`, which follows the DOL's paired-single and C routines instruction for instruction, with single-rounding fused multiply-adds; `tools/mtxmath/check.sh` compares it with the DOL's objects under `qemu-ppc` and between the 32 and 64-bit builds ([64-BIT.md](64-BIT.md), item 11).
- The float arithmetic of the SDK's GX functions the DOL links (`GXInitLightDistAttn`, `GXInitSpecularDir`, `GXProject`, `GXDrawSphere` with MSL's `sinf` and `cosf`) is `platform/gx/src/gx_sdk_math.h`, in the DOL's order; `sms_gx` is built without contraction on every host, and `tools/gxmath/check.sh` compares it with the DOL's objects under `qemu-ppc` ([64-BIT.md](64-BIT.md), item 18).
- The Gekko's quantised integer store (`psq_st` through a GQR, the audio code's `OSf32tos8`) is `port_gekko_quantize` in `port_fpu.h` (`fpu-07`); `tools/quantize/check.sh` checks it ([64-BIT.md](64-BIT.md), item 18).

## Tools

| Tool | Use |
| --- | --- |
| `tools/common.sh` | shared by `build.sh`, `run.sh` and `clean.sh`: host detection, `SMS_ARCH`, build folder, `rom/` lookup, moving files out of older layouts |
| `tools/bundle_disc.py` | packs the disc's files into `sms-standalone` (or `SMS.app`'s `disc.gcm`) |
| `tools/make_mac_app.sh`, `tools/extract_icon.py` | assemble and sign `SMS.app`; the app / `.exe` icon from the disc's memory-card icon |
| `tools/regress/regress.py [--record] CHECK...` | the regression checks (below): scripted headless runs hashed against `tools/regress/baseline.txt` and between the 32 and 64-bit builds |
| `python3 tools/regress/popo.py --disc PATH` | Puffer launch, self-hit rejection and Petey impact at 30/60/120 fps in both word sizes; [fixture and findings](PUFFER-FRAMERATE.md) |
| `python3 tools/regress/self_damage.py --disc PATH` | diagnostic stage samples that report self-directed damage and observed collision coverage; [scope and limitations](PUFFER-FRAMERATE.md#broader-self-collision-audit) |
| `tools/run_capture.sh SECS FIELDS` | headless run + captures + retail comparison |
| `tools/shots.py`, `tools/contact.py OUT.png FIELDS...` | convert captures to PNG in `build/shots/` and compare with retail; contact sheet of captures |
| `tools/gdbrun.sh` | backtrace at the first fatal signal |
| `tools/hangdump.sh N` | every thread's stack after N seconds |
| `tools/trace_resolve.py`, `tools/trace_compare.py` | lockstep tracing against retail (`platform/trace/README.md`) |
| `tools/mkpatch.sh` | write a `decomp-patches/` patch from edited copies |
| `tools/warn_scan.py`, `tools/syntax_check.py` | one g++ warning class over all units; `-fsyntax-only` over all units |
| `tools/gen_sources.py`, `tools/gen_stubs.py` | regenerate `cmake/decomp_sources.cmake` and `platform/sdk_stubs.cpp` |
| `tools/fpprobe/` | PowerPC FPU behaviour probe (a DOL run in Dolphin) |
| `tools/mslmath/`, `tools/mtxmath/`, `tools/gxmath/` (`check.sh`) | MSL's maths, the MTX/VEC and JSystem paired-single routines, and the SDK's GX arithmetic against the DOL's own objects under `qemu-ppc`, and between the 32 and 64-bit builds |
| `tools/quantize/check.sh` | the quantised store model (`port_gekko_quantize`) on every float |

The Linux debugging tools (`gdbrun.sh`, `hangdump.sh`, `run_capture.sh`, `syntax_check.py`) use `build/linux-32/`; set `SMS_ARCH=64` for `build/linux-64/`, or `SMS_BUILD=dir` for any other build folder.
Tools that compare with retail read the Dolphin captures from `$DOLPHIN_ORACLE`.

- **Moving the decomp pin:** `tools/update-decomp.sh [--64] [REF]` fetches the decomp, moves `decomp/` to REF (default `origin/main`), checks that every `decomp-patches/` patch applies in order with no failed hunk and no fuzz, and builds 32-bit (and 64-bit with `--64`).
  It keeps the new pin, staged for a commit, only when all of that passes; otherwise it restores the old pin and says what failed.
  Patches name files by their path in the decomp, so a decomp change that moves files (as the upstream merge that moved the libraries to `libs/` did) needs their `--- a/`/`+++ b/` lines rewritten, and the include roots in `CMakeLists.txt` and the source roots in `tools/gen_sources.py` updated with it.
  The submodule is always pinned to an exact decomp commit.
- **Regression checks:** `tools/regress/regress.py CHECK...` makes the scripted runs every port change is verified with, in both word sizes, and prints PASS or FAIL for each run against the baseline and for each pair of 32 and 64-bit runs, which must be identical.
  These are the runs this page, [64-BIT.md](64-BIT.md) and [ECLIPSE.md](ECLIPSE.md) mean by the scripted title and plaza frames, the plaza audio, the 30/60/120 fps plaza gate runs and the Eclipse runs.
  The checks are `title` (captures at fields 300 to 1500), `plaza` (a new game into Delfino Plaza with `SMS_WARP=1,0,1` and scripted input, captures at fields 3700 to 5400, and the same run with audio, whose first 88 s of `SMS_AUDIO_WAV` are hashed), `fps60` (the plaza gate at `SMS_WARP=1,5,0` under gdb, `tools/regress/gate.py`: at `SMS_FRAME_RATE=60` Mario must be captured after 100 frames, with the `setNextStage` of the 30 fps run), `fps120` (the same gate check at 30, 60 and 120 fps; 120 captures Mario within one frame of the native elapsed time), and `eclipse` (BSE's first boot, whose saved card the others start from and whose card files are hashed too, the Tutorial, the Tutorial as Luigi and as Piantissimo under gdb (`tools/regress/character.py`, which also reports their movement parameters and jumps), and the Fire Petey and Dark Zhine warps `SMS_WARP=72,0` and `79,0`); `vanilla` combines `title`, `plaza` and `fps120` and `all` everything, and `--list` names the single runs, which can be given on their own.
  Every run uses the deterministic clock, no settings file, no texture packs and skipped movies, so two runs give the same bytes; only hashes are kept (`baseline.txt` records the commit each run was recorded from), and the captures are deleted unless `--keep` is given.
  Nothing is built: the tool runs `build/linux-32` and `build/linux-64`, and `build-ecl` and `build-ecl64` for Eclipse ([ECLIPSE.md](ECLIPSE.md#building-it)), so build them first, or name other folders with `--build32`, `--build64`, `--ecl32` and `--ecl64`.
  It takes the disc from `rom/` as `run.sh` does, else from `decomp/orig/GMSE01/` or a neighbouring `sms-english` clone's, or `--disc`, and the Eclipse disc from where `tools/mods/get.py` puts it.
  `all` takes about 23 minutes on four cores with llvmpipe, two runs at a time (`--jobs`).
  A change that is meant to change what the runs show is recorded with `--record` (which refuses when 32 and 64-bit differ) and the new `baseline.txt` committed with it, saying why the frames changed.

The plaza interior regression is `python3 tools/regress/interiors.py --disc /path/to/GMSE01.iso`.
It uses gdb to place Mario on the real doorway triangles, lets the game perform the warps, and checks two entries and exits each for the boathouse and lighthouse at 30 and 60 fps in both word sizes.
It checks the active map model, Mario's height and the ground plane, so a return warp that strands Mario in the interior fails even before he walks off its floor.

The Turbo Nozzle pickup reproducer is `tools/regress/nozzle.py`, run under gdb on regular Sunshine.
It breaks an enabled nozzle box through its trample message, moves Mario into the released nozzle, and checks that Turbo is equipped, its stage unlock flag is set, and the Turbo save prompt opens during ten seconds of gameplay.
It also runs the Turbo controller in the two affected ground/swim statuses with nonzero dash speed and checks that the former out-of-bounds target stays unchanged.
Use a fresh temporary card directory; existing unlock flags deliberately fail the test.
Set `SMS_WARP=2,7,1` for Bianco Hills or `3,7,1` for Ricco Harbor, and vary `SMS_FRAME_RATE` and the executable to check other frame rates and word sizes.
The script reads the frame rate from the game and continues an already attached inferior, so it can also be used with a Windows GDB remote target.
Windows needs an SDL window, Windows paths for the disc and save directory, and a debugger built for the Windows target ABI.

```sh
nozzle_save=$(mktemp -d)
SMS_SAVE_DIR="$nozzle_save" SMS_SETTINGS=/dev/null SMS_TEXTURE_PACKS=0 \
SMS_HEADLESS=1 SMS_AUDIO=0 SMS_SKIP_MOVIES=1 SMS_VI_DETERMINISTIC=1 \
SMS_FRAME_RATE=60 SMS_WARP=2,7,1 \
SMS_AUTOPRESS='START@1400,STICK_LEFT@2000,A@2400,STICK_LEFT@3100,A@3250,A@3450,A@3800' \
gdb -q -batch -nx -ex 'set debuginfod enabled off' \
    -ex 'handle SIG34 nostop noprint' -x tools/regress/nozzle.py \
    --args build/linux-64/sms /path/to/GMSE01.iso
```

## Platform layer


| Module | What it does |
| --- | --- |
| `platform/sdk_stubs.cpp` | Generated: a weak, logging stub for all 321 SDK functions in `api-surface.tsv` that are not header inlines/macros; real implementations override them. |
| `platform/port_runtime.cpp` | Boot: emulated MEM1 (24 MiB) mapped at `0x80000000`, game source selection, GLX vendor choice, crash handler. |
| `platform/os/` | One emulated CPU: every `OSThread` is a host thread, only the CPU owner runs, strict priorities; message queues, mutexes, conds; interrupts delivered at OS-call check points and on idle; arena, clocks, OSAlloc, stopwatches, cache ops (→ `GXPC_InvalidateRange`). |
| `platform/dvd/` | DVD over the disc image (`platform/disc`) or an extracted folder; the disc's own FST; async reads complete as interrupts. |
| `platform/disc/` | GameCube `.iso`/`.gcm`/`.ciso` reader; also reads the disc image bundled at the end of the executable. |
| `platform/vi/` | 59.94 Hz retrace from a host timer (or the deterministic clock), callbacks, captures, trace hook. |
| `platform/pad/` | Controller 1 from keyboard and SDL game controllers, bindings, scripted input, `.dtm` movie input hook. |
| `platform/card/` | Memory card in slot A as host files. |
| `platform/ar/` | 16 MiB ARAM, ARQ transfers (completion runs before `ARQPostRequest` returns: JAudio busy-waits on it). |
| `platform/mtx/` | `PSMTX*`/`PSVEC*`/`C_MTX*` as the DOL computes them (`mtx_ps.inc`). |
| `platform/audio/` | DSP mail HLE, software mixer, AI output via SDL2; `noaudio.cpp` is the `SMS_NO_AUDIO` configuration. |
| `platform/thp/` | THP movie decoding (the decomp's SDK decoder built for the host). |
| `platform/endian/`, `platform/misc/endian.cpp` | Big-endian → host conversion of resources the game reads in place (dispatch from archive fetches by magic/file name). |
| `platform/gx/` | GX/GD over OpenGL 3.3. |
| `platform/trace/` | Native-vs-retail lockstep tracing. |
| `platform/misc/` | SDK globals, fallback fake DSP (only without the audio HLE), MSL `rand`. |

Low-memory OS globals and hardware register blocks that SDK headers *define* (`AT_ADDRESS`) become weak host globals via the `dolphin/types.h` override; `u32`/`s32` are `int` on LP64 hosts and unchanged on 32-bit.

## Decomp patches

Each file in `decomp-patches/` starts with a `Reason:` line; they are applied in name order to copies under `build/<os>-<arch>/patched/`.

| Patch | Reason |
| --- | --- |
| 0001–0003 | g++ rejects `case` labels that jump over initialised locals (`gesso.cpp`, `MarioAutodemo.cpp`, `MarDirectorDirect.cpp`): brace the case bodies. |
| 0004 | `JASTrack.cpp`: `goto bail` jumps over `u32 r31 = 0`: hoist the declaration. |
| 0005 | `MapDraw.cpp` redeclares `SMSGetGameRenderWidth/Height` as `int` (conflicts with `Resolution.hpp`). |
| 0006–0008 | Reference-binding leniency (`MarDirectorInitECT.cpp`, `GCConsole2.cpp`, `bombhei.cpp`, `tobiPuku.cpp`). |
| 0009–0010 | Duplicate string globals (`SunModel.hpp`, `NpcAnm.cpp`/`NpcParts.cpp`) made weak. |
| 0011 | Endian: `JKRMemArchive` converts RARC metadata on open and passes each resource (with its name) to the converters on first fetch. |
| 0012 | Endian: Yaz0 decompressed length read big-endian. |
| 0013 | Endian: `JSUInputStream` typed reads convert from big-endian. |
| 0014 | `SMS_NO_AUDIO`: empty JAudio configuration; per-frame work and wave-load queries become no-ops. |
| 0015 | `TVec2`/`TVec3` `operator+`/`-` (and two TU-local helpers) return a reference to a local, which g++ compiles to a NULL return: return by value on the port. |
| 0016 | `SMS_SKIP_MOVIES`. |
| 0017 | Endian: `J3DTevStage::load` builds its `{reg, op, AB, CD}` BP command words big-endian. |
| 0018–0019 | Host integer types: `JKRArchive`'s default constructors pass `(s32)0` to its `intptr_t` parameter (ambiguous with the `const char*` constructor under g++), and `TMarDirector::fireStartDemoCamera` is defined with `u32` where `MarDirector.hpp` declares `uintptr_t` (the same type under MWCC, not on the host). |
| 0020 | Clang: `TTabePuku::init` binds `theNerve()`'s const nerve to a non-const `TNerveBase<TLiveActor>*` (g++ `-fpermissive` only warns): `const_cast`. |
| 0021–0022 | Host integer types for clang (g++ `-fpermissive` only warns): `ARQCallback` takes `uintptr_t` like the callbacks passed to it (`JKRAramPiece::doneDMA`, JASystem's `aramDmaFinish`), and the four `fireStartDemoCamera` callbacks still spelled with `u32` take `uintptr_t` as `MarDirector.hpp` declares. |
| `endian-01..16` | Loader-site byte-order fixes (JPA, J2D BLO, BMG, JUTColor, PRM, SPC, streams, DL vertex counts, sequences, card saves, THP headers, J3DSkinDeform/J3DCluster display lists, the plaza shine-shadow sphere, the HUD/map 2D archive swap); see `platform/endian/README.md`. |
| `port-02` | `SMS_WARP` / `SMS_WARP_MOVIE`: debug warp or movie from a file-select load. |
| `zzz-presence-01` | `SMS_PRESENCE`: `TApplication::gameLoop` hands the area, episode, Shine and blue coin counts, lives and pause state to `platform/presence` once a frame, for the launcher's Discord Rich Presence. |
| `audio-01..02` | JAudio bitfield/byte-order fixes (`TChannel` mix config, BMS note-on flags); see `platform/audio/README.md`. |
| `ret-02..03` | Explicit returns for the 34 functions that fall off the end of a non-void body and whose value nothing reads (undefined behaviour under g++, harmless under MWCC). |
| `thp-01..02` | Host THP decoder (portable bit reader and IDCT, big-endian audio header); see `platform/thp/README.md`. |
| `endian-18` | `JSUInputStream`'s typed reads keep the value when a read fails at the end of the stream, as the console does, instead of byte-swapping it (see [Memory and undefined-behaviour checks](#memory-and-undefined-behaviour-checks)). |
| `fpu-02..04` | MWCC's float-to-unsigned conversions at every site where the DOL makes them, with the runtime's result (`port_cvt_fp2unsigned` and `port_cvt_dbl_usll` in `port_fpu.h`: 0 below zero and for NaN, 0xFFFFFFFF from 2^32 up): the hit-check table index and the iris wipe's first row (`fpu-02..03`), and the 37 other source sites (`fpu-04`; `TMarDirector`'s wipe fade is in `framerate-35`). See [Float-to-integer conversions](#float-to-integer-conversions). |
| `fpu-07` | `OSf32tos8` (`JASTrack`'s two calls) stores through GQR4 as the Gekko does, saturated to -128..127 and converted towards zero (`port_gekko_quantize` in `port_fpu.h`); the inline had no host body. See [64-BIT.md](64-BIT.md), item 18. |
| `bounds-01..04` | Retail out-of-bounds accesses whose result depends on byte order or data layout, given the console's result: `TGCConsole2`'s unset pane index, the two 16-byte null textures read as 32, NPC colour indices past their tables, an archive root's `..` (`JKRArchive` lookups find nothing, as the console's reading of the bytes before the directory table does). |
| `uninit-01..03` | Retail reads of uninitialised stack values made deterministic: the stack light objects' direction, the logo wipe's pen vector z, the plaza tightrope colour. |

## Memory and undefined-behaviour checks

Last audit 2026-09-30, on the scripted title and plaza runs (no stage sweep).

- **Tooling.**
  AddressSanitizer runs only in a 32-bit build: x86-64 ASan puts its shadow memory at `0x7fff8000`, over MEM1's fixed `0x80000000`.
  That build also had UBSan (`-fsanitize=address,undefined,float-cast-overflow -fno-sanitize=return -fsanitize-recover=all`, `-O1 -g1`, a separate build folder; GCC's `vptr` check needs `-fno-sanitize=vptr` on `J3DModel.cpp` and `JUTConsole.cpp`, whose classes have no key function).
  ASan cannot see inside the game's JKR heaps, which live in MEM1, so uninitialised reads there come from valgrind memcheck (`--track-origins=yes`) on the normal 64-bit build, started with `--vgdb-error=0`: gdb stops at `port_os_init`, marks the arena undefined with `monitor make_memory undefined 0x80004000 0x3ffc000`, sets `vgdb-error` back up and detaches.
  memcheck then reports reads of arena memory that nothing has written since boot, and uninitialised stack values; it does not see stale data in reused heap blocks.
  It runs the game about 100 times slower than native (llvmpipe, `LP_NUM_THREADS=0`), so in the time available it covered the boot, the logos and the title screen, not the plaza.
  A gdb probe on `GXLoadLightObjImm` recorded every light object the game loads, in both word sizes.
- **Fixed** (patches above):
  - The stack `GXLightObj`s of `TLightCommon`/`TLightMario::setLight` and `perform`, `TSilhouette::setting` and `TMapObjPlane` leave the direction unset, so GX_LIGHT0 and GX_LIGHT1 carried stack garbage, different in each word size: a NaN (`ffffb750`) and values near 1e14 in the 32-bit build, zeros or small values in the 64-bit build (`uninit-01`).
    The lights' angular coefficients are (1, 0, 0) or all zero, so the result on the console does not depend on a finite direction, but the host shader multiplies it (0 × NaN is NaN), and GLSL's `max(NaN, 0.0)` is undefined, so a GPU driver could light those materials differently from llvmpipe.
  - `JSUInputStream`'s typed reads byte-swapped the value after a read that failed at the end of the stream, where the console leaves it unchanged (`endian-18`).
    Delfino Plaza's `TMapWireManager` entry ends after its two counts, so `TMapWire::mDrawWidth` and `mDrawHeight` (5.0 and 6.0) became 5.7e-41 and 6.9e-41 and the tightropes were drawn with no width.
    The same entry leaves the tightrope colour to an uninitialised stack `s32`: on the console, whatever the scene search in the preceding `TCubeManagerBase::load` left in that slot, which is not reproduced; the port starts it at 0 (black) so both word sizes draw the same (`uninit-03`).
  - MWCC converts a float to an unsigned integer with `__cvt_fp2unsigned`, which gives 0 for a negative value; the host wraps it (`fpu-02..03`).
    `TObjHitCheck::getTableIndex` got a first cell of 0xEA instead of 0 for an actor whose entry radius reaches past the world origin, entering it in 22 extra cells; `Hxs2_Circle` started its rows at 0xFFFFFFEC and drew nothing once the iris wipe's ring passed the screen centre.
    The DOL has 70 such conversions in 22 objects; UBSan saw a negative value only at these two sites on these runs, and `fpu-04` has since given the rest the runtime's result (see [Float-to-integer conversions](#float-to-integer-conversions)).
  - `TGCConsole2` starts its highlighted-pane index at 0x17, past the 22-entry `unk334`, and calls `isVisible()`/`hide()` on `unk334[23]`, which is the `TBoundPane` `unk390`: the console reads and clears the high byte of its big-endian `unk4.x2` (0), the host the low byte (60), which `hide()` zeroed (`bounds-01`).
  - `J3DSys` and `gd-reinit-gx` load 4x4 IA8 null textures (32 bytes) from 16-byte arrays; the console's second half is the next `.data` in the DOL, which the port now carries (`bounds-02`).
  - Scene data can give an NPC a body or cloth colour index past the end of its `NpcInitData` table, and `SMS_InitChangeNpcColor` reads the entry regardless: the console reads the next bytes of that file's `.rodata`, the port whatever its own layout puts there, which differs between the word sizes.
    `SMSPortNpcColorEntry` returns retail's bytes for every such entry a scene uses (`bounds-03`): Super Mario Eclipse's Yoshi village Yoshis and other Piantas (see [ECLIPSE.md](ECLIPSE.md)), and Nintendo's `test11`, the only one on the retail disc.
  - `Hxs_PenDraw` normalises a vector with an unset z (`-2e-19` and similar in the 32-bit build, 0 in the 64-bit one); it is now 0 (`uninit-02`).
- **Real, left as they are** (the port reads what the console reads, or the value does not matter):
  - Uninitialised members read from never-written arena memory, 0 on both the console (MEM1 is zeroed at boot) and the port: `JUTGamePad::CRumble` reads the pad's port number before the constructor sets it; `MSModBgm::unk0`, `TOptionControl::mSelectedOption`, `TMario::mFreezeImmunityTimer`, `TSpcInterp`'s lock flag, `TConsoleStr::unk2A5`, `J3DMatPacket::mpShapePacket` and joints' scale flags that `J3DModel::calcNrmMtx` reads before anything computes them.
  - `TMarDirector::direct`'s movement-pass `JDrama::TGraphics` is never initialised, so `setViewport` tests garbage `field_rendering`; the draw pass sets the viewport again.
  - `TModelDataNode` keeps a pointer to `TMapObjBase::makeMActors`' stack name buffer and later `strcmp`s against it (ASan: stack-use-after-return); on the console and the port the buffer is usually the same address holding the new name.
  - `J2DPicture::insert` reads `unk104[i + 3]` and `J3DTevBlock4`'s display list reads `mTevKColorSel`/`mTevKAlphaSel` up to index 15 of 4-entry arrays: the neighbouring members, at the same offsets as on the console.
  - `JDrama` stores -1 in `VITVMode` as a sentinel; the `JPA`, camera and matrix code converts out-of-range floats to `s16`/`u8`/`u16`, which both MWCC (`fctiwz`, then a halfword or byte store) and g++ truncate to the low bits of the 32-bit result; `RumbleChannelMgr::start` and the `pointer-overflow` reports are pointer arithmetic that is never dereferenced.
- **Benign reports**: UBSan's misaligned-access reports (JKR heaps align to 4, the host ABI wants 8 for classes with `double` or `long long`; x86 does not care), its downcast and member-call type reports on the decomp's approximate class hierarchies (`TBGCheckList` arrays that are never constructed, name-searched objects cast to the searched type), memcheck's reports inside Mesa, and `gx::resolveWriteBack` hashing game memory that holds struct padding copied from the DVD layer.
- **Result**: the scripted title and plaza frames are unchanged against `3676789` and byte-identical between 32 and 64-bit (the tightropes and the iris ring are not in those frames), and the 30/60/120 fps plaza gate runs still captures Mario after 100 frames with the same `setNextStage`.
- **Not covered**: stale data in reused JKR heap blocks (memcheck would need the heaps' frees marked undefined), and anything past the plaza runs.

## Float-to-integer conversions

Audited 2026-09-30 from the DOL's disassembly (the decomp's `build/GMSE01/asm`), mapped to source with a `-fsanitize=float-cast-overflow -O0` compile of every port unit, which lists each host float-to-integer conversion by source line and target type.

- **What the console does.**
  MWCC converts a float or double to a 32-bit unsigned integer only by calling the runtime's `__cvt_fp2unsigned` (`runtime.c`; no inline sequence anywhere in the DOL): 0 below zero and for NaN, 0xFFFFFFFF from 2^32 up (+inf included), truncation otherwise.
  To a 64-bit integer, signed or unsigned, it calls `__cvt_dbl_usll`: truncation as a signed value (a negative value wraps as a `u64`), and 0x7FFF...F or 0x8000...0 by the sign bit from 2^63 in magnitude, infinities and NaN included.
  To `u8`, `u16` and every signed type it uses `fctiwz` and keeps the low bits: saturation to the `s32` range, NaN as 0x80000000.
- **What the host does.**
  x86 wraps a negative value, and outside the 32-bit range the two builds disagree with the console and with each other: 5e9 gives 0 (32-bit) or 0x2A05F200 (64-bit), -3e9 gives 0x80000000 or 0x4D2FA200, NaN 0x80000000 or 0; the console gives 0xFFFFFFFF, 0 and 0.
  No GCC flag changes this code generation (`-fsanitize=float-cast-overflow` only reports).
  Clang's `-fno-strict-float-cast-overflow` saturates, which is the runtime's rule for `u32` but not the console's for `u8`, `u16` and signed types, which keep the low bits of `fctiwz`, and the Linux build is GCC.
- **The sites.**
  The DOL makes 70 `__cvt_fp2unsigned` calls and 2 `__cvt_dbl_usll` calls.
  60 of the former are game and JSystem code, from 40 source lines (the hit-check table index's two are inlined six times, `JGadget::TVector`'s growth six, `JAISound`'s random value five): `ObjHitCheck` (2 lines), `hx_wiper` (16: frame-buffer rows, the game-over bounce timer, the test wipes, the iris ring), `TGCConsole2::setTimer`, `TSelectDir` and `TMovieDirector`'s sound fades, `TMarDirector`'s wipe fade, `TMarioGamePad::reset`'s button repeat, `CLBLinearInbetween<u32>`, `JGadget::TVector::GetSize_extend`, `MSSetSoundCanRestart`, `J3DDrawBuffer::entryZSort`, `JAIBasic`'s frame-SE priorities and distance waits (5), `JAISound`'s random value and pitch spread, `TOscillator::calc`'s table index, `JPADrawCalcColorAnmFrameRepeat`, and the fog mantissa of `J3DGDSetFog` and `FifoSetFog`.
  The other 10 are the SDK's: `GXSetFog`, `GXGetNumXfbLines`, `GXGetYScaleFactor` (3) and `GXSetDispCopyYScale`, which `platform/gx` reimplements, and `OSDumpContext` (4), which the port does not have.
  `__cvt_dbl_usll` is called by `JGeometry::TUtil<f32>::mod` and MSL's `std::fmodf`.
- **Handled.**
  `port_fpu.h` has the runtime's two conversions (`port_cvt_fp2unsigned`, `port_cvt_dbl_usll`; checked against a model of the runtime's instructions on 20,000,000 inputs in each word size), and `port_compat.h` has `port_cvt_fp<T>` for a template (the runtime's conversion for a 32-bit unsigned `T`, a cast otherwise).
  `fpu-02..03` route the first 3 source lines through it, `fpu-04` the other 37 and `TUtil<f32>::mod`, `framerate-35` the wipe fade it rewrites, and `platform/gx` its four SDK sites.
  `std::fmodf` and `TUtil<f32>::mod` now go through `sms_msl_fmodf` (`msl_math.c`, `fpu-05`), which makes both runtime calls as the DOL does.
  The port's own `u32` conversion of a frame rate (`framerate-18`) uses it as well.
  In range the result is the host's own truncation, so nothing changes there.
- **Left.**
  When this audit was made, `std::fmodf` was the host's exact `fmodf` and `TUtil<f32>::mod` kept its subtraction unfused; both are the console's since [64-BIT.md](64-BIT.md), item 15.
  `GCConsole2`'s one `__cvt_sll_flt` (a signed 64-bit integer to float, rounded to double first) is the host's single rounding; the two differ only past 2^53.
  The `fctiwz` paths (`u8`, `u16`, signed types) match the console except from 2^31 up and +inf, where the console gives 0x7FFFFFFF and x86 0x80000000 (so `u8` 0xFF against 0); the port does not change them.
- **Result.**
  The scripted title and plaza frames are unchanged against `6fc6838` and byte-identical between 32 and 64-bit (the previous audit's UBSan run saw an out-of-range value at none of the `fpu-04` sites on these runs), and the 30/60/120 fps plaza gate runs still captures Mario after 100 frames with the same `setNextStage` in both builds.
  A 32-bit build with `-fsanitize=float-cast-overflow` (`-O1 -g1`, a separate build folder) reports no conversion to a 32 or 64-bit unsigned type on the same runs; its 25 remaining reports (the first at each source line) are `s16`, `u8` and `u16` conversions of values between -3150 and 48789, where the console and both builds keep the same low bits of the truncated `s32`.

## Environment variables

The everyday options are in the [README](../README.md#options); this is the full list.

| Variable | Effect |
| --- | --- |
| `SMS_ARCH=32\|64` | which build `build.sh` and `run.sh` use (see [BUILD.md](../BUILD.md)) |
| `SMS_DISC_IMAGE`, `SMS_DISC_ROOT` | game source (image file, or extracted `files/` folder); either, or a disc argument, overrides files bundled into the executable |
| `SMS_SAVE_DIR` | memory card directory |
| `SMS_HEADLESS=1` / `--headless` | no window (offscreen EGL; Linux only) |
| `SMS_BINDINGS` | key bindings file (default `./bindings.txt`, then `../../bindings.txt` when started from `build/<os>-<arch>/`) |
| `SMS_AUDIO=0` | no sound output; the game's audio (JAudio's thread and sequencer, the software DSP) still runs, paced as with no output device ([64-BIT.md](64-BIT.md), item 19) |
| `SMS_NO_AUDIO=1` | run with an empty JAudio configuration and an idle AI DMA; THP movies stall (their video waits for audio), so combine with `SMS_SKIP_MOVIES=1` |
| `SMS_SKIP_MOVIES=1` | report every THP movie as finished at once |
| `SMS_WARP=stage,scenario[,shines]` | debugging: loading a file goes to that area instead (`1,0,1` is Delfino Plaza right after the airstrip), optionally with that Shine count |
| `SMS_WARP_MOVIE=n` | debugging: loading a file plays streaming movie `n` (0–19, `TMovieDirector::getStreamMovieName`) |
| `SMS_SHOTS=f,f,...`, `SMS_SHOT_DIR` | capture the XFB at these fields (retail numbering) as PPM; `tools/shots.py` converts them into `build/shots/` and compares with retail |
| `SMS_AUTOPRESS=control@field[+hold],...` | scripted input (e.g. `start@1400+20,stick_left@3300+40,a@3500+30`); in a window it is injected as SDL key events, i.e. through the keyboard path |
| `SMS_FIELD_CLOCK=retrace` | shots/autopress count VI retraces (wall clock) instead of game fields (2 per display copy, the default) |
| `SMS_VI_DETERMINISTIC=1` | virtual VI/OS clock: retraces fire when the game idles (or spins on `OSGetTick` for a whole field), `OSGetTime` follows them from a fixed date, AI DMA is paced by retraces (no output device); two runs with the same input give identical frames |
| `SMS_VI_HZ=<rate>` | retrace rate override (benchmarking) |
| `SMS_PRESENT_HZ=<rate>` | each swap also waits for the next refresh of a display at that rate, as a compositor that forces vsync does (frame pacing tests) |
| `SMS_VI_FIELD_BASE=<n>` | the retrace counter starts at `n` (retail spends about 240 fields in IPL/apploader/DOL load before the game's first frame; 240 puts the Nintendo logo on retail's field 300) and selects game-frame parity |
| `SMS_DVD_BPS`, `SMS_DVD_SEEK_MS`, `SMS_DVD_LOG=1` | drive timing model (reads occupy the drive for bytes/rate + seek, counted in fields; off by default) and a per-read log |
| `SMS_MOVIE`, `SMS_TRACE_OUT` | `.dtm` movie input and retail-format field traces (`platform/trace`) |
| `SMS_MEM_MB`, `SMS_QUIET_STUBS=1` | emulated MEM1 size; silence first-call stub logs |
| `SMS_PRESENCE=1` | print a `[presence] {...}` JSON line to stdout whenever the area, episode, Shine or blue coin count, lives, pause or demo state changes (`platform/presence`); SMS Launcher sets it and shows the line as Discord Rich Presence |
| `SMS_OVERLAY=1` | open the debug overlay (frame rate and where the frame's time goes) at start |
| `SMS_SETTINGS=file` | settings file to read instead of `settings.txt` (working directory, then `../../`); its names map to the variables in `kSettings` (`platform/port_runtime.cpp`), and any `SMS_*` name can be used as is |
| `SMS_TEXTURE_PACKS`, `SMS_TEXTURE_PACK_MB`, `SMS_TEXTURE_PACK_LOG`, `SMS_TEXTURE_PACK_SYNC`, `SMS_TEXTURE_PACK_PRELOAD`, `SMS_TEXTURE_PACK_PENDING_MB` | texture packs (see [mods/README.md](../mods/README.md)); preloading prepares loaded level resources before gameplay, `_PENDING_MB` bounds decoded backlog, `_SYNC=1` restores first-use loading for repeatable captures |
| `SMS_GX_*` | graphics switches (`platform/gx/README.md`) |

## Progress log

With the disc image, the 32-bit Linux build boots through the Nintendo logo, the Dolby screen, the opening THP movie and the title, creates a save file on the host memory card, shows file select, and on a scripted run (`SMS_AUTOPRESS=start@1400+20,a@3000+30,stick_left@3300+40,a@3500+30,stick_right@3700+60,a@3900+30`) walks Mario to block A, opens the file menu, starts a new game and plays the airstrip opening cutscene.
Frames from that run are in [`shots/`](shots/) (`title-*`, `filesel-*`, `airstrip-*`, and `compare-*` next to retail).
Since then the scripted plaza and beach reference runs have become the regression checks, and warps reach episode 0 of the airstrip and of each stage from Bianco Hills to Pianta Village; [64-BIT.md](64-BIT.md) records those runs.

## Performance

Measured headless on the 32-bit Linux build (Mesa llvmpipe software GL), 2026-09-23; details are in the commit that added this section.

- **Returns fixed.**
  41 of the 42 fall-off-the-end functions (39 with no return statement, 3 with a path that falls off) now return the value retail's r3 carries.
  Five of them are live and their callers read the result.
  Four (`DSPBuf::mixDSP`, `Dvd::openDvd`, `TMap::intersectLine`, `TLampTrapSpike::receiveMessage`) are fixed in the decomp, where the explicit return compiles to the same bytes.
  `TConductor::isBossDefeated` is not fixed: the decomp's `switch` lacks retail's `default:` arm, every form with it tried so far lowers its match (98.8% to 95.6%), and the port does not patch around the decomp (see [Where a fix goes](#where-a-fix-goes)).
  The other 37 are `ret-02..03`: nothing reads their value, so only g++ needs the return.
  The decomp itself has since made `JUTGamePad::read` (0085b21c) and `PushReadedBuffer` (aac56beb) `void` and given JGadget's `TList::Confirm` its `return true` (aac56beb), so `ret-02..03` now cover 34.
  After the patches, `-Wreturn-type` reports nothing over all units, so the game library no longer depends on `-O0`.
- **-O1/-O2 are safe as far as the port reaches.**
  With the existing `-fno-strict-aliasing -fwrapv`, a game library built at -O1 or -O2 boots through the logo, the attract movies (THP) and the title to file select, with audio, and showed no crashes over about 10 runs of 60–100 s.
  Gameplay levels are not reachable yet, so level code is untested at -O2.
- **Where the time goes** (main thread = game threads plus the GX translation and the GL driver front-end):

  | Build | Title/movies, VI uncapped | Boot to frame 1600 (VI 240 Hz) | File select: main thread | File select: fps (VI 240 Hz) |
  | --- | --- | --- | --- | --- |
  | all -O0 (current) | 61 fps | 97 s | 26.3 ms/frame | ~13 |
  | game lib -O1 | – | 73 s | 26.2 ms/frame | 17.4 |
  | game lib -O2 | 77 fps | 58 s | 26.2 ms/frame | 17.8 |
  | game lib + platform + `platform/gx` -O2 | – | ~42 s | 10.2 ms/frame | 36.6 |

  The game library's -O level mainly speeds up boot and movies (THP decoding lives there).
  The per-frame cost of 3D scenes is in `platform/` and `platform/gx` at -O0.
  After both are optimised, llvmpipe (about 52 ms of CPU per frame, spread over its threads) is the limit.
- **Result:** the default build type is `RelWithDebInfo`, `-O2 -g` for `sms_game`, `sms` and `sms_gx`, keeping `-fno-strict-aliasing` and `-fwrapv` (the decomp type-puns freely; strict aliasing was not tried).
  `SMS_VI_HZ=<rate>` overrides the retrace rate for benchmarking.
- **Delfino Plaza, 2026-09-25** (the scripted plaza run, `SMS_GX_STATS=30`, the 810-batch window):

  | | before | after |
  | --- | --- | --- |
  | GL calls per frame | ~41,000 | ~5,200 |
  | of which `glMapBufferRange` + `glUnmapBuffer` | – | ~1,640 (one pair per batch) |
  | vertex loader (32-bit; 64-bit after: 3.0) | 10.6 ms/frame | 4.7 ms/frame |

  The GL state is shadowed and only changes are sent, samplers and uniforms are cached, each batch's vertices, indices and XF block go into one streamed buffer through one map, and the loader keeps packed per-format vertices and per-VAT readers.
  What is left on the game thread is mostly the game itself, the vertex loader and the EFB-copy write-back (`encodeTexture`, `hashBytes`); on llvmpipe the rest is rasterisation.
  The overlay's frame breakdown (README, "Frame rate") shows the same split on any machine.
- **Widescreen** (`SMS_WIDESCREEN`): the game camera is widened by a port patch and sms_gx maps each draw into the wider EFB (`drawXMap` in `gx_render.cpp`).
  The HUD stays 4:3 in the middle by default.
  With `SMS_WIDESCREEN_HUD=edges` the gameplay HUD is anchored to the screen edges per piece, not per 2D batch (which pulls composite panes apart): `TGCConsole2::perform` marks its HUD drawing (`GXPC_SetHud`), and `J2DPane::draw` brackets each pane with its extent (`GXPC_HudPaneBegin`/`End`, patch `widescreen-03`).
  A piece is the outermost pane narrower than three quarters of the screen (the HUD's containers are 600 wide), and moves by which third of the 4:3 frame its centre is in; draws outside any piece (the water tank, drawn in 3D) move by their own extent.
- **30, 60 and 120 frames per second** (`SMS_FRAME_RATE`, patches `framerate-*`): 60 is the port default, 30 is native, and 120 is optional.
  `port_active_frame_rate` is the gameplay rate while `TMarDirector` runs, and 30 for logos, menus and movies.
  `SMSGetVSyncTimesPerSec` reports that rate, so animations and fades keep their real-time duration.
  The VI timer runs at 59.94 Hz for 30/60 configurations and 119.88 Hz for 120; the display waits two/one retraces at 30/60 and four/one for menus/gameplay with 120 configured.
  This clock is independent of monitor refresh rate.
  Frames keep to a fixed schedule of retraces (`framerate-37`, `port_vi_frame_wait`): retail waits for one more retrace after a frame that ends past its own, which halves the game's speed wherever the host's present waits for the display (vsync, macOS, the Steam Deck's compositor), so a late frame starts the next at once instead, and one a whole frame behind is not presented (but one is at least every 0.05 s) when that lets the game keep time, i.e. presenting is slow (2 ms or more on average, `sms_gx`) and the frames' other work fits in their time, so a host too slow for the frame rate still shows every frame; a frame more than 0.1 s late starts a new schedule, and the deterministic clock keeps retail's wait.
  `SMS_PRESENT_HZ=<rate>` makes each swap wait for a display at that rate, to try this without such a display.
  Movement keeps its 120 Hz ticks: `TMarDirector::direct` adds `600 / SMSGetVSyncTimesPerSec()` per frame and spends 5 per tick (four, two or one ticks per frame).
  The shared helpers in `src/port_include/port_framerate.h` scale visual steps by 30 / active rate, run native integer counters once per four movement ticks, and compensate tick animations by active rate / 30.
  Exponential chases use square roots at 60 and fourth roots at 120 to preserve their decay per second.
  `framerate-02` to `framerate-35` apply those helpers to wipes, gates, timers, flocks, effects, movement animations and sound.
  Keep-alive ripple and smoke requests repeat across all intervening displayed frames, preserving their 1/30 s lifetime.
  Menu button repeats are recomputed when entering or leaving gameplay; JAI sound frame work runs once per two frames at 60 or four at 120.
  A held sound effect (one the game requests every frame to keep playing, such as the Rocket and Turbo charge sound) is requested again on the frames in between; `framerate-38` keeps it playing where retail would restart it, which also stops the restarts' track interrupts from requeueing an SE port command and dropping the BGM volume updates queued after it (Shadow Mario's distance fade).
  JPA particles use whole 1/60 s steps; `framerate-36` runs their simulation on alternate frames at 120 while drawing every frame, avoiding truncation of the 0.5 animation rate to zero.
  VI captures, scripted input, deterministic audio DMA, tracing, virtual OS time and optional DVD timing retain native time units when the retrace rate doubles.
  `tools/framerate/check.sh` checks configuration and equal elapsed-time movement, integer counts and visual chases in both word sizes without requiring a GPU or high-refresh monitor.
  `tools/regress/regress.py fps120` checks the plaza gate at all three rates against its baseline and between word sizes.
  Retail capture/audio baselines explicitly use 30 fps; random choices can diverge at higher rates because random numbers are drawn per frame.
- **Software GL.**
  The 32-bit Linux build without the GPU driver's `:i386` libraries renders with llvmpipe and cannot hold 30 fps in the plaza; the port logs a warning and the overlay says so.

## Crash diagnostics

The launcher captures stdout and stderr separately, keeps a full session log on disk, and reports a game's actual exit code or signal. Windows game executables run directly rather than through MSYS bash, which can translate native exception statuses to 127. Ordinary resource warnings and stage context are never used as a game crash explanation.

The port installs crash reporting before boot and before switching onto its low stack. Every report is written to stderr without allocating or taking a lock the crashed thread may hold, the most important lines first, and has the same sections on every platform:

- **What happened:** the signal and its code (Linux, macOS) or the Windows exception code, the faulting and instruction addresses, the thread, and the module and offset of the instruction.
- **The fault address explained:** in the null page (a null pointer plus an offset), in emulated MEM1 with its GameCube address, a GameCube address past the end of MEM1, a MEM1 address with its upper 32 bits set (a pointer stored in a `u32`), or the module it falls in. Linux adds the `/proc/self/maps` lines of the fault address and the instruction (the mapping and its permissions).
- **Registers:** every general register (x86-64, x86, arm64), and whether the fault was a read, a write or an instruction fetch, from the page-fault error code (x86), the ESR (macOS arm64) or the access-violation parameters (Windows).
- **Backtrace:** raw frames, then the executable's frames symbolised to function, file and line, inlined calls included: `addr2line` on Linux and Windows (MSYS2's, found on `PATH`, or `SMS_ADDR2LINE`), `atos` on macOS. On Windows it runs from a thread made at start-up, and the report goes on after 10 s if it cannot finish (a crash holding the heap or loader lock).
- **Memory:** the instruction bytes around the faulting instruction (` | ` before it) and the top 24 words of the stack.
- **Game context:** every emulated `OSThread` (priority, state, entry function, the one owning the emulated CPU), the last eight disc files the game asked for (which show the stage being loaded), the build (compiler, architecture, Eclipse), the system (OS version, Wine if present, processors), the executable, and how long the game had run.

On Linux and macOS, game threads get an alternate signal stack, so stack exhaustion can still produce a report. A fault while reporting ends the process with the original signal; a second thread crashing meanwhile waits for the first report. Linux retains signal termination; macOS keeps the existing `128 + signal` exit to avoid the Rosetta termination hang.

Windows uses a vectored exception logger, which runs before the stack is unwound. It reports every error-severity exception (`0xC…`: hardware faults, heap corruption, and unwinding failures such as `0xC00000FF` `STATUS_BAD_FUNCTION_TABLE`) and C++ throws, each with a budget so exceptions a driver raises and handles itself cannot flood the log. An exception raised while another was being handled names the one it interrupted. On 64-bit Windows the backtrace unwinds with the modules' own unwind data (frame pointers do not chain there) inside the thread's stack bounds; it stops rather than guesses at a frame without unwind data, and on a game thread it ends at the entry to the low stack (`port_win64_stack_call`'s thunk). 32-bit follows the `ebp` chain. To resolve a frame by hand, run `addr2line -f -C -e <exe>` on the executable's image base (`objdump -p <exe> | grep ImageBase`) plus the printed offset. `abort()` gets the same report from the `SIGABRT` handler. A last-chance filter adds `unhandled exception …: the process ends` when nothing handled an exception; on a 64-bit game thread dispatch cannot walk past the low-stack thunk to reach it, so its absence after a game-thread report does not mean the exception was handled. The logger returns `EXCEPTION_CONTINUE_SEARCH` and does not turn hardware exceptions into CRT signals, preserving their exit statuses. A first-chance report can be followed by successful handling; it is not proof of a fatal crash. Fast-fail and some heap-corruption termination paths can bypass in-process handlers, so the direct process exit code and Windows Event Viewer remain useful. Fully exhausted custom Windows stacks may also prevent an in-process report.

ROM-free Linux verification uses a configured game build's compilation settings and deliberate access violations, aborts, worker-thread faults and stack exhaustion:

```sh
python3 tools/test_crash_logging.py --build-directory build/linux-64 --arch 64
python3 tools/test_crash_logging.py --build-directory build/linux-32 --arch 32
```

For native Windows verification, compile `platform/os/tests/crash_probe.cpp` with `platform/os/windows_crash.cpp`, `platform/os/crash_info.cpp`, the `src/` include directory, and (for 64-bit) `platform/os/windows_stack.cpp`. Then run `python tools/test_crash_logging.py PATH_TO_PROBE.exe --arch 64` or `--arch 32` from native Python. The 64-bit probe includes a fault on the custom low stack. The unwind probe raises `0xC00000FF` to check the logger reports unwinding failures. The report probe checks the `abort()` report without ending the process. The probe also builds with MinGW on Linux and runs under Wine (with MSYS2's `addr2line.exe` on `WINEPATH` for symbols). The heap probe explicitly raises that status to test the logger; it does not guarantee the logger sees real heap corruption or fast-fail.
