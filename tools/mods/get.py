#!/usr/bin/env python3
"""Download and install optional mods, fresh each time.

    python3 tools/mods/get.py textures    the Super Mario Sunshine UHD Texture Pack
    python3 tools/mods/get.py eclipse     Super Mario Eclipse (patched from your disc)
    python3 tools/mods/get.py all         both

Each mod's previous install is removed first, then its release is downloaded
from where its authors publish it, checked against a known checksum, and
installed under mods/. Nothing of either mod is part of this repository.

Options:
    --iso PATH        your Super Mario Sunshine image (North America, the
                      unmodified 1:1 ISO), for Eclipse; by default the one
                      named by SMS_DISC_IMAGE or disc_image in settings.txt,
                      or the one in rom/
    --keep-download   keep the downloaded archives in mods/.downloads/

Needs Python 3 and 7-Zip (7z, 7zz or 7za on PATH).
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vcdiff  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
MODS = os.path.join(ROOT, "mods")
DOWNLOADS = os.path.join(MODS, ".downloads")

# Pinned releases. To move to a newer one, change the URL and checksums
# together (the log of a failed check prints the new checksum).
TEXTURES = {
    "name": "Super Mario Sunshine UHD Texture Pack v2.1.1 (qashto, razius)",
    "page": "https://github.com/qashto/Super_Mario_Sunshine_UHD_Texture_Pack",
    "url": "https://github.com/qashto/Super_Mario_Sunshine_UHD_Texture_Pack/releases/download/2.1.1/GMS.7z",
    "file": "GMS.7z",
    "md5": "274985dba9214b857b3e5605d2ee49b2",
    "size": 986431331,
    "unpacked": 3218563135,
    "install": os.path.join(MODS, "textures", "GMS"),
}
ECLIPSE = {
    "name": "Super Mario Eclipse v1.1.0 (Eclipse Team)",
    "page": "https://gamebanana.com/mods/536309",
    "url": "https://gamebanana.com/dl/1729332",
    "file": "super_mario_eclipse_v110.7z",
    "md5": "37c1805ab88b2a3bb2a96bcdf6884433",
    "size": 892257115,
    "patch": "patches/v1.1.0.xdelta",
    "patch_size": 1191516980,
    "source_md5": "0c6d2edae9fdf40dfc410ff1623e4119",  # GMSE01, as Eclipse's own patcher requires
    "result_md5": "caa546309e0443f7b47632b040a250d7",
    "install": os.path.join(MODS, "eclipse"),
    "iso": "Super Mario Eclipse v1.1.0.iso",
}


class Failure(Exception):
    pass


def say(msg):
    print(msg, flush=True)


def mib(n):
    return "%.0f MiB" % (n / 1048576.0)


def remove(path):
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    elif os.path.lexists(path):
        os.remove(path)


def need_space(path, nbytes):
    os.makedirs(path, exist_ok=True)
    free = shutil.disk_usage(path).free
    if free < nbytes:
        raise Failure("needs %s free under %s, and %s is" % (mib(nbytes), path, mib(free)))


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url, dest):
    req = urllib.request.Request(url, headers={"User-Agent": "sms-pc-port mod installer"})
    part = dest + ".part"
    with urllib.request.urlopen(req) as r, open(part, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done, last = 0, 0.0
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            now = time.time()
            if now - last > 1.0 and sys.stdout.isatty():
                last = now
                pct = " (%d%%)" % (100 * done // total) if total else ""
                print("\r  %s%s   " % (mib(done), pct), end="", flush=True)
    if sys.stdout.isatty():
        print("\r" + " " * 30 + "\r", end="")
    os.replace(part, dest)


def fetch(mod):
    os.makedirs(DOWNLOADS, exist_ok=True)
    dest = os.path.join(DOWNLOADS, mod["file"])
    remove(dest)
    say("Downloading %s\n  from %s" % (mod["name"], mod["url"]))
    try:
        download(mod["url"], dest)
    except OSError as e:
        raise Failure("download failed: %s" % e)
    got = md5_of(dest)
    if mod["md5"] and got != mod["md5"]:
        remove(dest)
        raise Failure("the download does not match the expected release (MD5 %s, expected %s);"
                      " see %s" % (got, mod["md5"], mod["page"]))
    say("  %s, MD5 %s" % (mib(os.path.getsize(dest)), got))
    return dest


def seven_zip():
    for name in ("7z", "7zz", "7za"):
        exe = shutil.which(name)
        if exe:
            return exe
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)")):
        if base and os.path.isfile(os.path.join(base, "7-Zip", "7z.exe")):
            return os.path.join(base, "7-Zip", "7z.exe")
    raise Failure("7-Zip is needed to unpack the download: install it (Linux: the 7zip or"
                  " p7zip-full package; macOS: brew install sevenzip; Windows: 7-zip.org)")


def extract(archive, dest, members=()):
    remove(dest)
    os.makedirs(dest)
    say("Unpacking %s" % os.path.basename(archive))
    r = subprocess.run([seven_zip(), "x", "-y", "-o" + dest, archive] + list(members),
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        raise Failure("7-Zip could not unpack %s:\n%s" % (archive, r.stderr.strip()))


def settings_disc():
    for path in (os.path.join(ROOT, "settings.txt"),):
        try:
            with open(path, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line.startswith("#") or "=" not in line:
                        continue
                    k, v = (s.strip() for s in line.split("=", 1))
                    if k in ("disc_image", "SMS_DISC_IMAGE") and v:
                        return v
        except OSError:
            pass
    return None


def find_iso(explicit):
    if explicit:
        return explicit
    for cand in (os.environ.get("SMS_DISC_IMAGE"), settings_disc()):
        if cand:
            return cand
    rom = os.path.join(ROOT, "rom")
    if os.path.isdir(rom):
        images = sorted(n for n in os.listdir(rom) if n.lower().endswith((".iso", ".gcm")))
        if len(images) == 1:
            return os.path.join(rom, images[0])
        if len(images) > 1:
            raise Failure("rom/ holds several images; name yours with --iso")
    raise Failure("no Super Mario Sunshine image found: pass --iso PATH (or put it in rom/)")


def get_textures(keep):
    mod = TEXTURES
    say("== Textures: %s" % mod["name"])
    seven_zip()  # before downloading anything
    if os.path.isdir(mod["install"]):
        say("Removing the previous install, %s" % os.path.relpath(mod["install"], ROOT))
        remove(mod["install"])
    need_space(MODS, mod["size"] + mod["unpacked"] + (64 << 20))
    archive = fetch(mod)
    tmp = os.path.join(DOWNLOADS, "textures")
    try:
        extract(archive, tmp)
        # the release holds GMS/Textures/GMS/<folders of tex1_* files>
        src = os.path.join(tmp, "GMS", "Textures", "GMS")
        if not os.path.isdir(src):
            raise Failure("the archive is not laid out as expected (no GMS/Textures/GMS)")
        os.makedirs(os.path.dirname(mod["install"]), exist_ok=True)
        shutil.move(src, mod["install"])
        count = sum(1 for _, _, files in os.walk(mod["install"])
                    for n in files if n.startswith("tex1_"))
    finally:
        remove(tmp)
        if not keep:
            remove(archive)
    say("Installed %d textures in %s." % (count, os.path.relpath(mod["install"], ROOT)))
    say("The port uses them on its next start; see mods/README.md for the memory budget"
        " and SMS_GX_SCALE for a higher internal resolution.")


def get_eclipse(keep, iso_arg):
    mod = ECLIPSE
    say("== Eclipse: %s" % mod["name"])
    seven_zip()
    iso = find_iso(iso_arg)
    if not os.path.isfile(iso):
        raise Failure("no such image: %s" % iso)
    say("Checking your image, %s" % iso)
    got = md5_of(iso)
    if got != mod["source_md5"]:
        raise Failure("Eclipse patches only the North American ISO with MD5 %s, and this one is %s"
                      " (compressed or trimmed images such as CISO, NKit and RVZ do not work)"
                      % (mod["source_md5"], got))
    if os.path.isdir(mod["install"]):
        say("Removing the previous install, %s" % os.path.relpath(mod["install"], ROOT))
        remove(mod["install"])
    need_space(MODS, mod["size"] + mod["patch_size"] + os.path.getsize(iso) + (64 << 20))
    archive = fetch(mod)
    tmp = os.path.join(DOWNLOADS, "eclipse")
    out = os.path.join(mod["install"], mod["iso"])
    try:
        extract(archive, tmp, [mod["patch"], "readme.md", "changelog.md", "license.md"])
        if not keep:
            remove(archive)
        os.makedirs(mod["install"])
        say("Patching your image (a minute or two)")
        last = [0.0]

        def progress(done, total):
            now = time.time()
            if sys.stdout.isatty() and now - last[0] > 1.0:
                last[0] = now
                print("\r  %d%%   " % (100 * done // total), end="", flush=True)

        try:
            vcdiff.apply(iso, os.path.join(tmp, mod["patch"]), out + ".part", progress)
        except vcdiff.VcdiffError as e:
            raise Failure("patching failed: %s" % e)
        if sys.stdout.isatty():
            print("\r        \r", end="")
        got = md5_of(out + ".part")
        if got != mod["result_md5"]:
            raise Failure("the patched image is not the expected one (MD5 %s, expected %s)"
                          % (got, mod["result_md5"]))
        os.replace(out + ".part", out)
        shutil.copy(os.path.join(tmp, "changelog.md"), os.path.join(mod["install"], "changelog.md"))
    except BaseException:
        remove(mod["install"])
        raise
    finally:
        remove(tmp)
        if not keep:
            remove(archive)
    say("Installed %s." % os.path.relpath(out, ROOT))
    say("It is the same image Eclipse's own patcher makes, for Dolphin or a console. The port"
        " cannot run it yet: Eclipse also changes the game's code, which has to be ported first"
        " (docs/ECLIPSE.md).")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n\n".join(__doc__.split("\n\n")[1:]))
    ap.add_argument("what", choices=["textures", "eclipse", "all"])
    ap.add_argument("--iso")
    ap.add_argument("--keep-download", action="store_true")
    args = ap.parse_args()
    try:
        if args.what in ("textures", "all"):
            get_textures(args.keep_download)
        if args.what in ("eclipse", "all"):
            get_eclipse(args.keep_download, args.iso)
    except Failure as e:
        sys.exit("error: %s" % e)
    except KeyboardInterrupt:
        sys.exit("stopped")
    finally:
        try:
            os.rmdir(DOWNLOADS)  # only when empty
        except OSError:
            pass


if __name__ == "__main__":
    main()
