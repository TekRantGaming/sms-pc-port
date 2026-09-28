#!/usr/bin/env python3
"""Apply a VCDIFF delta (RFC 3284), as written by xdelta3, using only the
standard library.

    python3 vcdiff.py SOURCE DELTA OUTPUT

Supports the default code table, VCD_SOURCE and VCD_TARGET windows and
xdelta3's per-window Adler-32 checksum. Deltas that use secondary
compression (xdelta3 -S djw/lzma) or a custom code table are refused.
"""

import mmap
import os
import sys
import zlib

NOOP, ADD, RUN, COPY = 0, 1, 2, 3
S_NEAR, S_SAME = 4, 3


def _default_code_table():
    # RFC 3284 section 5.6
    t = [(RUN, 0, 0, NOOP, 0, 0)]
    t += [(ADD, s, 0, NOOP, 0, 0) for s in range(0, 18)]
    for mode in range(9):
        t.append((COPY, 0, mode, NOOP, 0, 0))
        t += [(COPY, s, mode, NOOP, 0, 0) for s in range(4, 19)]
    for mode in range(6):
        t += [(ADD, a, 0, COPY, c, mode) for a in range(1, 5) for c in range(4, 7)]
    for mode in range(6, 9):
        t += [(ADD, a, 0, COPY, 4, mode) for a in range(1, 5)]
    t += [(COPY, 4, mode, ADD, 1, 0) for mode in range(9)]
    assert len(t) == 256
    return t


CODE_TABLE = _default_code_table()


class VcdiffError(Exception):
    pass


def _varint(buf, pos):
    v = 0
    while True:
        b = buf[pos]
        pos += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v, pos


def _read_varint(f):
    v = 0
    while True:
        c = f.read(1)
        if not c:
            raise VcdiffError("truncated delta")
        b = c[0]
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v


def _decode_window(target, seg, slen, data, inst, addr):
    """Run one window's instructions, filling bytearray `target`."""
    near = [0] * S_NEAR
    same = [0] * (S_SAME * 256)
    next_slot = 0
    tpos = dpos = ipos = apos = 0
    ilen = len(inst)
    table = CODE_TABLE
    while ipos < ilen:
        t1, s1, m1, t2, s2, m2 = table[inst[ipos]]
        ipos += 1
        for typ, size, mode in ((t1, s1, m1), (t2, s2, m2)):
            if typ == NOOP:
                continue
            if size == 0:
                size, ipos = _varint(inst, ipos)
            if typ == ADD:
                target[tpos:tpos + size] = data[dpos:dpos + size]
                dpos += size
                tpos += size
            elif typ == RUN:
                target[tpos:tpos + size] = data[dpos:dpos + 1] * size
                dpos += 1
                tpos += size
            else:
                here = slen + tpos
                if mode == 0:
                    a, apos = _varint(addr, apos)
                elif mode == 1:
                    v, apos = _varint(addr, apos)
                    a = here - v
                elif mode < 2 + S_NEAR:
                    v, apos = _varint(addr, apos)
                    a = near[mode - 2] + v
                else:
                    a = same[(mode - 2 - S_NEAR) * 256 + addr[apos]]
                    apos += 1
                near[next_slot] = a
                next_slot = (next_slot + 1) % S_NEAR
                same[a % (S_SAME * 256)] = a
                if a >= here:
                    raise VcdiffError("copy address past the data decoded so far")
                n = size
                if a < slen:  # from the source segment, maybe running into the target
                    k = min(n, slen - a)
                    target[tpos:tpos + k] = seg[a:a + k]
                    tpos += k
                    n -= k
                    a = slen
                if n:
                    ta = a - slen
                    if ta + n <= tpos:
                        target[tpos:tpos + n] = target[ta:ta + n]
                        tpos += n
                    else:  # overlapping: repeats the pattern [ta, tpos)
                        while n:
                            k = min(n, tpos - ta)
                            target[tpos:tpos + k] = target[ta:ta + k]
                            tpos += k
                            n -= k
    if tpos != len(target) or dpos != len(data) or apos != len(addr):
        raise VcdiffError("window sections do not add up")


def apply(source_path, delta_path, out_path, progress=None):
    """Write out_path = delta applied to source_path. progress(done, total)
    is called with the delta bytes read so far."""
    total = os.path.getsize(delta_path)
    with open(source_path, "rb") as sf, open(delta_path, "rb") as df, open(out_path, "w+b") as of:
        smap = mmap.mmap(sf.fileno(), 0, access=mmap.ACCESS_READ) if os.path.getsize(source_path) else None
        src = memoryview(smap if smap is not None else b"")
        seg = b""
        try:
            if df.read(4) != b"\xd6\xc3\xc4\x00":
                raise VcdiffError("not a VCDIFF delta")
            hdr = df.read(1)[0]
            if hdr & 1:
                raise VcdiffError("secondary compression is not supported")
            if hdr & 2:
                raise VcdiffError("custom code tables are not supported")
            if hdr & 4:
                df.read(_read_varint(df))  # application header
            written = 0
            while True:
                c = df.read(1)
                if not c:
                    break
                win = c[0]
                slen = spos = 0
                seg = b""
                if win & 3:
                    slen = _read_varint(df)
                    spos = _read_varint(df)
                    if win & 1:
                        if spos + slen > len(src):
                            raise VcdiffError("source segment past the end of the source")
                        seg = src[spos:spos + slen]
                    else:
                        of.seek(spos)
                        seg = of.read(slen)
                        of.seek(written)
                _read_varint(df)  # length of the delta encoding
                tlen = _read_varint(df)
                if df.read(1)[0]:
                    raise VcdiffError("compressed window sections are not supported")
                dlen = _read_varint(df)
                ilen = _read_varint(df)
                alen = _read_varint(df)
                cksum = int.from_bytes(df.read(4), "big") if win & 4 else None
                data, inst, addr = df.read(dlen), df.read(ilen), df.read(alen)
                if len(data) + len(inst) + len(addr) != dlen + ilen + alen:
                    raise VcdiffError("truncated delta")
                target = bytearray(tlen)
                _decode_window(target, seg, slen, data, inst, addr)
                if cksum is not None and zlib.adler32(target) != cksum:
                    raise VcdiffError("checksum mismatch in the window at output offset %d" % written)
                of.write(target)
                written += tlen
                if progress:
                    progress(df.tell(), total)
        finally:
            if isinstance(seg, memoryview):
                seg.release()
            src.release()
            if smap is not None:
                smap.close()


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    apply(sys.argv[1], sys.argv[2], sys.argv[3])
