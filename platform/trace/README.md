# platform/trace — native-vs-retail lockstep testing

The goal is that the native port behaves like retail.
Three pieces check that against the Dolphin oracle (its checkout is `$DOLPHIN_ORACLE` below):

1. **Movie player** (`trace.cpp`): feeds the oracle's `.dtm` pad input into `PADRead`, so the native game gets the same inputs as the retail run.
2. **Trace writer** (`trace.cpp`): writes the same named memory ranges every VI field, in the oracle's trace format.
   Values are converted to big-endian with each range's DWARF layout, so they compare byte for byte with retail.
3. **Tools**:
   - `tools/trace_resolve.py` (gdb Python) turns the oracle's symbolic range files into native addresses and layouts.
   - `tools/trace_compare.py` reports the first diverging field, range and member between a retail trace and a native trace.

Everything is off unless `SMS_MOVIE` or `SMS_TRACE_OUT` is set.

## Hooks (for the bring-up lead)

Two calls, declared in `platform/trace/trace.h`:

```c
// platform/vi/vi.cpp, retrace_irq(), right after g_retrace_count++ (before the game's callbacks):
port_trace_on_retrace(g_retrace_count);

// platform/pad/pad.cpp, top of PADRead() after init():
if (port_trace_pad_read(status))
    return PAD_CHAN0_BIT;
```

`port_trace_movie_active()` tells whether a movie is playing, if other input sources should be muted.
The trace sources are compiled into `sms` by the `platform/**/*.cpp` glob.
They are 32-bit only: native pointer members must be 4 bytes for the layouts to line up with retail.

## Running

```sh
# 1. resolve the oracle's range file against this binary (repeat after every relink)
TRACE_RANGES=$DOLPHIN_ORACLE/ranges/play.txt TRACE_OUT=build/play.ranges \
    gdb -batch -x tools/trace_resolve.py build/linux-32/sms

# 2. native run: same movie as the retail run, poll timing taken from the retail trace
SMS_MOVIE=$DOLPHIN_ORACLE/movies/play5.dtm \
SMS_MOVIE_POLLMAP=$DOLPHIN_ORACLE/runs/play-r9/trace.txt \
SMS_TRACE_RANGES=build/play.ranges SMS_TRACE_OUT=build/play-native.txt SMS_TRACE_FRAMES=11500 \
    build/linux-32/sms --headless

# 3. compare
tools/trace_compare.py $DOLPHIN_ORACLE/runs/play-r9/trace.txt build/play-native.txt \
    --names build/play.ranges.json
```

