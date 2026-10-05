#!/usr/bin/env python3
"""HD movie patches: enhanced video frames plus original-disc audio and metadata.

The SMP1 format stores a JSON identity header and length-prefixed THP JPEGs.
Applying it requires the exact original movie; no audio is shipped in patches.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct

from cutscenes import audio_packets, metadata
from bundle_disc import be32

MAGIC = b'SMP1'
MAX_FRAME = 32 << 20


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def exact(f, size):
    data = f.read(size)
    if len(data) != size:
        raise ValueError('Truncated HD movie patch')
    return data


def layout(data):
    meta = metadata(data)
    comp = be32(data, 0x20)
    kinds = list(data[comp+4:comp+4+be32(data, comp)])
    if kinds[0] != 0 or kinds.count(0) != 1 or be32(data, 0x24):
        raise ValueError('Expected a non-indexed video-first THP')
    return meta, comp, kinds


def frames(data, meta, kinds):
    offset, size = be32(data, 0x28), be32(data, 0x18)
    for _ in range(meta['frames']):
        if size < 8+4*len(kinds) or offset+size > len(data):
            raise ValueError('Invalid THP frame chain')
        sizes = list(struct.unpack_from('>'+'I'*len(kinds), data, offset+8))
        cursor, packets = offset+8+4*len(kinds), []
        for kind, n in zip(kinds, sizes):
            length = n * meta.get('audio_tracks', 1) if kind == 1 else n
            if cursor+length > offset+size:
                raise ValueError('Invalid THP component size')
            packets.append(data[cursor:cursor+length])
            cursor += length
        yield sizes, packets
        offset, size = offset+size, be32(data, offset)


def make(original, enhanced, output):
    source, target = Path(original).read_bytes(), Path(enhanced).read_bytes()
    sm, sc, sk = layout(source)
    tm, tc, tk = layout(target)
    for key in sm:
        if key not in ('width', 'height') and sm[key] != tm.get(key):
            raise ValueError('Enhanced movie changes the original timeline or audio metadata')
    if sk != tk or audio_packets(source) != audio_packets(target):
        raise ValueError('Enhanced movie changes original audio packets')
    if tm['width'] * sm['height'] != tm['height'] * sm['width']:
        raise ValueError('Enhanced movie changes the original framing')
    header = dict(schema=1, source_sha256=hashlib.sha256(source).hexdigest(),
                  target_sha256=hashlib.sha256(target).hexdigest(), target_bytes=len(target),
                  source=sm, width=tm['width'], height=tm['height'],
                  frames=tm['frames'], audio_packets_sha256=audio_packets(source))
    encoded = json.dumps(header, sort_keys=True, separators=(',', ':')).encode()
    out = Path(output);out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name+'.part')
    with tmp.open('wb') as f:
        f.write(MAGIC+struct.pack('>I', len(encoded))+encoded)
        for _, packets in frames(target, tm, tk):
            f.write(struct.pack('>I', len(packets[0])))
            f.write(packets[0])
    os.replace(tmp, out)
    return dict(header, patch_sha256=sha256(out), patch_bytes=out.stat().st_size)


def patch_header(f):
    if exact(f, 4) != MAGIC:
        raise ValueError('Expected an SMP1 HD movie patch')
    length = struct.unpack('>I', exact(f, 4))[0]
    if not 0 < length <= 65536:
        raise ValueError('Invalid movie patch header size')
    header = json.loads(exact(f, length))
    if header.get('schema') != 1 or not 0 < header.get('frames', 0) <= 20000:
        raise ValueError('Unsupported movie patch header')
    width, height = header.get('width', 0), header.get('height', 0)
    if not (0 < width <= 2048 and 0 < height <= 2048 and width % 16 == height % 16 == 0):
        raise ValueError('Unsupported HD movie dimensions')
    return header


def apply(original, patch, output):
    source = Path(original).read_bytes()
    meta, comp, kinds = layout(source)
    out = Path(output)
    if out.resolve() in (Path(original).resolve(), Path(patch).resolve()):
        raise ValueError('Output must differ from the original and patch')
    with Path(patch).open('rb') as f:
        h = patch_header(f)
        if hashlib.sha256(source).hexdigest() != h['source_sha256'] or meta != h['source']:
            raise ValueError('Your movie does not match the original disc required by this patch')
        if meta['frames'] != h['frames'] or h['width']*meta['height'] != h['height']*meta['width']:
            raise ValueError('Patch changes the frame count or aspect ratio')
        records = []
        for sizes, packets in frames(source, meta, kinds):
            n = struct.unpack('>I', exact(f, 4))[0]
            if not 4 <= n <= MAX_FRAME:
                raise ValueError('Invalid patched video frame size')
            position = f.tell()
            jpeg = exact(f, n)
            if jpeg[:2] != b'\xff\xd8' or jpeg[-2:] != b'\xff\xd9':
                raise ValueError('Invalid patched JPEG frame')
            frame_size = (8+4*len(kinds)+n+sum(len(p) for p in packets[1:])+31) & ~31
            records.append((position, n, frame_size))
        if f.read(1):
            raise ValueError('Unexpected data after patched movie frames')
        prefix = bytearray(source[:be32(source, 0x28)])
        sizes = [record[2] for record in records]
        struct.pack_into('>II', prefix, comp+20, h['width'], h['height'])
        struct.pack_into('>I', prefix, 8, max(sizes))
        struct.pack_into('>I', prefix, 0x18, sizes[0])
        struct.pack_into('>I', prefix, 0x1c, sum(sizes))
        struct.pack_into('>I', prefix, 0x2c, len(prefix)+sum(sizes[:-1]))
        if len(prefix)+sum(sizes) != h['target_bytes']:
            raise ValueError('Patched movie size does not match its identity')
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_name(out.name+'.part')
        try:
            with tmp.open('wb') as target:
                target.write(prefix)
                for i, ((position, n, frame_size), (components, packets)) in enumerate(zip(records, frames(source, meta, kinds))):
                    f.seek(position)
                    components[0] = n
                    header = struct.pack('>II', sizes[(i+1) % len(sizes)], sizes[(i-1) % len(sizes)])
                    header += struct.pack('>'+'I'*len(kinds), *components)
                    target.write(header);target.write(exact(f, n))
                    for packet in packets[1:]:target.write(packet)
                    target.write(bytes(frame_size-len(header)-n-sum(len(p) for p in packets[1:])))
            if sha256(tmp) != h['target_sha256']:
                raise ValueError('HD movie failed its final checksum')
            os.replace(tmp, out)
        finally:
            tmp.unlink(missing_ok=True)
    return h


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for command in ('make', 'apply'):
        p = sub.add_parser(command)
        p.add_argument('original', type=Path)
        p.add_argument('other', type=Path, help='enhanced THP for make, SMP1 patch for apply')
        p.add_argument('output', type=Path)
    a = parser.parse_args()
    try:
        result = make(a.original, a.other, a.output) if a.command == 'make' else apply(a.original, a.other, a.output)
        print(json.dumps(result, indent=2))
    except (ValueError, KeyError, OSError, struct.error) as error:
        parser.exit(1, str(error)+'\n')


if __name__ == '__main__':main()
