#!/usr/bin/env bash
# Packages a built port for release, without any game data:
#   Windows (MSYS2 MINGW64): dist/SMS-PC-Port-<version>-windows-x64.zip
#     sms.exe, its MinGW/SDL2 DLLs, settings.txt, bindings.txt, README
#   Linux: dist/SMS-PC-Port-<version>-linux-x86_64.AppImage (64-bit build)
#     made with linuxdeploy (downloaded into build/deps/ when missing)
# Usage: packaging/package.sh [version]   (after ./build.sh; SMS_ARCH=64 on Linux)
set -euo pipefail
cd "$(dirname "$0")/.."
version="${1:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}"
mkdir -p dist

readme() {
	cat <<EOF
Super Mario Sunshine - PC Port ($version)

This package contains only the port: no game data. You need your own disc
image of Super Mario Sunshine, North America (GMSE01), revision 0, as an ISO,
GCM, NKit ISO or Dolphin CISO.

1. Start the game. The launcher opens on its Install page.
2. Choose Browse... (or drop the image onto the window) and press Install.
3. Press Play.

The launcher's other pages set the display mode, resolution, anti-aliasing,
camera (invert, free camera, mouse look), audio and key bindings; they are
saved in settings.txt and bindings.txt. Hold Shift while starting the game to
show the launcher when it is turned off.

In game: F11 or Alt+Enter toggles fullscreen, F10 releases the mouse (mouse
look), \` shows the performance overlay, Esc quits.

HD texture packs made for Dolphin go in mods/textures/ (see the project's
mods/README.md).

Source: https://github.com/TekRantGaming/sms-pc-port
Dear ImGui is MIT licensed (LICENSE-imgui.txt).
EOF
}

case "$(uname -s)" in
MINGW* | MSYS*)
	bdir=build/windows-64
	name="SMS-PC-Port-$version-windows-x64"
	out="dist/$name"
	rm -rf "$out"
	mkdir -p "$out"
	strip -o "$out/sms.exe" "$bdir/sms.exe"
	# the DLLs it imports from the MinGW prefix (SDL2, the compiler runtime),
	# and theirs in turn, read from the import tables
	todo=("$out/sms.exe")
	while [ ${#todo[@]} -gt 0 ]; do
		f="${todo[0]}"
		todo=("${todo[@]:1}")
		for dll in $(objdump -p "$f" | awk '/DLL Name:/ {print $3}'); do
			if [ -f "/mingw64/bin/$dll" ] && [ ! -f "$out/$dll" ]; then
				cp "/mingw64/bin/$dll" "$out/"
				todo+=("$out/$dll")
			fi
		done
	done
	# bsdtar (libarchive) unpacks the HD texture pack's .7z for the launcher on
	# Windows builds whose own tar.exe cannot; its DLLs sit beside it in tools/
	mkdir -p "$out/tools"
	cp /mingw64/bin/bsdtar.exe "$out/tools/"
	todo=("$out/tools/bsdtar.exe")
	while [ ${#todo[@]} -gt 0 ]; do
		f="${todo[0]}"
		todo=("${todo[@]:1}")
		for dll in $(objdump -p "$f" | awk '/DLL Name:/ {print $3}'); do
			if [ -f "/mingw64/bin/$dll" ] && [ ! -f "$out/tools/$dll" ]; then
				cp "/mingw64/bin/$dll" "$out/tools/"
				todo+=("$out/tools/$dll")
			fi
		done
	done
	# the committed defaults, not this checkout's own settings
	git show HEAD:settings.txt > "$out/settings.txt"
	git show HEAD:bindings.txt > "$out/bindings.txt"
	cp platform/gx/third_party/imgui/LICENSE.txt "$out/LICENSE-imgui.txt"
	readme > "$out/README.txt"
	rm -f "dist/$name.zip"
	(cd dist && 7z a -tzip -mx=9 "$name.zip" "$name" >/dev/null)
	echo "dist/$name.zip"
	;;
Linux)
	bdir=build/linux-64
	[ -x "$bdir/sms" ] || { echo "build first: SMS_ARCH=64 ./build.sh" >&2; exit 1; }
	appdir=build/AppDir
	rm -rf "$appdir"
	mkdir -p "$appdir/usr/bin" "$appdir/usr/share/sms-port"
	strip -o "$appdir/usr/bin/sms" "$bdir/sms"
	git show HEAD:settings.txt > "$appdir/usr/share/sms-port/settings.txt"
	git show HEAD:bindings.txt > "$appdir/usr/share/sms-port/bindings.txt"
	cp platform/gx/third_party/imgui/LICENSE.txt "$appdir/usr/share/sms-port/LICENSE-imgui.txt"
	readme > "$appdir/usr/share/sms-port/README.txt"
	tool=build/deps/linuxdeploy-x86_64.AppImage
	if [ ! -x "$tool" ]; then
		mkdir -p build/deps
		curl -fsSL -o "$tool" \
			https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
		chmod +x "$tool"
	fi
	# CI runners have no FUSE: let the tool unpack itself
	export APPIMAGE_EXTRACT_AND_RUN=1
	export LDAI_OUTPUT="dist/SMS-PC-Port-$version-linux-x86_64.AppImage"
	export OUTPUT="$LDAI_OUTPUT"
	cp packaging/icon.png build/sms-pc-port.png
	# bsdtar (libarchive-tools) unpacks the HD texture pack's .7z for the launcher
	extra=()
	if command -v bsdtar >/dev/null; then extra=(--executable "$(command -v bsdtar)"); fi
	"$tool" --appdir "$appdir" \
		--executable "$appdir/usr/bin/sms" "${extra[@]}" \
		--desktop-file packaging/linux/sms-pc-port.desktop \
		--icon-file build/sms-pc-port.png \
		--custom-apprun packaging/linux/AppRun \
		--output appimage
	echo "$OUTPUT"
	;;
*)
	echo "packaging/package.sh: Windows (MSYS2 MINGW64) and Linux only" >&2
	exit 1
	;;
esac
