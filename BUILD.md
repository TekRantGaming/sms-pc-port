# Building and running

Every system uses the same scripts from the repository root:

```sh
./build.sh [IMAGE]     # build for this computer
./run.sh   [IMAGE]     # play
./clean.sh             # delete the build output
```

| System | Shell | Output folder | Executable | Standalone (built when an image is given) |
| --- | --- | --- | --- | --- |
| [Linux](#linux), 32-bit (default) | any | `build/linux-32/` | `sms` | `sms-standalone` |
| [Linux](#linux), 64-bit (`SMS_ARCH=64`) | any | `build/linux-64/` | `sms` | `sms-standalone` |
| [macOS](#macos) | any (Terminal) | `build/macos-64/` | `sms` | [`SMS.app`](#macos-app) |
| [Windows](#windows-msys2-mingw64) | MSYS2 MINGW64, or PowerShell with `build.cmd` / `run.cmd` | `build/windows-64/` | `sms.exe` | `sms-standalone.exe` |

- **The game comes from your disc image**: Super Mario Sunshine, North America (GMSE01), Rev 0, as `.iso`, `.gcm` or Dolphin `.ciso`.
  Put it in [`rom/`](rom/), pass its path, or set `SMS_DISC_IMAGE`.
  The port reads it in place; nothing is extracted or copied.
- **`./build.sh`** updates the pinned `decomp/` submodule, configures `build/<os>-<arch>/` with CMake and compiles `sms`.
  With an image (the argument, `SMS_DISC_IMAGE`, or the one image in `rom/`) it also builds the [standalone executable](#standalone-executable).
  `JOBS=n` limits parallel compiler jobs (default: all cores).
  The first build compiles about 600 game files; later builds only rebuild what changed.
- **`./run.sh`** runs `build/<os>-<arch>/`.
  The game source is, in order: an image or extracted `files/` folder passed as argument, `SMS_DISC_IMAGE` or `SMS_DISC_ROOT`; else the standalone executable if it was built; else the image in `rom/`.
  Anything starting with `-` (such as `--headless`) goes to the game.
- **`SMS_ARCH=32` or `64`** picks the word size for both scripts.
  Linux builds either (32-bit is the default); macOS builds only 64-bit. The launcher runs on 64-bit computers and offers both 32-bit and 64-bit games on Linux and Windows, defaulting to 64-bit. Its private tools include cross compilers and target libraries, so manual prerequisites below are not needed when using the launcher.
  When both Linux builds exist, `./run.sh` takes 32-bit unless `SMS_ARCH=64` is set; when only one exists, it takes that one.
- **`./clean.sh`** deletes the build output: every `build/<os>-<arch>/` folder (with its standalone executable or `SMS.app`), captures in `build/`, and the build folders of older layouts (`build-mac/`, `build-64/`, `build32/`).
  It never deletes a disc image: `rom/` is never touched, an image left in an old build folder's `rom/` is moved to `rom/` first, and a folder that still holds an image is skipped.
  It also keeps the downloaded SDL2 in `build/deps/` and `tools/mkpatch.sh` edits in `build/patchwork/`.
  `--dry-run` lists what it would delete; `--all` deletes all of `build/`; `SMS_ARCH=64 ./clean.sh` deletes only that build.
  Everything generated lives under `build/`, so `./clean.sh --all` returns the checkout to a fresh clone plus your `rom/`.

## Linux

Ubuntu or Debian packages; other distributions need the same tools and libraries.

For the **32-bit build** (the default), add the i386 architecture and install the multilib compiler and the 32-bit runtime libraries:

```sh
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install git cmake make python3 patch binutils gcc-multilib g++-multilib libsdl2-dev libegl-dev libgl-dev
sudo apt install libsdl2-2.0-0:i386 libgl1:i386 libegl1:i386 libgl1-mesa-dri:i386 libegl-mesa0:i386
```

The build compiles against the (architecture-independent) headers of the 64-bit `-dev` packages and links the `:i386` runtime libraries directly, so no `:i386` `-dev` packages are needed.

For the **64-bit build**, the first `apt install` line without `gcc-multilib g++-multilib` is enough:

```sh
sudo apt install git cmake make python3 patch binutils g++ libsdl2-dev libegl-dev libgl-dev
SMS_ARCH=64 ./build.sh
SMS_ARCH=64 ./run.sh
```

Then build and run:

```sh
./build.sh
./run.sh
```

`./run.sh --headless` (or `SMS_HEADLESS=1`) renders offscreen through EGL with no window.

## macOS

macOS builds an **x86_64** program, which runs natively on Intel Macs and under **Rosetta 2** on Apple Silicon.
Native arm64 cannot `mmap` below 4 GiB (PAGEZERO), and the port needs game memory and stacks there for pointer-in-`u32` slots.
No Intel Homebrew is required: the script uses Homebrew's LLVM `clang` targeting x86_64 (`-arch x86_64`), normal Homebrew tools, and a universal `SDL2.framework`.

### One-time setup

```sh
xcode-select --install              # if needed
softwareupdate --install-rosetta    # Apple Silicon only
brew install cmake python3 llvm
```

| Requirement | Why |
| --- | --- |
| Xcode Command Line Tools | macOS SDK and system headers, `make`, `patch` |
| Rosetta 2 (Apple Silicon) | runs the x86_64 program |
| Homebrew (`/opt/homebrew` on Apple Silicon is fine) | `cmake`, `python3`, `llvm` |
| `llvm` | the compiler (`clang` / `clang++` from `$(brew --prefix llvm)/bin`, the one the macOS build is tested with) and `llvm-objcopy`, which renames the game's `operator new/delete` in Mach-O archives |
| Universal `SDL2.framework` | window, input and audio; `./build.sh` downloads it into `build/deps/` (Homebrew's `sdl2` bottle is arm64-only on Apple Silicon). It is copied beside `build/macos-64/sms` and into `SMS.app`; the program finds it only there (`@executable_path`, `@executable_path/../Frameworks`) |

Clang has no `-fexec-charset=CP932`, so configuring mirrors the game sources as CP932 (`tools/darwin_cp932_mirror.py`) so the disc's Shift-JIS names still match.
The linker uses `-Wl,-pagezero_size,0x1000` so low-memory `mmap` works on x86_64.

### Build and run

```sh
./build.sh
./run.sh
```

With the image in `rom/`, `./build.sh` also builds `build/macos-64/SMS.app` (see [macOS app](#macos-app)), and `./run.sh` runs it in the terminal so its log shows.

### Known gaps

| Gap | Why it matters |
| --- | --- |
| Native arm64 | not supported: PAGEZERO blocks low `mmap` |
| Lockstep `platform/trace` | Linux ELF only; macOS links empty stubs (as Windows does) |
| Headless (EGL) | not wired on macOS; the SDL2 window is the only mode |

### macOS app

`./build.sh /path/to/GMSE01.iso` (or with the image in `rom/`) also builds `build/macos-64/SMS.app` (about 1.1 GiB):

| Path in the bundle | Contents |
| --- | --- |
| `Contents/MacOS/sms` | the port executable |
| `Contents/Frameworks/SDL2.framework` | SDL2, so the other Mac needs no SDL install |
| `Contents/Resources/disc.gcm` | the game's files, packed by `tools/bundle_disc.py --image-only` |
| `Contents/Resources/SMS.icns` | the icon: the game's memory-card Mario head, taken from the disc by `tools/extract_icon.py` |

`tools/make_mac_app.sh` assembles it and signs it ad hoc.
The image lives in `Resources` rather than after the executable (as `sms-standalone` does on Linux and Windows) because appended data would break the code signature.
Double-click it in Finder, or run `./run.sh` with no disc argument to run it in the terminal so its log shows.

To move it to another Mac of yours, zip it with `ditto` (it keeps the framework's symlinks):

```sh
ditto -c -k --keepParent build/macos-64/SMS.app SMS.zip
```

The app has no Developer ID signature, so macOS blocks a downloaded copy ("SMS is damaged" or "cannot be verified").
After unzipping, run once:

```sh
xattr -dr com.apple.quarantine /path/to/SMS.app
```

On Apple Silicon, macOS offers to install Rosetta 2 on first launch if it is missing.
Saves go to `~/.local/share/sms-port/card-a`, the same place as the terminal build.
Keep the app private: it contains the whole game and the icon art from your disc, so sharing it is sharing the game.

## Windows (MSYS2 MINGW64)

Install [MSYS2](https://www.msys2.org/) and open **MSYS2 MINGW64** from the Start menu.
The default Windows build is native x86_64. Its game memory, executable image, and game thread stacks stay below 4 GiB because retail stores addresses in 32-bit slots. The process itself uses 64-bit pointers and x64 SDL2/compiler runtime DLLs. A 32-bit build is also available with `SMS_ARCH=32` in a MINGW32 shell, or with the launcher’s x64-host cross compiler from MINGW64 (`SMS_WINDOWS_32_CROSS=1`).
Install the compiler, SDL2, build tools and `patch`:

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-SDL2 mingw-w64-x86_64-ninja mingw-w64-x86_64-make mingw-w64-x86_64-python patch git
```

In the MINGW64 shell, enter this repository (for example `cd /c/path/to/sms-pc-port`), then build and run:

```sh
./build.sh
./run.sh
```

Use single quotes around paths with spaces or parentheses: `./run.sh '/c/Games/Super Mario Sunshine (US).iso'`.
Keep the MINGW64 shell open while playing so its SDL2 and compiler runtime DLLs are on `PATH`.
Saves go to `%APPDATA%\sms-port\card-a` (or `SMS_SAVE_DIR`).
Headless mode (EGL) is not available on Windows; the SDL2 window is the only mode.

### From PowerShell or Command Prompt

The `.sh` files are Bash scripts, so do not open them through Windows file associations or Git for Windows.
`build.cmd`, `run.cmd` and `clean.cmd` start MSYS2's MINGW64 Bash for you and put its 64-bit DLLs on `PATH`:

```powershell
.\build.cmd
.\run.cmd
.\run.cmd 'C:\Games\Super Mario Sunshine (US).iso'
.\clean.cmd
```

If MSYS2 is not installed at `C:\msys64`, set `MSYS2_ROOT` to its installation folder first.

### Decompilation build on Windows

The decompilation is a separate GameCube build that produces `mario.dol`, which runs in Dolphin or on GameCube hardware; the PC port does not need it.
Use **PowerShell** with native Windows Python and Ninja; see the decomp README for installation.
In `sms-english` (or this repository's `decomp/` submodule), place your GMSE01 Rev 0 image in `orig/GMSE01/`, then run:

```powershell
python configure.py --version GMSE01
ninja
```

If Ninja is installed through MSYS2 but is not on PowerShell's `PATH`, run `C:\msys64\mingw64\bin\ninja.exe` in place of `ninja`.
The output is `build/GMSE01/mario.dol`; it should match the original disc's DOL byte for byte.
The decomp downloads its own GameCube toolchain; the MINGW64 GCC compiler is only for the PC port.
The `sms-english` [README](https://github.com/chasem-dev/sms-english/blob/main/README.md) has the native Windows setup details.

## Standalone executable

Give the build script your disc image (or leave it in `rom/`) to also get an executable with the game's files inside:

| System | Standalone |
| --- | --- |
| Linux | `build/linux-32/sms-standalone` (`build/linux-64/` with `SMS_ARCH=64`) |
| macOS | `build/macos-64/SMS.app` (see [macOS app](#macos-app)) |
| Windows | `build/windows-64/sms-standalone.exe` |

`tools/bundle_disc.py` reads the image (`.iso`, `.gcm` or Dolphin `.ciso`), checks that it is GMSE01, and packs the disc's files into a trimmed disc image with no padding (about 1.1 GiB).
It appends that image to a copy of `sms`, followed by a small trailer that `platform/disc` finds when the program starts.
`sms` itself is unchanged and still takes a disc image.
The standalone executable needs no image, no `rom/` folder and no extracted files, and runs from any folder.
On Linux it still needs the system's SDL2 (`libsdl2`); on Windows its MinGW and SDL2 DLLs must be next to it or on `PATH`.
On macOS the build produces [`SMS.app`](#macos-app) instead, with SDL2 and the packed image inside the bundle.
`./run.sh` without a disc argument starts it when it exists; a disc argument, `SMS_DISC_IMAGE` or `SMS_DISC_ROOT` still takes precedence over the bundled files.
Keep the executable private: it contains the game.

### Icons

The icon is the game's memory-card icon (the Mario head), always taken from your disc, never stored in this repository:

| Where | How |
| --- | --- |
| Window, taskbar and Dock, every platform | `platform/misc/window_icon.cpp` decodes it from the game source at startup and hands it to SDL |
| `SMS.app` in Finder and the Dock | `Contents/Resources/SMS.icns`, written by `tools/extract_icon.py --icns` |
| `sms.exe` / `sms-standalone.exe` in Explorer | an icon resource from `tools/extract_icon.py --ico`, compiled in when the build script is given the disc image |

Linux executables carry no icon of their own; the window icon is what the desktop shows.

## Manual CMake build

`build.sh` is a thin wrapper around CMake; this is what it runs (Linux, 32-bit):

```sh
git submodule update --init decomp
cmake -S . -B build/linux-32 -DSMS_ARCH=32 -DSMS_GX_BUILD_TESTS=OFF
cmake --build build/linux-32 --target sms --parallel
```

| CMake option | Meaning |
| --- | --- |
| `-DSMS_ARCH=32` / `64` | word size (`auto`, the default for a bare CMake build, picks 32 when `-m32` links) |
| `-DSMS_BUNDLE_DISC=/path/GMSE01.iso` | adds the `sms_standalone` target (`SMS.app` on macOS) |
| `-DSMS_GX_BUILD_TESTS=ON` | also builds `platform/gx`'s self-tests |
| `-G Ninja` | used on Windows (MSYS2) |
| macOS: `-DCMAKE_OSX_ARCHITECTURES=x86_64 -DSMS_SDL2_FRAMEWORK=$PWD/build/deps/SDL2.framework -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++` | required; then copy `SDL2.framework` beside `sms` (`@executable_path`) |

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `g++ -m32 does not link` | install `gcc-multilib g++-multilib` ([Linux](#linux)), or build 64-bit with `SMS_ARCH=64` |
| `No game: put your GMSE01 disc image ... in rom/` | put the image in `rom/`, or pass its path to `./run.sh` |
| `rom/ holds more than one disc image` | keep one image in `rom/`, or pass the one you want |
| Linux, 32-bit: the window is slow and the log names `llvmpipe` | the i386 GPU driver userspace (for example NVIDIA's) is missing or does not match the kernel driver, so rendering falls back to Mesa's software renderer; install the matching `:i386` driver libraries, or use the 64-bit build |
| macOS: `Rosetta 2 is required` | `softwareupdate --install-rosetta` |
| macOS: `Missing llvm-objcopy` | `brew install llvm` |
| macOS: "SMS is damaged" / "cannot be verified" on a copied `SMS.app` | `xattr -dr com.apple.quarantine /path/to/SMS.app` |
| Windows: `Use the MSYS2 MINGW64 shell` | open **MSYS2 MINGW64** (not MSYS or UCRT64), or use `build.cmd` / `run.cmd` |
| Windows: missing DLL when starting `sms.exe` directly | start it from the MINGW64 shell or with `run.cmd` |
| Stale or broken build after pulling | `./clean.sh` and `./build.sh` again (`rom/` and `build/deps/` are kept) |
