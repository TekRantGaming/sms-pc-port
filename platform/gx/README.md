# sms_gx — GX/GD for the PC port

`sms_gx` is a static library that implements the Dolphin SDK graphics API the game uses (GX and GD, as declared by `decomp/libs/dolphin/include/dolphin/gx*.h` and `gd*.h`) on top of OpenGL 3.3 core.
It opens its own SDL2 window, or an offscreen EGL context when there is no display.

## Why not aurora

Step 1 of the brief looked at encounter's `aurora`, the GX/VI/PAD layer used by Metaforce.
Its MIT license is compatible, but it was a poor fit here:

- Its renderer is WebGPU through Dawn.
  Prebuilt Dawn packages exist only for 64-bit targets (`linux-x86_64`, `linux-aarch64`, ...), and building Dawn from source with two compile jobs on a shared machine takes hours.
  The port builds 32-bit first.
- It needs SDL3 (this machine has SDL2), C++20, and fetches abseil, xxhash and more at configure time.
- It ships its own `dolphin/*` headers and object layouts, which would have to be reconciled with the decomp's headers.

So Step 2 was implemented instead: a compact GX-over-OpenGL backend, about 4,700 lines of code.
No Dolphin-emulator code and no SDK source was used.
Register layouts were cross-checked against the decompiled SDK in `decomp/libs/dolphin/src/gx`, and the public headers in `decomp/libs/dolphin/include/dolphin` define the API.

## Design

The hardware is driven only by register writes and draw commands:

- BP registers (the pixel pipeline: TEV, blending, textures, copies)
- CP registers (the vertex loader: vertex descriptors, attribute formats, arrays)
- XF registers and XF memory (the transform unit: matrices, texgen, lighting, viewport, projection)

`sms_gx` models exactly that.
Every GX API call is encoded into the same register writes the SDK would send (`src/gx_api.cpp`).
Bytes written through the write-gather pipe, and display lists built by GD or J3D, go through one command-stream parser (`src/gx_fifo.cpp`).
The renderer reads its state only from that register file, so API calls, GD display lists and raw FIFO writes (`MarioUtil/PacketUtil.cpp`) always agree.

| File | Role |
| --- | --- |
| `src/gx_api.cpp` | GX API: register encoding, texture/TLUT/light objects, getters, FIFO objects, perf/verify stubs, helper shapes |
| `src/gx_fifo.cpp` | Register file, the BP/CP/XF command parser, the vertex loader (direct, index8 and index16 attributes, every component type and fraction), the display-list cache, the write-gather pipe, the memory write stamps, and the physical-address window |
| `src/gx_shader.cpp` | Generates GLSL from the register state. The vertex shader covers position/normal matrices, the GX projection and viewport, texgen (regular, post-transform/dual-tex, colour, approximate emboss) and lighting (8 lights, diffuse/spot/specular attenuation). The fragment shader covers up to 16 TEV stages in integer math, swap tables, konst selection, compare modes, 4 indirect stages, alpha compare and fog. Programs are cached by the state bytes that shape the code. |
| `src/gx_texture.cpp` | Decoders for I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CMPR, C4, C8 and C14X2 with IA8/RGB565/RGB5A3 TLUTs. The GL texture cache is keyed by address, format, size, mip levels and TLUT, and content hashes are re-checked after an invalidate. |
| `src/gx_render.cpp` | EFB as an FBO (640×528 × scale). Batches primitives (quads, strips and fans become triangle lists). Maps blend, logic-op, depth, cull, scissor, colour/alpha-update and dst-alpha state. Handles EFB copies (display copies become XFB textures; texture copies go through a format-converting pass that includes intensity/YUV, R/G/B/A/RG/GB and Z formats), XFB presentation and EFB peeks. |
| `src/gx_platform.cpp` | Creates the SDL2 window or headless EGL context, presents on `GXCopyDisp`, pumps events and handles the command-line and environment switches |
| `src/gx_glthread.cpp` | The GL thread and its command queue; `src/gl_loader.cpp` turns the GL entry points into proxies that queue work for it |
| `src/gx_vert.cpp` | `GXPosition3f32` and the other vertex writers as real functions |
| `src/gl_loader.cpp` | Loads the GL 3.3 entry points through the host's get-proc function, so nothing links against libGL |
| decomp `libs/dolphin/src/gd/*.c` | GD, compiled unchanged from the decompiled SDK with `src/gd_host_prefix.h` force-included |

