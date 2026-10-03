#!/usr/bin/env python3
"""Mirror decomp (and patched) sources as CP932 for compilers without CP932 conversion.

Apple Clang rejects -fexec-charset=CP932, and some portable GCC builds lack
iconv. Encoding the source tree as CP932 makes string literal bytes match the
disc (same as GCC's -fexec-charset=CP932).
"""
from __future__ import annotations

import argparse
import codecs
import sys
from pathlib import Path


def _keep_utf8(err: UnicodeEncodeError):
    # Characters CP932 lacks (em dashes etc.) only appear in decomp comments;
    # keep their UTF-8 bytes rather than fail the whole mirror.
    return err.object[err.start:err.end].encode("utf-8"), err.end


codecs.register_error("keep_utf8", _keep_utf8)


def encode_cp932(text: str) -> bytes:
    # Many Shift-JIS characters have 0x5C ('\\') as their second byte (ソ is
    # 83 5C). Written raw, the compiler reads that byte as an escape and drops
    # it ("GCコンソール" became "GCコンメ[ル"), so such characters are written
    # as octal escapes, which stop after three digits.
    out = bytearray()
    for ch in text:
        if ord(ch) < 0x80:
            out += ch.encode("ascii")
            continue
        b = ch.encode("cp932", errors="keep_utf8")
        if 0x5C in b[1:]:
            out += "".join("\\%03o" % x for x in b).encode("ascii")
        else:
            out += b
    return bytes(out)


def needs_cp932(data: bytes) -> bool:
    return any(b > 127 for b in data)


def mirror_file(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    data = src.read_bytes()
    if needs_cp932(data):
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            pass
        else:
            data = encode_cp932(text)
    if not dst.exists() or dst.read_bytes() != data:
        dst.write_bytes(data)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--decomp", type=Path, required=True)
    ap.add_argument("--patched", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    out: Path = args.out
    out.mkdir(parents=True, exist_ok=True)
    seen = set()

    def mirror(src: Path, dst: Path) -> None:
        mirror_file(src, dst)
        seen.add(dst)

    patched_root: Path = args.patched
    decomp: Path = args.decomp

    # The game's include/ and src/, and each library's libs/<name>/include
    # and libs/<name>/src, each mirrored at its own path.
    libs = sorted(p for p in (decomp / "libs").iterdir() if p.is_dir()) if (decomp / "libs").is_dir() else []
    include_roots = [Path("include")] + [p.relative_to(decomp) / "include" for p in libs]
    src_roots = [Path("src")] + [p.relative_to(decomp) / "src" for p in libs]

    for sub in include_roots:
        for path in (decomp / sub).rglob("*"):
            if not path.is_file():
                continue
            rel = path.relative_to(decomp)
            patched = patched_root / rel
            src = patched if patched.is_file() else path
            mirror(src, out / rel)

    for sub in src_roots:
        for path in (decomp / sub).rglob("*"):
            if not path.is_file():
                continue
            if path.suffix.lower() not in {".c", ".cpp", ".cc", ".h", ".hpp"}:
                continue
            rel = path.relative_to(decomp)
            patched = patched_root / rel
            src = patched if patched.is_file() else path
            mirror(src, out / rel)

    if patched_root.is_dir():
        for path in patched_root.rglob("*"):
            if not path.is_file():
                continue
            rel = path.relative_to(patched_root)
            dst = out / rel
            if dst not in seen:
                mirror(path, dst)

    for path in out.rglob("*"):
        if path.is_file() and path not in seen:
            path.unlink()

    print(f"CP932 mirror at {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
