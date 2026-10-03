# platform/thp — THP movie decoding on the host

The game's THP player (`decomp/libs/THPPlayer/src/*`) calls the Dolphin SDK decoders `THPInit`, `THPVideoDecode` and `THPAudioDecode`.
The port used to stub them in `platform/sdk_stubs.cpp`.
They now come from the decomp's own SDK decoder source, `decomp/libs/dolphin/src/thp/THPDec.c` and `THPAudio.c`.
Host patches make that source run on the host and support optional HD replacements.

| Patch | Change (all under `#ifdef TARGET_PC`) |
|---|---|
| `decomp-patches/thp-01-THPDec-host-decoder.patch` | Replaces the PowerPC asm with portable C: the Huffman bit reader (same `c`/`currByte`/`cnt` state, the stream read as big-endian 32-bit words), the coefficient decode, and the paired-single AAN IDCT (float, dequantised with the same AAN-scaled tables, `x/8 + 128` saturated to u8). Marker parsing, table generation, MCU row loops and the output layout are the decomp's own code. The locked cache becomes a static buffer: `LCStoreData` is a `memcpy` plus `DCFlushRange`, so the GX layer re-decodes the texture, and the HID2 locked-cache check is skipped. Output is unchanged: Y/U/V planes as GX I8 tiles (8×4), written in 16-row strips. |
| `decomp-patches/thp-02-THPAudio-be-header.patch` | `THPAudioDecode` reads the big-endian `THPAudioRecordHeader` through a host-order copy. The ADPCM decode is unchanged, since it works byte by byte. |

The player-side byte order (THP header, component info, frame size words) is converted by `decomp-patches/endian-12-THP-headers.patch`.

## Linking it (for the bring-up lead)

`platform/thp/thp.cmake` adds the two patched files (`${SMS_PATCH_ROOT}/libs/dolphin/src/thp/THPDec.c`, `THPAudio.c`) to `sms_game`.
They build with the game's C flags: `TARGET_PC`, and the force-included `port_compat.h` for `port_be16`/`port_be32`.
Their strong `THPInit`/`THPVideoDecode`/`THPAudioDecode` override the weak stubs.
Use one of:

```cmake
# in CMakeLists.txt, after add_library(sms_game ...):
include(${CMAKE_CURRENT_SOURCE_DIR}/platform/thp/thp.cmake)
```

```sh
# or with no CMakeLists.txt change:
cmake -S . -B build -DCMAKE_PROJECT_INCLUDE=$PWD/platform/thp/thp.cmake
```

Configure prints `SMS port: host THP decoders linked`.
`nm build/linux-32/sms | grep THPVideoDecode` then shows `T`, not `W`.
The patches are applied by `cmake/patches.cmake` like every other patch.
The platform glob must not pick up `platform/thp/tests`: the test keeps its patched copies in `build-thp-test/`, and its only source is a `.cc`.

`DCFlushRange` should reach `GXPC_InvalidateRange` (platform/gx README, step 4), so textures decoded into the same buffers are refreshed.
`THPDraw` also calls `GXInvalidateTexAll` every frame.

## Test

```sh
make -C platform/thp/tests run          # MOVIE=data/openingA.thp by default; DISC=..., WORK=... to override
```

The test reads the movie from the extracted disc (read-only) and decodes every frame the way the player does, using a big-endian frame walk.
It writes five frames as PNGs (Y/U/V untiled, BT.601 to RGB) and all audio (track 0) as a WAV.
It fails if any frame fails to decode, or if the audio sample count differs from the header.

Results on GMSE01:

| Movie | Video | Audio |
|---|---|---|
| `openingA.thp` (opening after "new game") | 640×320, 2049/2049 frames | 2,189,735 samples = header, 68.43 s vs 68.37 s video |
| `openingBA.thp` | 640×320, 3138/3138 | 3,353,532 = header |
| `Entrance.thp` | 640×448 (generic N×N path), 2816/2816 | 3,009,416 = header |
| `EX128x144_q0.thp` | 128×144, 300/300 | none |

The frames checked by eye (the plane on the airstrip, Peach, the Isle Delfino welcome screen) are clean.
There are no block artefacts and the colours are right.
The audio is a real signal: RMS about 2100, 2 clipped samples out of 4.4 million.

## HD replacements

`thp-03` expands the MCU row cache for dimensions up to 2048×2048; `thp-05` preserves full host pointer width in decoder alignment.
`thp-04` sends oversized textures to `hd_movies.cpp`, which copies complete I8 tile rows into distinct regions of an aligned low-address pool and draws them through the original YUV renderer.
`zz-hd-movies-01` preserves the original display footprint and subtitle placement.
See [HD cutscenes](../../docs/HD-CUTSCENES.md) for conversion and playback.