## Integration (for the bring-up lead)

1. **Build.**
   The top-level `CMakeLists.txt` already does `add_subdirectory(platform/gx)` with `-DSMS_WITH_GX=ON` and links `sms_gx` whole-archive, so its strong `GX*`/`GD*` definitions replace the weak stubs.
   `sms_gx` links SDL2 and EGL publicly when they are found.
2. **Vertex writers.**
   On the GameCube, `GXVert.h` defines `GXPosition3f32` and the other writers as inlines that store to `0xCC008000`.
   In the port those stores would crash.
   Add the override directory ahead of the decomp headers for the game sources:
   ```cmake
   target_include_directories(sms_game BEFORE PRIVATE ${SMS_GX_OVERRIDE_INCLUDE_DIR})
   ```
   `port_include/dolphin/gx/GXVert.h` then declares the writers as functions that `sms_gx` implements.
   In C++ it also maps `GXWGFifo` onto `GXPC_WGPipe`, a proxy object, so `GXWGFifo.u8 = 0x61;` in `PacketUtil.cpp` feeds the same command parser.
3. **Addresses.**
   Command streams carry 32-bit physical addresses: J3D material display lists (texture images), `GDSetArray`, TLUT loads and display-list calls.
   `sms_gx` maps them as `ptr = 0x80000000 + phys`.
   That matches the unmodified `OSCachedToPhysical` macro and a MEM1 arena mapped at `0x80000000`, which is what `platform/os` does.
   If MEM1 moves, call `GXPC_SetMemoryWindow(base, size)` and keep `OSCachedToPhysical` consistent with it.
   API calls (`GXInitTexObj`, `GXSetArray`, `GXCopyTex`, `GXCopyDisp`, `GXInitTlutObj`) keep full host pointers, so they also work for memory outside the window and on 64-bit hosts.
4. **Caches.**
   Have `DCFlushRange`/`DCStoreRange` call `GXPC_InvalidateRange(p, n)`.
   `GXInvalidateTexAll` also re-checks every cached texture's contents, so textures the CPU rewrites get re-decoded.
5. **Byte order.**
   Display lists, pipe data, textures and TLUTs are read big-endian, as they sit on disc.
   Vertex and matrix arrays default to host order.
   The resource loader should register every file it loads in place with `GXPC_AddBigEndianRange(buf, size)`.
   BMD/BDL vertex arrays inside a registered range are then read big-endian automatically.
   `GXPC_SetArrayBigEndian(attr, 1)` is also available per attribute.
