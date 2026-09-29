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
