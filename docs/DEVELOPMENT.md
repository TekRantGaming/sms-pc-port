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
- **PC-specific fixes go in `decomp-patches/`**: byte order (`endian-*`), host compiler leniency (`0001`–`0010`, `0015`, `ret-02..03`), host services (`thp-*`, `audio-*`), port-only features (`port-*`, `SMS_*` switches). Each patch starts with a `Reason:` line saying why it cannot live in the decomp.
  A patch never corrects the decomp's behaviour; it only adapts retail's behaviour to the PC.
- **Emulation of the hardware goes in `platform/`** (GX, DVD, OS, audio), never in game source.
- Example: the sun-glass tint that stopped part-way down the screen was a decomp bug (`TOrthoProj`'s reconstructed constructor stored its last two edges swapped), so it was fixed in `sms-english` and verified against retail, not patched here.

## How the build works

`build.sh` configures `build/<os>-<arch>/` with CMake ([BUILD.md](../BUILD.md#manual-cmake-build) has the options); everything it generates stays in that folder.

- `-DSMS_ARCH=32` compiles with `-m32` (Linux: `gcc-multilib g++-multilib`; Windows: MINGW32).
  `-DSMS_ARCH=64` is a native 64-bit build that keeps game memory, static data and thread stacks below 4 GiB ([64-BIT.md](64-BIT.md)); it is what macOS builds.
- Default build type `RelWithDebInfo` = `-O2 -g` for all targets (the fall-off-the-end functions got explicit returns, `ret-*` patches).
- The 32-bit Linux build compiles against the amd64 SDL2/EGL headers and links the i386 runtime libraries (`/usr/lib/i386-linux-gnu/libSDL2-2.0.so.0`, `libEGL.so.1`) directly, so no `:i386` `-dev` packages are needed.
- The decomp keeps the game in `src/` and `include/`, and each library in `libs/<name>/src` and `libs/<name>/include` (`dolphin`, `JSystem`, `THPPlayer`, `PowerPC_EABI_Support`, `TRK_MINNOW_DOLPHIN`, `OdemuExi2`), as upstream `doldecomp/sms` does.
  The game sees the headers through the same roots in `configure.py`'s order (`include`, then each `libs/<name>/include`), except MSL's C and C++ headers: the port uses the host's.
- Game units: every `.c`/`.cpp` in `decomp/src`, `decomp/libs/JSystem/src` and `decomp/libs/THPPlayer/src` (588 units, listed by `tools/gen_sources.py`, which reads `configure.py` for the ones that get `SMS.pch`), plus the SDK's pad clamp and the decomp's THP decoder (`platform/thp/thp.cmake`); the rest of the SDK, MSL, the runtime, MetroTRK and OdemuExi2 are left out.
- `decomp-patches/` are applied in name order to copies under `build/<os>-<arch>/patched/`, at the same paths as in the decomp (`patched/src/...`, `patched/libs/JSystem/include/...`); `decomp/` itself is never modified.
- Flags: `-std=gnu++03 -fno-gnu-keywords -fpermissive -fno-strict-aliasing -fwrapv -finput-charset=UTF-8 -fexec-charset=CP932 -DGEKKO -DTARGET_PC -DVERSION_GMSE01 -DBUILD_VERSION=2 -DNDEBUG=1`, host libc/libstdc++ instead of MSL, `-include src/port_compat.h`.
  String literals are Shift-JIS, as in the MWCC build (archive object names are matched against them); clang has no CP932 execution charset, so on macOS the sources are mirrored as CP932 first (`tools/darwin_cp932_mirror.py`).
- The game's global `operator new/delete` (JKRHeap) are renamed in `libsms_game.a` with `objcopy --redefine-syms` (`llvm-objcopy` on macOS), so only game code allocates from JKR heaps; libstdc++ and `platform/` use the host allocator.
- `rand()` is MSL's (RAND_MAX 32767, same LCG) via `port_compat.h`; glibc's 2^31 range overflows the game's `1.f / (RAND_MAX + 1)`.

## Tools

| Tool | Use |
| --- | --- |
| `tools/common.sh` | shared by `build.sh`, `run.sh` and `clean.sh`: host detection, `SMS_ARCH`, build folder, `rom/` lookup, moving files out of older layouts |
| `tools/bundle_disc.py` | packs the disc's files into `sms-standalone` (or `SMS.app`'s `disc.gcm`) |
| `tools/make_mac_app.sh`, `tools/extract_icon.py` | assemble and sign `SMS.app`; the app / `.exe` icon from the disc's memory-card icon |
| `tools/run_capture.sh SECS FIELDS` | headless run + captures + retail comparison |
| `tools/shots.py`, `tools/contact.py OUT.png FIELDS...` | convert captures to PNG in `build/shots/` and compare with retail; contact sheet of captures |
| `tools/gdbrun.sh` | backtrace at the first fatal signal |
| `tools/hangdump.sh N` | every thread's stack after N seconds |
| `tools/trace_resolve.py`, `tools/trace_compare.py` | lockstep tracing against retail (`platform/trace/README.md`) |
| `tools/mkpatch.sh` | write a `decomp-patches/` patch from edited copies |
| `tools/warn_scan.py`, `tools/syntax_check.py` | one g++ warning class over all units; `-fsyntax-only` over all units |
| `tools/gen_sources.py`, `tools/gen_stubs.py` | regenerate `cmake/decomp_sources.cmake` and `platform/sdk_stubs.cpp` |
| `tools/fpprobe/` | PowerPC FPU behaviour probe (a DOL run in Dolphin) |

The Linux debugging tools (`gdbrun.sh`, `hangdump.sh`, `run_capture.sh`, `syntax_check.py`) use `build/linux-32/`; set `SMS_ARCH=64` for `build/linux-64/`, or `SMS_BUILD=dir` for any other build folder.
Tools that compare with retail read the Dolphin captures from `$DOLPHIN_ORACLE`.

- **Moving the decomp pin:** `tools/update-decomp.sh [--64] [REF]` fetches the decomp, moves `decomp/` to REF (default `origin/main`), checks that every `decomp-patches/` patch applies in order with no failed hunk and no fuzz, and builds 32-bit (and 64-bit with `--64`).
  It keeps the new pin, staged for a commit, only when all of that passes; otherwise it restores the old pin and says what failed.
  Patches name files by their path in the decomp, so a decomp change that moves files (as the upstream merge that moved the libraries to `libs/` did) needs their `--- a/`/`+++ b/` lines rewritten, and the include roots in `CMakeLists.txt` and the source roots in `tools/gen_sources.py` updated with it.
  The submodule is always pinned to an exact decomp commit.

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
| `platform/mtx/` | C versions of `PSMTX*`/`PSVEC*`/`C_MTX*`. |
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
| `endian-01..16` | Loader-site byte-order fixes (JPA, J2D BLO, BMG, JUTColor, PRM, SPC, streams, DL vertex counts, sequences, card saves, THP headers, J3DSkinDeform/J3DCluster display lists, the plaza shine-shadow sphere, the HUD/map 2D archive swap); see `platform/endian/README.md`. |
| `port-02` | `SMS_WARP` / `SMS_WARP_MOVIE`: debug warp or movie from a file-select load. |
| `audio-01..02` | JAudio bitfield/byte-order fixes (`TChannel` mix config, BMS note-on flags); see `platform/audio/README.md`. |
| `ret-02..03` | Explicit returns for the 37 functions that fall off the end of a non-void body and whose value nothing reads (undefined behaviour under g++, harmless under MWCC). |
| `thp-01..02` | Host THP decoder (portable bit reader and IDCT, big-endian audio header); see `platform/thp/README.md`. |

## Environment variables

The everyday options are in the [README](../README.md#options); this is the full list.

| Variable | Effect |
| --- | --- |
| `SMS_ARCH=32\|64` | which build `build.sh` and `run.sh` use (see [BUILD.md](../BUILD.md)) |
| `SMS_DISC_IMAGE`, `SMS_DISC_ROOT` | game source (image file, or extracted `files/` folder); either, or a disc argument, overrides files bundled into the executable |
| `SMS_SAVE_DIR` | memory card directory |
| `SMS_HEADLESS=1` / `--headless` | no window (offscreen EGL; Linux only) |
| `SMS_BINDINGS` | key bindings file (default `./bindings.txt`, then `../../bindings.txt` when started from `build/<os>-<arch>/`) |
| `SMS_AUDIO=0`, `SMS_NO_AUDIO=1` | idle the AI DMA / run with an empty JAudio configuration; either way THP movies stall (their video waits for audio), so combine with `SMS_SKIP_MOVIES=1` |
| `SMS_SKIP_MOVIES=1` | report every THP movie as finished at once |
| `SMS_WARP=stage,scenario[,shines]` | debugging: loading a file goes to that area instead (`1,0,1` is Delfino Plaza right after the airstrip), optionally with that Shine count |
| `SMS_WARP_MOVIE=n` | debugging: loading a file plays streaming movie `n` (0–19, `TMovieDirector::getStreamMovieName`) |
| `SMS_SHOTS=f,f,...`, `SMS_SHOT_DIR` | capture the XFB at these fields (retail numbering) as PPM; `tools/shots.py` converts them into `build/shots/` and compares with retail |
| `SMS_AUTOPRESS=control@field[+hold],...` | scripted input (e.g. `start@1400+20,stick_left@3300+40,a@3500+30`); in a window it is injected as SDL key events, i.e. through the keyboard path |
| `SMS_FIELD_CLOCK=retrace` | shots/autopress count VI retraces (wall clock) instead of game fields (2 per display copy, the default) |
| `SMS_VI_DETERMINISTIC=1` | virtual VI/OS clock: retraces fire when the game idles (or spins on `OSGetTick` for a whole field), `OSGetTime` follows them from a fixed date, AI DMA is paced by retraces (no output device); two runs with the same input give identical frames |
| `SMS_VI_HZ=<rate>` | retrace rate override (benchmarking) |
| `SMS_VI_FIELD_BASE=<n>` | the retrace counter starts at `n` (retail spends about 240 fields in IPL/apploader/DOL load before the game's first frame; 240 puts the Nintendo logo on retail's field 300) and selects game-frame parity |
| `SMS_DVD_BPS`, `SMS_DVD_SEEK_MS`, `SMS_DVD_LOG=1` | drive timing model (reads occupy the drive for bytes/rate + seek, counted in fields; off by default) and a per-read log |
| `SMS_MOVIE`, `SMS_TRACE_OUT` | `.dtm` movie input and retail-format field traces (`platform/trace`) |
| `SMS_MEM_MB`, `SMS_QUIET_STUBS=1` | emulated MEM1 size; silence first-call stub logs |
| `SMS_OVERLAY=1` | open the debug overlay (frame rate and where the frame's time goes) at start |
| `SMS_SETTINGS=file` | settings file to read instead of `settings.txt` (working directory, then `../../`); its names map to the variables in `kSettings` (`platform/port_runtime.cpp`), and any `SMS_*` name can be used as is |
| `SMS_TEXTURE_PACKS`, `SMS_TEXTURE_PACK_MB`, `SMS_TEXTURE_PACK_LOG`, `SMS_TEXTURE_PACK_SYNC` | texture packs (see [mods/README.md](../mods/README.md)); `_SYNC=1` decodes on first use instead of on the worker thread, for repeatable captures |
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
- **60 frames per second** (`SMS_FRAME_RATE=60`, patches `framerate-*`): while `TMarDirector` runs, the display waits one retrace per frame instead of two and `SMSGetVSyncTimesPerSec` reports 60 instead of 30.
  The game is built for this: `TMarDirector::direct` runs the movement pass in ticks of 1/120 s (it adds `600 / SMSGetVSyncTimesPerSec()` per frame and spends 5 per tick: four ticks a frame at 30, two at 60), animations advance `SMSGetAnmFrameRate()` per frame, and `TEmitterViewObj` runs particles that many times per frame, so gameplay keeps its speed; at 60 every other frame matches the 30 fps frame at the same moment.
  What counts rendered frames instead is fixed where it shows: the root fader takes the rate in force, and the stage-entry wipe keeps its one-second lead (`TConsoleStr::getWipeCloseTime`).
  Gameplay state that an object steps on its calc-anim pass (`CUE_CALC_ANIM`, once per frame) runs twice as often per second at 60 while movement ticks keep their rate, so a balance between the two passes breaks: the plaza gates (`TModelGate`) open by gaining per movement tick and closing per calc pass, and at 60 an already-opened gate never reopened; `framerate-03` steps the gate's calc-pass state once per 1/30 s.
  Two classes of step go wrong at 60: a per-frame step with an unscaled constant (a timer or amount stepped on the calc-anim pass, or on every `perform` call) runs twice as fast, and a per-tick step at `SMSGetAnmFrameRate() / 4` (one frame's animation spread over four ticks) runs at half speed, since that rate halves while the tick rate does not.
  `framerate-04` to `framerate-15` fix the known instances by stepping the first once per 1/30 s and running the second at its 30 fps rate, each only while `port_fps60_active` is set.
  `framerate-16` to `framerate-35` cover the rest of the calc-anim audit: purely visual steps (flocks, grass, flags, spins, blends, chases) take half a step at 60 so the motion stays smooth, while whole-number counters step once per 1/30 s.
  Where a counted step emits a keep-alive particle (a type 1 or 3 request, which `TMarioParticleManager` kills on the first pass it is not repeated), the request is repeated on the next frame so the emitter lives its 30 fps time.
  The menu auto-repeat (counted in pad reads, one per displayed frame) is recomputed from the rate in force when it changes, and `MSound::mainLoop` (JAI's frame work: fades, volume moves, sound lifetimes) runs every other frame at 60, so a sound length given in frames is in 30 fps frames.
  The check is to run the same scripted input at 30 and at 60 and compare the frames captured at the same fields; random choices (particle scatter, idle NPCs) diverge, since the random numbers are drawn per frame.
- **Software GL.**
  The 32-bit Linux build without the GPU driver's `:i386` libraries renders with llvmpipe and cannot hold 30 fps in the plaza; the port logs a warning and the overlay says so.