6. **Window and present.**
   No code change is needed.
   `GXInit` calls `GXPC_InitAuto(1)`: an SDL2 window with a GL 3.3 core context opens when `DISPLAY` or `WAYLAND_DISPLAY` is set, otherwise the library falls back to headless EGL.
   Every `GXCopyDisp` draws the XFB into the window, letterboxed to 4:3, swaps, and pumps events.
   If VI should own presentation, call `GXPC_SetAutoPresent(0)` and then `GXPC_Present(xfb)` on retrace.
   - Command line: `GXPC_ParseArgs(&argc, argv)` strips `--headless`, `--window` and `--vsync`.
   - Environment: `SMS_HEADLESS=1`, `SMS_GX_SCALE=n` (internal resolution), `SMS_WINDOW_SCALE=n`, `SMS_VSYNC=1`, `SMS_FULLSCREEN=1`, `SMS_PRESENT_HZ=rate` (testing: swaps wait for a display at that rate).
   - `SMS_FULLSCREEN=1` starts in desktop fullscreen on the same monitor chosen for the window, without changing its display mode. Unset it or use `SMS_FULLSCREEN=0` to keep the centered, resizable window. The launcher sets this from **Settings → Visuals → Full screen**, effective on the next game launch. Headless runs ignore it. If fullscreen fails, the game logs the error and keeps its window available.
   - Windows start centered on the pointer's monitor, with a default size of up to 1280×720. The window and its borders fit within 80% of the usable desktop, leaving its title bar and resize edges accessible. Internal resolution does not enlarge the window; `SMS_WINDOW_SCALE` requests a different starting size, still fitted to the screen.
   - Frame dumps: `SMS_GX_DUMP_EVERY=n SMS_GX_DUMP_DIR=dir` writes every n-th XFB as a PPM, which is useful for headless bring-up.
   - If no display or GPU driver works, run with `LIBGL_ALWAYS_SOFTWARE=1`.
7. **Input.**
   The PAD layer registers `sms_gx_set_event_callback(void (*)(const SDL_Event*))` and may call `sms_gx_pump_events()` from `PADRead`.
   Events are also pumped after every present.
   Game controllers are opened as they are plugged in, and closing the window exits the process.
8. **Threads.**
   GX calls come from the game's threads, one at a time (as `platform/os` runs them).
   OpenGL runs on a thread of its own once the window or headless context is ready (see Performance below); `SMS_GX_GL_THREAD=0` keeps it on the thread that ran `GXInit`.

## Performance

What a heavy frame (the scripted Delfino Plaza view, about 730 draws and 170,000 vertices) costs the game's thread, and what keeps it down:

- **Display-list cache** (`callDisplayList`, `gx_fifo.cpp`).
  Nearly all vertices come from display lists the game calls every frame (J3D shapes).
  The first call of a list decodes it as usual and keeps the packed vertices and batch indices; later calls reuse them while everything the decode read is unchanged (vertex descriptor and formats, array bases, strides and byte order, default matrix indices, the list's bytes and the array elements it indexes).
  The kept vertices and indices also go into a GPU buffer once (the *arena*, `gx_render.cpp`), so a batch draws them in place with one multi-draw instead of copying them each frame.
  Lists that only set state (J3D materials, vertex formats) are kept decoded into register writes; lists that both set state and draw are parsed every time.
  `SMS_GX_DL_CACHE=0` turns the cache off; `SMS_GX_DL_ARENA=0` copies the kept vertices into each batch instead.
- **Write stamps.**
  Every write the renderer is told about (`DCFlushRange`/`DCStoreRange`, DVD and ARAM transfers, `DCZeroRange`, copy write-backs) stamps the 4 KiB pages it covers.
  The display-list cache and the texture cache compare their source bytes by hash only when a stamp touched them, and every 30 (lists) or 8 (textures) frames regardless, for writes nothing reported; a list caught changing without a stamp is compared on every call from then on.
  `SMS_GX_HASH_ALWAYS=1` compares by hash on every use, as before the stamps.
- **Stream buffer.**
  Per-draw data (streamed vertices, indices, the XF block) goes into a 128 MiB ring that, with `ARB_buffer_storage` (GL 4.4; not macOS), stays mapped, so a batch costs no map and unmap. `SMS_GX_PERSISTENT_MAP=0` maps each batch's range instead.
- **EFB copy write-backs are encoded on the GPU**: a pass writes the copy's GX tile layout (scaled to the texels the game sees) and only those bytes are read back. `SMS_GX_COPY_VERIFY=1` also encodes each write-back on the CPU and logs any difference.
- **MSAA**: a copy or peek resolves only the region and buffers it reads, and nothing already resolved since the last draw.
- **Shader programs are kept on disk** (`gx_shader.cpp`): each linked program's binary (`glGetProgramBinary`) goes to `gx-programs-<32|64>.bin` in `$XDG_CACHE_HOME/sms-port` (`~/.cache/sms-port`; `~/Library/Caches/sms-port` on macOS; `%LOCALAPPDATA%\sms-port` on Windows), and the next session loads them all at its first draw, so materials seen before cost no compile (a stage's arrival compiles dozens, a hitch of up to 100 ms). The file starts over when the GL driver changes, and an entry is used only while its key still generates the same GLSL. `SMS_GX_SHADER_CACHE=path` puts it elsewhere; `=0` turns it off.
- **The GL thread** (`gx_glthread.cpp`).
  Once the context is ready it moves to a thread of its own, and the GL entry points become proxies: calls without results are queued, calls that return something wait for the GL thread, and pointers are copied (or are offsets into bound buffers).
  The driver's time for each draw and state change then leaves the game's thread.
  The GL thread sleeps when it has nothing to do and is woken once a few dozen KiB of commands have collected or when the game needs an answer.
  Presenting runs on it while the game's thread waits for the swap, as before, so frame pacing is unchanged.
  The results the game reads a frame late (copy write-backs, peek snapshots, pixel-metric queries) are fetched by the GL thread as soon as their frame is shown.
  `SMS_GX_GL_THREAD=0` keeps GL on the game's thread. 32-bit Windows always does (its GL entry points are `__stdcall`).

