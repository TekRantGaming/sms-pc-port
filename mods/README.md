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
DDS files can hold BC1–BC3 or BC7 blocks or plain RGBA; a GPU that cannot sample BC7 (macOS) gets them decoded.

A pack names each image after the texture it replaces (its size, format and a hash of its data), so it matches the game's own textures wherever they are loaded.
Replacements load in the background: a texture shows its original until its replacement has been read.
They look best with a larger internal resolution, for example `SMS_GX_SCALE=2`.

Switches: `SMS_TEXTURE_PACKS=dir;dir` uses those folders instead of `mods/textures/`, `SMS_TEXTURE_PACKS=0` turns packs off, and `SMS_TEXTURE_PACK_LOG=1` logs the pack name of every texture the game loads and whether it was replaced (for checking a pack or making one).

To make a pack, run with `SMS_TEXTURE_DUMP=dir`: every texture the game loads is written there once as a PNG under its pack name.
Edit or upscale the images, keep the names, and put them under `mods/textures/`.
Replacements take more video memory than the original textures: the UHD pack's plaza textures are about 16 times the size of the originals.
Past `SMS_TEXTURE_PACK_MB` (1536 by default), the replacements unused for longest are freed, and read again when needed.

## <name>/files/: game file mods

A mod that changes the game's files (models, stages, textures inside archives, text) goes in `mods/<name>/files/`, laid out like the disc's own `files/` folder, and is switched on with `mod = <name>` in `settings.txt` (or `SMS_MOD=<name>`; several as `a;b`, a later one winning).
Each file there takes the place of the disc's file at the same path, or is added to the disc if it has none; everything else still comes from your disc image.
The log names each mod and how many files it replaced and added.

This covers mods that only change data.
Mods that also change the game's code, such as Super Mario Eclipse, need that code ported too; see [docs/ECLIPSE.md](../docs/ECLIPSE.md).
