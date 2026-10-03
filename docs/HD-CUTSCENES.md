# HD cutscenes

The optional HD pack replaces movie pixels while keeping the game's cutscene playback, subtitles, rumble, skip controls, frame rate and original compressed audio tracks.
The disc stays intact; movies without a replacement play normally.
The first completed AI pilot is the entire 68-second opening (`openingA.thp`), upscaled from 640×320 to 1920×960.
All 21 original THP files have been extracted locally; the other movies have not yet been AI-upscaled.

## Play

Build this version of the port with `SMS_ARCH=64 ./build.sh /path/to/GMSE01.iso` (see [build instructions](../BUILD.md)), then put generated replacements in `mods/hd-cutscenes/files/data/`, using the original movie filenames.
Run:

```sh
./run-hd-cutscenes.sh /path/to/GMSE01.iso
```

With a freshly built standalone executable, the disc argument is optional.
Use `SMS_ARCH=64` to select the 64-bit build on Linux when both builds exist.
The helper enables cutscenes even if they were disabled in settings.
Existing `SMS_MOD` entries are retained; the HD pack takes precedence for movies it replaces.
Run `./run.sh` normally to play without the HD pack.

For a pack elsewhere:

```sh
SMS_HD_CUTSCENES=/path/to/hd-cutscenes ./run-hd-cutscenes.sh
```

This machine's pilot pack is linked at `mods/hd-cutscenes`.
Its files and conversion work are stored at `/mnt/1tbhdd2/sms-hd-cutscenes-v1` to avoid filling the system disk.
The HD movies are local assets, separate from Git.

## Generate from your disc

Install Python 3 and FFmpeg.
For AI processing, also obtain the executable and model files from the official [Real-ESRGAN project](https://github.com/xinntao/Real-ESRGAN) and [ncnn Vulkan releases](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan/releases).
The pilot uses the ncnn executable from [v0.2.0](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan/releases/tag/v0.2.0), with the `realesr-animevideov3` models from the official [20220424 Ubuntu bundle](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip), scale 3.
It cleans up compressed edges and produces smoother surfaces, but reconstructs detail and can change fine textures or small facial features.
It is not a re-render from the original animation assets.
Lanczos mode provides a more conservative interpolation option.

Extract all THPs and a manifest with dimensions, timing, audio tracks and SHA-256 hashes:

```sh
python3 tools/media/cutscenes.py extract \
  --iso /path/to/GMSE01.iso --out /path/to/cutscene-work
```

Generate a full AI preview and a playable THP replacement:

```sh
python3 tools/media/cutscenes.py upscale \
  --input /path/to/cutscene-work/originals/openingA.thp \
  --output /path/to/cutscene-work/opening-ai-hd.mp4 \
  --method ai --scale 3 \
  --realesrgan /path/to/realesrgan-ncnn-vulkan \
  --models /path/to/models \
  --work /path/to/cutscene-work/opening-ai \
  --thp mods/hd-cutscenes/files/data/openingA.thp
```

Use `--ffmpeg /path/to/ffmpeg` if FFmpeg is not on PATH.
Omit the AI arguments and use `--method lanczos` for interpolation.
Choose fresh output filenames for a new run.
Work folders record the source hash and processing settings, so partial AI frame batches can be reused only for the same job.
Conversion logs stay in that folder; `--keep-frames` also retains the decoded and upscaled frames and intermediate MJPEG for inspection.
HD frame sets can use several gigabytes of temporary space.

The MP4 preview fits the original framing inside a 1920×1080 canvas and uses AAC audio.
The THP keeps its natural dimensions (1920×960 or 1920×1344 at scale 3), every source frame, and all original compressed audio packets without re-encoding.
The tool verifies the preview's decoded frame count and the THP's audio packet hash.

## Playback implementation

Host-only patches expand the SDK decoder's MCU row buffers to support aligned dimensions up to 2048×2048 and preserve pointer width in work-buffer alignment.
The host draw helper copies Y/U/V tile rectangles into a persistent low-address pool, then uses the original renderer with textures at most 960×960.
This avoids GX's 1024-pixel texture limit and its 32-bit texture address slots.
MovieDirector scales only its display-size copy of the header, preserving placement and subtitle geometry.
Original-sized movies follow the existing draw path.
No FFmpeg library or AI runtime is required by the game; those tools are used only when generating movies.

The standalone decoder check is:

```sh
make -C platform/thp/tests run ARCH=-m64 \
  DISC=/path/to/hd-cutscenes/files MOVIE=data/openingA.thp
```

The completed opening decodes all 2,049 frames and 2,189,735 audio samples (track 0), matching the original timeline.
The full compressed packet hash also covers the second audio track.

The tiled draw check covers every Y/U/V pixel and display boundary at 1920×960, 1920×1344, 1280×896 and 2048×2048, plus the unchanged 640×448 path:

```sh
make -C platform/thp/tests tiles ARCH=-m64
make -C platform/thp/tests tiles ARCH=-m32
```

Full opening playback completed in both Linux word sizes; matching captures were pixel-identical.
The ordinary plaza and audio checks also match the existing regression baseline on Mesa software rendering.