| Variable | Meaning |
|---|---|
| `SMS_MOVIE` | `.dtm` to play on port 1 (from the oracle's `scripts/make_dtm.py`) |
| `SMS_MOVIE_POLLMAP` | a retail trace: its `polls=` per field gives the exact poll index at every field |
| `SMS_MOVIE_POLL_BIAS` | added to the poll index (default 0) |
| `SMS_TRACE_OUT` | trace file |
| `SMS_TRACE_RANGES` | native ranges from `tools/trace_resolve.py` |
| `SMS_TRACE_SYNC` | how native retraces map to retail fields (below); default `app+8:1=2@231` |
| `SMS_TRACE_START` / `SMS_TRACE_EVERY` | first field written / write every n-th field |
| `SMS_TRACE_FRAMES` | exit once this retail-numbered field is reached |

### Timing semantics

The oracle's movie has one record per controller *poll*, not per field.
SMS polls twice per field once running, and the retail trace records the running poll count (`polls=`) at each field boundary.
Retail spends about 230 fields in the IPL/apploader before `main`, which the port does not.

So native retraces are numbered in retail fields.
- With the default `SMS_TRACE_SYNC=app+8:1=2@231`, the first retrace at which `TApplication::mAppState` (byte 8 of the `app` range) reads 2 becomes field 231, as in retail.
  Numbering then advances one per retrace.
- The general form is `<label>+<hexoff>:<width>=<value>@<field>`, evaluated on the big-endian traced bytes.
  `SMS_TRACE_SYNC=+N` instead gives a fixed offset: field = retrace + N.
- Before the sync point, `PADRead` sees a connected pad with neutral input, and nothing is traced.
- During field F, `PADRead` returns movie record `polls(F+1) − 1`.
  That is the last poll before the next boundary, where `polls(F)` is the poll count retail had reached at F's boundary.
  SMS reads the pad late in the field, after both of the field's SI polls.
  This reproduces retail's `JUTGamePad::mPadStatus` exactly: a 6-poll START press at polls 450–455 shows at boundaries 340–341 in both traces.
  `polls(F)` comes from the retail trace (`SMS_MOVIE_POLLMAP`); past its end, or without one, it continues at two polls per field (`polls ≈ 2F − 227`).
  Use `SMS_MOVIE_POLL_BIAS` if a native run turns out to read one poll early or late.
- The trace is written in `retrace_irq`, at the same point as the game's retrace callbacks.
  Each `F` line carries `polls=` (the movie record in use) and `retrace=` (the native count).
  `ticks`, `pc`, `lr` and the MEM1 hash are 0 or absent.

### Ranges and byte order

`tools/trace_resolve.py` runs inside gdb on the binary, without starting it.
For each oracle range (`symbol`, `*symbol+off`, `*(symbol+n)`, `0xADDR`, `size`):
- **Symbols:** looked up in the port's DWARF.
  CodeWarrior-mangled static members such as `mPadStatus__10JUTGamePad` are looked up as `JUTGamePad::mPadStatus`.
- **Types:** the pointee type of `*gpMarioOriginal` is `TMario`.
  For `*(gpApplication+0x20)`, the member at that offset is a `TMarioGamePad*`, so the pointee is `TMarioGamePad`.
- **Layout:** each type is flattened into integer/float/pointer/byte members, written as a compact layout (`4p,4i,2ix3,1bx2,4f,...`).
- **Relocation:** host globals are written relative to `port_trace_anchor` so the PIE load address is irrelevant.
- **Heap:** absolute MEM1 addresses are used as they are, since game heaps live in the emulated MEM1 at `0x80000000`.
  Their layout is unknown and treated as 4-byte integers, unless a `# type <label> <Type>` line in the range file names one.
- **Other output:** `TRACE_OUT.json` lists every member name per label, for the compare report.

At run time each range is copied and every multi-byte member byte-swapped, so the `D` lines hold big-endian bytes like retail's.
- **Unreadable ranges:** a dereference through NULL or outside MEM1 and the host image gives `D <label> - -`.
- **Class layouts:** the port's class layouts equal the GameCube's where the decomp declares the same members.
  x86-32 aligns 8-byte members to 4, so a structure with `s64`/`f64` members after odd padding could shift.
  The per-member report shows where.

## Comparing

`tools/trace_compare.py RETAIL NATIVE [--names X.json] [--align field|polls] [--only RE] [--ignore RE] [--from F] [--to F] [--ftol 1e-3] [--frel 1e-5]`

- **Pairing:** retail and native fields are paired by number (native is already in retail numbering), or by equal `polls=` with `--align polls`.
  `--resync LABEL+OFF:W=VALUE` shifts native numbering so that the first field where the condition holds lines up in both traces, for traces recorded without re-sync points.
- **Ignored differences:**
  - pointer members (layout `p`);
  - any 4-byte word whose retail value points into MEM1 while the native value is non-zero (disable with `--strict-pointers`);
  - float members within the tolerance.
- **Report:** the first divergence overall with member name, both byte windows, both poll counts, and a timing hint (the nearest retail field with the native value).
  Then the first divergence of every label.
- **Exit status:** 0 when nothing differs.

The oracle's own `compare_traces.py` compares two Dolphin runs.
This tool is its native counterpart: it uses the native trace's layouts in place of the two linker maps.

### GameCube layout

The oracle's offsets are GameCube (MWCC) offsets, and the port's g++ layout differs from them.
- **Tail padding:** the Itanium ABI puts a derived class's first member into its base's tail padding.
  For example `JDrama::TViewObj::unkC` sits at 10, not 0xC, so `TPlacement::mPosition` sits at 0xC, not 0x10.
- **Alignment:** 8-byte members align to 4, not 8.

`trace_resolve.py` therefore recomputes every type's MWCC layout from its DWARF members: no tail-padding reuse, 8-byte scalars aligned to 8, members in declaration order.
It emits a copy map (`map=gcoff:natoff:<w><k>[x<n>]`), and the writer rebuilds each range in the GameCube layout before byte-swapping.
Bytes that no member covers stay zero.

The rebuilt layout is only as complete as the decomp's declared members, and those match retail wherever the decomp matches.
Checked for `JUTGamePad` against retail's constructor (`__ct__10JUTGamePadFQ210JUTGamePad8EPadPort`), all confirmed by the stores in retail's code:
- `sth r0,0x78(r29)` → `mPortNum` at 0x78.
- `addi r30,r29,0x7c` for the `JSULink` → `mLink` at 0x7C.
- `stw 0x8c`/`0x90` → `mPadRecord` 0x8C, `mPadReplay` 0x90.
- `stb 0x98` → `mButtonReset` 0x98.

The resolver computes exactly this.
Only the offset *comments* in `decomp/libs/JSystem/include/JSystem/JUtility/JUTGamePad.hpp` are wrong from `mPortNum` on:
- `mPortNum` 0x78, not 0x7C.
- `mErrorStatus` 0x7A, not 0x7E.
- `mLink` 0x7C, not 0x80.
- `mPadRecord` 0x8C, not 0x90.
- `mPadReplay` 0x90, not 0x94.
- `field_0x9c` 0x94.

This is cosmetic: no code depends on the comments.

## Range syntax

The resolver reads dolphin-oracle range files (`symbol[+off]`, `0xADDR`, `*symbol[+off]`, `*(symbol+n)[+off]`), plus nesting.
`*(*gpCardLoad+0x298)+0x10` reads the pointer at offset 0x298 of the object `gpCardLoad` points to, then adds 0x10.
All offsets are GameCube offsets; each one is translated through the MWCC layout of the type it indexes.
Native starts are written as `@0xSTATIC` (host symbol) or `0xMEM1`, followed by `/0xOFF` per dereference.

Heap objects should be reached through pointers, never absolute addresses, because the native heap layout differs from retail's.
`ranges/play-symbolic.txt` in the oracle is `play.txt` with `cardload = *gpCardLoad` and the three blocks as `*(*gpCardLoad+0x298/0x29c/0x2a0)`.
It was checked against `runs/play-r9`: retail's `TCardLoad` holds the old blockA/B/C addresses at those offsets.
The oracle's `run_oracle.py` resolves `cardload` today, and skips the nested block lines until its Dolphin range format supports a pointer chain.

## Deterministic runs (SMS_VI_DETERMINISTIC=1)

With the lead's deterministic VI/OSTime mode, `SMS_AUDIO=0`, and a fresh card per run (`SMS_SAVE_DIR`), two runs of `play5.dtm` give identical traces.
`trace_compare.py` finds no difference in 11,269 fields; the only byte differences are host pointer values.
`build-trace-test/detrun.sh` is the command used.

Findings against `runs/play-r9`, with `play-symbolic.txt` and the re-syncs `app+8:1=2@231,app+e:4=0x0f000000@456,app+e:4=0@1244,mdstate+0:1=4@5419`:

1. **The opening movie stalls natively with `SMS_AUDIO=0`.**
   `THPPlayer` only advances video while `curVideoNumber - curAudioNumber <= 1`, and the audio frame count advances in the AI DMA callback, which is idle.
   Native sat in the movie from field 1244 to the end of the run.
   `SMS_SKIP_MOVIES=1` avoids it.
   Deterministic runs need AI DMA paced by the retrace clock (lead: platform/audio).
   Since then the AI DMA is paced by the retraces in deterministic runs, and `SMS_AUDIO=0` keeps it running ([64-BIT.md](../../docs/64-BIT.md), item 19), so the movie no longer stalls.
2. **From the airstrip re-sync (retail field 5419) Mario's path matches retail.**
   Position agrees within 0.01 through the walk and the first jump (y = 540.5 at retail 5736 on both), with native one field ahead: native field f = retail field f+1.
   **First real divergence: native field 5773 / retail 5774**, Mario x/z 0.01 apart (755.455/1173.571 vs 755.445/1173.575).
   It grows to about 12 units by 5800 and the paths separate completely by 6000.
3. **Differences present from the re-sync on, which are candidate causes:**
   - **`THitActor::mEntryRadius`** 215.8806 native vs 215.8703 retail.
     `THitActor::calcEntryRadius` takes one `__frsqrte` estimate with no Newton step.
     The port's `__frsqrte` (`src/port_compat.h`) is an exact `1/sqrt`, but Gekko's is a table estimate.
     Every hit actor's entry radius differs slightly, and so do collision candidates.
     Emulating the estimate is the lead's call (compat header).
   - **Input phase.** SMS runs its logic at 30 Hz, and native's frames land on the opposite field parity from retail's after the re-sync.
     Pad input keyed to retail field numbers therefore reaches native one field earlier relative to its frames.
     For example, stick X 0xb8 at native 5714 vs retail 5715.
     Presses held for 3 fields still line up in game frames here, but any 1-field press would not.
   - **`TMario::mFlag` bit 0:** 9 native vs 8 retail from the re-sync on.
   - **`mHeadMtx` (head look):** differs by about 1e-3.
4. **`PADClamp` is a no-op in `platform/pad`.**
   Retail clamps sticks and triggers (the SDK clamp is in `decomp/libs/dolphin/src/pad/Padclamp.c`).
   A trace-worktree build with it applied changed nothing up to 5773 for this movie, since it only uses full deflections.
   It should still be linked for movie fidelity.

## Floating-point fidelity (2026-09-23)

- **Estimates:** `__frsqrte` and `__fres` now reproduce Gekko's table estimates bit-exactly (`src/port_fpu.h`).
  They were measured with `tools/fpprobe`: a probe DOL run in the oracle, checked on 106,497 values, and `TMario::mEntryRadius` = 0x4357e16c as in retail.
- **Metric:** on the airstrip window after the re-sync (native fields 5421–5740 vs retail +1), the share of Mario/camera float members that are bit-exact with retail:
  - baseline 83.79%;
  - with the estimates 84.07%;
  - estimates plus `-msse2 -mfpmath=sse -ffp-contract=off` 86.04% (x87 extended precision removed).
  - An `-mfma -ffp-contract=fast` variant was built but not yet run.
- **First divergence is unchanged by these:** native 5773 / retail 5774 (Mario x/z 0.01).
  - Camera `mYaw` starts to differ at 5751, and Mario's intended yaw follows it.
  - Retail has `MARIO_FLAG_OCCLUDED` set throughout the airstrip, native does not.
    The flag comes from `TMario::drawSyncCallback`, which peeks EFB alpha (`GXPeekARGB`) and is read by `CameraNormal`.
    The oracle traces with Dolphin's Null video backend, where EFB peeks return 0, so the retail trace always reports "occluded".
  - The retail reference for gameplay lockstep must therefore come from a real-video oracle run: `movies/play5gl.dtm`, identical input, `--video OGL`, running as `runs/play-gl1`.
  - Two further timing differences remain.
    The `TMarioGamePad` stick-scaling ramp (`_E4`) is one update out of phase, and `mRepeatCount` is +1: native reaches the airstrip after a different number of pad updates, because `SMS_SKIP_MOVIES` skips the opening.
    Running the movie (needs audio DMA paced in deterministic mode) and fixing the 30 Hz field parity should remove both.

## Status (2026-09-23)

What was checked: a native run (worktree build with the two hooks) traced against `dolphin-oracle/runs/play-r9` (`movies/play5.dtm`, 11500 fields).

- **Tracing and sync:** the whole path traces, 11,260 native fields.
  With `SMS_TRACE_SYNC=app+8:1=2@231,app+e:4=0x0f000000@456,app+e:4=0@1244,mdstate+0:1=0@5352`, sync lands at native retrace 3–4, and the title re-sync moves native by +9..+11 fields.
  Native reaches the logo and title faster than retail's DVD loads.
- **Pad input:** the movie reaches the game correctly.
  `JUTGamePad::mPadStatus[0].button` and the gamepad's `mButton` follow retail's START/A presses, e.g. START at retail fields 340–341 and native 339–340 (or 340–341 in another run).
- **App states:** `app` states match the retail sequence: 2 → 3 → 4 → 5 on area 15, i.e. boot, Nintendo logo, title.
- **Native is not deterministic run to run.**
  - The VI retrace is a real-time 59.94 Hz host timer, so sync lands one retrace earlier or later from run to run, and input lands with it.
  - In one run native left file select into the airstrip at field 1283 (retail 1244, then 4100 fields of opening movie).
  - In two runs it stayed in file select.
  - For true lockstep, VI needs a deterministic clock: one retrace per game frame, or retraces driven by the game's `VIWaitForRetrace`/`GXCopyDisp` instead of wall time.
  - This is platform/vi, the lead's file.
- **Heap ranges:** `cardload` and `blockA/B/C` in `play.txt` are absolute retail heap addresses, which never match natively.
  Use `play-symbolic.txt` (see Range syntax).
- **Bug found:** tracing the airstrip exposed a stack smash in `J3DSkinDeform::initMtxIndexArray`: display lists were parsed as native u16.
  It is fixed by `decomp-patches/endian-14-J3DSkinDeform-dl-be.patch`.
- **Earlier gamepad divergence:** the `TMarioGamePad::mFlags` difference at field 231 came from comparing native (g++) offsets.
  With the GameCube layout it is gone.
