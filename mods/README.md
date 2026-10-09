# mods/

Optional additions to the game, each switched on by putting it here.
Nothing in this folder is needed to play, and git ignores everything in it but this file.

## Installing with get.py

`tools/mods/get.py` downloads a mod from where its authors publish it and installs it here, removing that mod's previous install first:

```sh
python3 tools/mods/get.py textures    # the UHD texture pack, into mods/textures/GMS
python3 tools/mods/get.py eclipse     # Super Mario Eclipse, patched from your disc, into mods/eclipse
python3 tools/mods/get.py all
```

It needs 7-Zip (`7z`, `7zz` or `7za`) to unpack the downloads, and checks each against the release it expects.
The texture pack is about 1 GB to download and 3 GB installed.
Eclipse is about 850 MB to download; it is an xdelta patch that turns your own North American ISO (found as `--iso PATH`, `SMS_DISC_IMAGE`, `disc_image` in `settings.txt`, or the image in `rom/`) into a Super Mario Eclipse ISO, which the installer checks against the expected result.
`--keep-download` keeps the downloaded archives in `mods/.downloads/`.

## textures/: HD texture packs

Texture packs made for Dolphin's "Load Custom Textures" work unchanged.
Unpack one into `mods/textures/` (or let `get.py textures` install the UHD pack), for example the [Super Mario Sunshine UHD Texture Pack](https://github.com/qashto/Super_Mario_Sunshine_UHD_Texture_Pack) (from its `GMS.7z` release, `GMS/Textures/GMS` goes to `mods/textures/GMS`).
Every `tex1_*.png` and `tex1_*.dds` below `mods/textures/` is used, in any sub-folder; several packs can sit side by side.
DDS files can hold BC1–BC3 or BC7 blocks or plain RGBA. Supported blocks stay compressed, including files with only one or a few mipmap levels: the missing levels are made from the last one in the background after the level loads and compressed in the same format, so far-away surfaces do not shimmer (until they arrive, about a second for an 8192×8192 image, sampling stops at the file's last level). A GPU that cannot sample BC7 (macOS) gets them decoded instead.

A pack names each image after the texture it replaces (its size, format and a hash of its data), so it matches the game's own textures wherever they are loaded.
Replacements in loaded BTI files, models and particle resources prepare in the background, including textures on offscreen objects. Before the first gameplay frame, the render thread finishes uploading the queued replacements. This adds some loading time and avoids doing those uploads when the camera first sees an object.
Textures created or fetched later show their original until the replacement is ready. Normal draws only request or bind replacements; uploads run at frame boundaries, with soft limits of 16 MiB and 2 ms between textures. One large upload can exceed these limits.
They look best with a larger internal resolution, for example `SMS_GX_SCALE=2`.

Switches: `SMS_TEXTURE_PACKS=dir;dir` uses those folders instead of `mods/textures/`, `SMS_TEXTURE_PACKS=0` turns packs off, and `SMS_TEXTURE_PACK_LOG=1` logs the pack name of every texture the game loads and whether it was replaced (for checking a pack or making one).

To make a pack, run with `SMS_TEXTURE_DUMP=dir`: every texture the game loads is written there once as a PNG under its pack name.
Edit or upscale the images, keep the names, and put them under `mods/textures/`.
Replacements take more video memory than the original textures; their cost depends on resolution and GPU compression support.
Past `SMS_TEXTURE_PACK_MB` (1536 by default), the replacements unused for longest are freed, and read again when needed.
The decoder pauses when images awaiting upload reach `SMS_TEXTURE_PACK_PENDING_MB` (256 by default), allowing at most one additional decoded result. This budget excludes decode scratch space and GPU memory. `SMS_TEXTURE_PACK_PRELOAD=0` disables resource preloading; `SMS_TEXTURE_PACK_SYNC=1` restores synchronous first-use loading for repeatable captures and bypasses preloading and upload limits. The matching settings are `texture_pack_preload` and `texture_pack_pending_mb` in `settings.txt`.

## <name>/files/: game file mods

A mod that changes the game's files (models, stages, textures inside archives, text) goes in `mods/<name>/files/`, laid out like the disc's own `files/` folder, and is switched on with `mod = <name>` in `settings.txt` (or `SMS_MOD=<name>`; several as `a;b`, a later one winning).
Each file there takes the place of the disc's file at the same path, or is added to the disc if it has none; everything else still comes from your disc image.
The log names each mod and how many files it replaced and added.

This covers mods that only change data.
Mods that also change the game's code, such as Super Mario Eclipse, need that code ported too; see [docs/ECLIPSE.md](../docs/ECLIPSE.md).