| Heavy plaza view, 120 fps, i5-6600K + GTX 1060 (Linux, 64-bit) | before | after |
| --- | --- | --- |
| 4:3, 1x | 9.5 ms/frame | 3.6 ms/frame |
| 16:9, 1x | 10.3 ms/frame | 3.9 ms/frame |
| 16:9, 2x, 4x MSAA | 11.6 ms/frame | 4.0 ms/frame |
| Eclipse, Fire Petey | 9.8 ms/frame | 3.7 ms/frame |

## Debugging aids

- `SMS_GX_TRACE_FRAME=n` (or `a-b`) writes every draw of display frame n (the draws after the n-th `GXCopyDisp`) to `$SMS_GX_DUMP_DIR/gx_trace_frame<n>.txt`, or to `$SMS_GX_TRACE_FILE` for a single frame.
  Each draw lists primitive and vertex counts, projection/viewport/scissor, z/blend/alpha-compare/fog state, lighting channels and the lights they use, texgens, every TEV stage decoded (`PREV = (ZERO + lerp(ZERO,TEXC,RASC))*1 clamp`), TEV/konst registers, swap tables, bound texture maps (address, format, size, wrap/filter/LOD, TLUT, whether it is an EFB copy) and the first vertices.
  EFB copies and `GXPeekZ` calls appear in order.
- `SMS_GX_TRACE_BT=1` adds to each traced draw the host call stack of its last `GXBegin`, and logs every projection load with its caller (`module(+offset)`; resolve with `addr2line -f -C -e build/linux-32/sms`).
- `SMS_GX_TRACE_PROBE=x,y;x,y` adds, after each traced draw, the EFB colour/alpha and depth at those points, which finds the draw that breaks a pixel.
- `SMS_GX_DUMP_EVERY=n` writes every n-th XFB as a PPM (see above).
- `SMS_GX_DUMP_SHADERS=dir` writes every generated program as `dir/prog<id>.vs/.fs`; traces name the program each draw used.
- `SMS_GX_STATS=n` logs, every n display frames, draws/vertices per frame, shader compiles, texture uploads, the milliseconds per frame spent in sms_gx (split into textures, batches, EFB copies, peeks and GPU waits), how many reads per frame made the CPU wait for the GPU, GL calls per frame (in total and for the most-called entry points), batch flushes, texture bytes hashed, vertex-loader time and idle time.
  Without it (and with the overlay closed) only the overall sms_gx time is measured; the other timers read the clock per primitive and per texture bind.
