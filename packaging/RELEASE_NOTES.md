A native PC port of Super Mario Sunshine, built from the decompilation, with a launcher and PC enhancements.

### New in this release: HD cutscenes
- **All 21 movies at 3x resolution** (AI-enhanced, 1920 x 960), with their original timing, subtitles and audio. From the original project's HD cutscene release.
- On the launcher's **Graphics** page, **HD cutscenes → Download and install** downloads the movie patches (about 5.7 GB), builds the movies from your own disc image and checks every one against its published checksum. Nothing else needs installing. A switch then turns them on or off.
- The install is all-or-nothing: if it fails or you cancel it, you keep whatever you had before.
- **Brought up to date with the original project**: its audio fixes (excess reverb tails, abrupt sound endings and doubled footstep echoes) and the latest decompilation.

### Online co-op (first stage)
- A new **Online** page in the launcher: one player chooses **Host**, the others choose **Join** and enter the host's address, and everyone presses Play.
- Other players appear in your game when you are in the same level and episode, fully animated, with their cap, hands, FLUDD and nozzle, and a **name tag** over their head.
- Up to **8 players**. It works on a home network straight away. Over the internet, the host forwards UDP port 27016 (it can be changed) on their router.
- Everyone needs this version. Each player plays their own game for now: levels, enemies and Shine Sprites are not shared yet. Shared progress is the next stage.
- The launcher's sidebar now fits all of its pages on shorter screens.

### Earlier: HD texture pack installer
- **Download and install** on the Graphics page fetches the [Super Mario Sunshine UHD Texture Pack](https://github.com/qashto/Super_Mario_Sunshine_UHD_Texture_Pack) (qashto, razius) from its own release, with progress, resume and Cancel. You can then switch it on or off, reinstall it or remove it.

**These downloads contain no game data.** You need your own disc image of Super Mario Sunshine, North America (GMSE01), revision 0 (ISO, GCM, NKit ISO or Dolphin CISO). The launcher's Install page copies it into place.

### Downloads
- **Windows (64-bit):** `SMS-PC-Port-*-windows-x64.zip`. Unzip anywhere and run `sms.exe`.
- **Linux (64-bit):** `SMS-PC-Port-*-linux-x86_64.AppImage`. Make it executable (`chmod +x`) and run it. Settings, the installed disc image and mods live in `~/.local/share/sms-port`.

### Launcher
- **Install:** browse for or drag in your disc image. It is checked (game, region, revision) and copied into the game folder, or used where it is.
- **Display:** windowed, borderless or exclusive fullscreen (choose the resolution and refresh rate), monitor, vsync (off, on, adaptive), widescreen up to 32:9 or matched to your monitor, HUD position, aspect (keep, stretch, integer) and scaling filter.
- **Graphics:** internal resolution 1x to 8x (it recommends one for your monitor), MSAA 2x/4x/8x, FXAA, anisotropic filtering up to 16x, sharpening, brightness and HD texture packs.
- **Camera:** invert X and Y separately, a free camera that no longer swings back behind Mario (L recentres it), camera speed, and mouse look with sensitivity.
- **Gameplay, Audio and Controls:** 30 or 60 fps, skip intro movies, mods, performance overlay, master volume and keyboard rebinding. Controllers work automatically.

### In game
F11 or Alt+Enter toggles fullscreen. F10 releases the mouse when mouse look is on. `` ` `` shows the performance overlay. Esc quits.