- `GXPC_GetTimes` gives the same breakdown as totals for the overlay; see `gx_pc.h`. With the GL thread, the game's waits for it count as GPU waits.
- `SMS_GX_COPY_VERIFY=1` checks every EFB copy write-back's GPU encoding against the CPU encoder (`encodeTexture`).
  On this machine the 32-bit build renders with Mesa llvmpipe (no 32-bit NVIDIA GL is installed), so GPU work shows up as CPU time at the first sync point, usually the EFB copy.

## Coverage against `api-surface.tsv`

All **181** GX (149) and GD (32) functions the game, JSystem and THPPlayer call are provided.
Eleven of them are header inlines or macros that work unchanged: the `GDBegin`, `GDEnd` and `GDSetCurrent` family, `GDOverflowCheck`, `GXEnd`, `GXSetTexCoordGen` and `GXInitLightShininess`.
`libsms_gx.a` defines 349 `GX*`/`GD*` symbols in total, which is the whole public SDK surface.

Of the 181 used functions, about **157 are fully implemented**.
The following are accepted, and their state is stored, but they have no or only partial rendering effect:

| Function | Status |
| --- | --- |
| `GXSetMisc` | no-op |
| `GXEnableBreakPt`, `GXDisableBreakPt` | no-op |
| `GXPokeAlphaRead` | no-op |
| `GXSetDither` | dithering is not emulated |
| `GXSetCopyFilter`, `GXSetDispCopyGamma` | stored, not applied to the XFB |
| `GXSetDispCopyFrame2Field`, `GXSetFieldMode` | no interlacing |
| `GXSetDispCopyYScale` | returns the line count; the XFB keeps the EFB height |
| `GXSetCoPlanar` | no-op |
| `GXInvalidateVtxCache` | no-op |
| `GXInitTexCacheRegion` | bookkeeping only; textures are read from RAM |
| `GXSetZTexture` | not rendered |
| `GXSetFogRangeAdj` | registers written; range adjustment not rendered |
| `GXSetLineWidth` | lines are 1 px, because core GL has no wide lines |
| `GXEnableTexOffsets`, `GXSetTexCoordCylWrap` | not rendered |
| `GXPixModeSync`, `GXSetDrawDone`, `GXWaitDrawDone`, `GXDrawDone` | the pipeline is synchronous; callbacks fire immediately |
| `GXGetCPUFifo`, `GXGetFifoPtrs` | report a static FIFO object |

## Tests

`build/gx_selftest` (CTest `gx_selftest`, run offscreen) passes 42/42 checks on both this machine's NVIDIA EGL device and Mesa llvmpipe:

- copy-clear, and a vertex-colour quad through `PASSCLR`
- copy clears respect colour/alpha/depth write enables and preserve backing alpha in RGB8, preventing opaque boat-shadow fragments at the airstrip (issue #33)
- early/late alpha rejection, shadow-volume depth blocking and ordered overlapping primitives, including the early-Z fallback
- pixel metrics with early Z on count only the pixels that pass the alpha test, also through the early-Z fallback
- a textured RGBA8 quad through a **2-stage TEV** (texture × rasterized colour, then × konst colour): exact 8-bit results
- an **EFB → texture copy** (RGBA8, and I8 through the luma conversion) sampled back
- alpha blending, alpha compare, subtractive blending and scissor
- an unclamped last TEV stage keeping its low 8 bits for the output and the alpha compare, as Bianco Hills' river surface needs (sms-launcher issue #60)
- a quad exactly on an orthographic camera's far plane, as the episode select's gradient background is
- a **GD-built display list** (decomp `GDSetCullMode`/`GDSetZMode`, raw BP writes and a draw command) through `GXCallDisplayList`
- raw write-gather-pipe BP writes via `GXPC_WGPipe`
- perspective projection with depth test, a lit channel (diffuse light, ambient × material) and `GXPeekZ`
- a display copy read back from the XFB
- CPU decoders for I4, RGB5A3, CMPR, and C8 with an RGB565 TLUT

```sh
cmake -S platform/gx -B platform/gx/build && make -C platform/gx/build -j2
platform/gx/build/gx_selftest               # offscreen
platform/gx/build/gx_selftest --window f.ppm   # shows the frame; dumps the EFB
```

The library also builds with `-m32`.
The 32-bit build needs the i386 SDL2 and EGL development packages for the window and headless paths.
Without them it still compiles, but only a host-supplied context (`GXPC_Init(getProc, scale)`) is available.

## Known gaps

- **Not verified against the game yet.**
  Nothing here has drawn a J2D screen from the real game, and the 3D paths (J3D display lists, skinning, indirect effects) are untested beyond the self-test.
- **Indirect texturing is approximate.**
  Offsets are computed in texel units with the static matrices.
  Dynamic S/T matrices (bump mapping) and the exact hardware fixed-point rounding are missing.
  Emboss texgen passes the source coordinate through.
- **Early-Z.**
  `GXSetZCompLoc(GX_TRUE)` on a depth-writing draw whose alpha test can reject uses `ARB_shader_image_load_store`'s early fragment tests so alpha-rejected pixels still write depth, as on GX. Contexts without the extension use ordered colour/depth passes per primitive for those draws. `SMS_GX_FORCE_EARLY_Z_FALLBACK=1` exercises that fallback. This keeps later shadow-volume masks behind foreground depth (issue #33).
  Other draws keep the late test even with early Z on: early fragment tests also count occlusion-query samples before the alpha test's `discard`, and the pollution counters draw with `ReInitializeGX`'s early Z, Z off and an alpha test (with every texel counted, Noki Bay's wall rocks never rose).
- **EFB copies are also written back to RAM.**
  Every texture copy is encoded into its GX tile layout on the GPU, read back and stored (the GL copy stays as the sampling fast path), because the game reads some on the CPU: Delfino's goop map (`TPollutionLayer::isPolluted`) is updated only by EFB copies. A `DCFlushRange`/`DCStoreRange` over a copy drops the GL copy so RAM wins again. `SMS_GX_COPY_WRITEBACK=0` turns write-back off; `SMS_GX_COPY_LOG=n` logs the first n write-backs.
- **Pixel metrics** (`GXClearPixMetric`/`GXReadPixMetric`) count samples that pass (a GL occlusion query) plus 4 per triangle, which the pollution counters subtract again; copy passes and the early-Z fallback's depth passes are not counted. A depth-writing, alpha-tested early-Z draw through the extension would count its alpha-rejected samples; the game reads metrics only around draws with Z off.
- **GPU reads arrive one frame late.**
  A synchronous read makes the CPU wait until the GPU has drawn everything queued, so the two stop overlapping (the plaza made 30–85 such reads a frame).
  As in Dolphin, the reads the game repeats every frame are answered from the same read one frame earlier: copy write-backs are stored a frame later (a pixel buffer and a fence), a pixel-metric pair is answered from the previous frame's query when it drew the same number of triangles, and each group of `GXPeekARGB`/`GXPeekZ` calls with no drawing in between is answered from the previous frame's 1× snapshot of the EFB.
  A read with no previous frame to use is still synchronous.
  Rendering is unchanged, but the game sees the goop map and pollution counts a frame later, so scripted runs can drift slightly from the synchronous ones.
  `SMS_GX_SYNC_READS=1` goes back to synchronous reads.
- **Textures written by the CPU as 16-bit words** in host byte order would decode with swapped bytes.
  Formats made of bytes (I4, I8, IA4, C8, RGBA8) are unaffected.
- **Pixel formats.**
  `RGB565_Z16` (AA) is treated like RGB8; RGBA6 is not quantized to 6 bits.
- **Other hardware features** are not emulated: TMEM preloading, the vertex cache, bounding boxes, performance counters, the scissor box offset, and the texture LOD differences between GX and GL.
