#!/usr/bin/env python3
"""Extract Sunshine THPs and create HD previews / optional native movie overrides.

Requires FFmpeg for conversion; AI mode also requires Real-ESRGAN ncnn Vulkan
and its model directory. Disc assets and generated movies remain local.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bundle_disc import Disc, be32


def metadata(data):
    if data[:4] != b'THP\0' or be32(data, 4) not in (0x10000, 0x11000):
        raise ValueError('Expected THP 1.0 or 1.1')
    version = be32(data, 4)
    comp = be32(data, 0x20)
    count = be32(data, comp)
    if not 1 <= count <= 16:
        raise ValueError('Invalid THP component count')
    row = {'fps': struct.unpack_from('>f', data, 0x10)[0], 'frames': be32(data, 0x14)}
    info = comp + 20
    for i in range(count):
        kind = data[comp + 4 + i]
        if kind == 0:
            row['width'], row['height'] = struct.unpack_from('>II', data, info)
            info += 12 if version == 0x11000 else 8
        elif kind == 1:
            row['audio_channels'], row['audio_rate'], row['audio_samples'] = struct.unpack_from('>III', data, info)
            row['audio_tracks'] = be32(data, info + 12) if version == 0x11000 else 1
            info += 16 if version == 0x11000 else 12
        else:
            raise ValueError('Unsupported THP component')
    if not 0 < row['fps'] <= 120 or not row['frames'] or not row.get('width') or not row.get('height'):
        raise ValueError('Invalid THP dimensions or timeline')
    row['duration'] = row['frames'] / row['fps']
    return row


def extract(a):
    out = a.out.resolve()
    (out / 'originals').mkdir(parents=True, exist_ok=True)
    disc = Disc(a.iso)
    try:
        boot = disc.read(0, 0x440)
        if boot[:6] != b'GMSE01' or be32(boot, 0x1c) != 0xc2339f3d:
            raise ValueError('Expected a North American GMSE01 disc')
        fst = disc.read(be32(boot, 0x424), be32(boot, 0x428))
        count = be32(fst, 8)
        if count * 12 > len(fst):
            raise ValueError('Invalid file table')
        stack, movies = [], []
        for i in range(1, count):
            while stack and i >= stack[-1][0]:
                stack.pop()
            e = i * 12
            start = count * 12 + (be32(fst, e) & 0xffffff)
            name = fst[start:fst.index(b'\0', start)].decode('utf-8')
            if name in ('.', '..') or '/' in name or '\\' in name:
                raise ValueError('Unsafe disc filename')
            if fst[e]:
                stack.append((be32(fst, e+8), name))
                continue
            if not name.lower().endswith('.thp'):
                continue
            data = disc.read(be32(fst, e+4), be32(fst, e+8))
            row = metadata(data)
            row.update(disc_path='/'.join([s[1] for s in stack] + [name]),
                       bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
            dest = out / 'originals' / name
            if dest.exists() and dest.read_bytes() != data:
                raise ValueError(f'Refusing to overwrite a different file: {dest}')
            if not dest.exists():
                dest.write_bytes(data)
            if hashlib.sha256(dest.read_bytes()).hexdigest() != row['sha256']:
                raise ValueError(f'Extraction verification failed: {dest}')
            movies.append(row)
            print(f"{name}: {row['width']}x{row['height']}, {row['frames']} frames, {row['duration']:.2f}s", flush=True)
        (out / 'manifest.json').write_text(json.dumps({'disc_id': 'GMSE01', 'movies': movies}, indent=2) + '\n')
    finally:
        disc.f.close()


def run(cmd, log):
    print('Running: ' + str(cmd[0]), flush=True)
    with log.open('w') as f:
        subprocess.run([str(s) for s in cmd], stdout=f, stderr=subprocess.STDOUT, check=True)


def jpeg_for_thp(jpeg):
    """Keep supported JPEG tables/SOF/SOS and remove entropy byte stuffing.

    THP uses unstuffed entropy data; FFmpeg's ordinary JPEG encoder does not.
    Header APP/COM metadata is removed because the native decoder rejects it.
    """
    if jpeg[:2] != b'\xff\xd8' or jpeg[-2:] != b'\xff\xd9':
        raise ValueError('Invalid JPEG frame')
    out = bytearray(jpeg[:2])
    pos = 2
    while pos < len(jpeg):
        if jpeg[pos] != 0xff:
            raise ValueError('Invalid JPEG marker')
        marker = jpeg[pos+1]
        length = struct.unpack_from('>H', jpeg, pos+2)[0]
        end = pos + 2 + length
        if end > len(jpeg) or length < 2:
            raise ValueError('Invalid JPEG segment')
        if marker in (0xc0, 0xc4, 0xdb, 0xdd, 0xda):
            out += jpeg[pos:end]
        elif not (0xe0 <= marker <= 0xef or marker == 0xfe):
            raise ValueError(f'Unsupported JPEG marker {marker:x}')
        if marker == 0xda:
            out += jpeg[end:-2].replace(b'\xff\x00', b'\xff') + b'\xff\xd9'
            return bytes(out)
        pos = end
    raise ValueError('JPEG has no scan')


def pack_thp(original, mjpeg, out, width, height):
    data = original.read_bytes()
    meta = metadata(data)
    comp = be32(data, 0x20)
    kinds = list(data[comp+4:comp+4+be32(data, comp)])
    video_index = kinds.index(0)
    prefix = bytearray(data[:be32(data, 0x28)])
    if be32(data, 0x24):
        raise ValueError('Indexed THP movies are not supported by this packer')
    # Video info precedes optional audio info in this game's files.
    if (video_index != 0 or not 0 < width <= 2048 or not 0 < height <= 2048
            or width % 16 or height % 16
            or width * meta['height'] != height * meta['width']):
        raise ValueError('Use video-first components and dimensions aligned to 16, at most 2048')
    struct.pack_into('>II', prefix, comp+20, width, height)
    encoded = mjpeg.read_bytes()
    # Encoder APP metadata does not contain EOI; entropy escapes marker bytes.
    jpegs = [p+b'\xff\xd9' for p in encoded.split(b'\xff\xd9')[:-1]]
    if len(jpegs) != meta['frames']:
        raise ValueError(f"JPEG frame count {len(jpegs)} != {meta['frames']}")
    frames = []
    offset, size = be32(data, 0x28), be32(data, 0x18)
    for image in jpegs:
        next_size = be32(data, offset)
        sizes = list(struct.unpack_from('>'+'I'*len(kinds), data, offset+8))
        cursor = offset + 8 + 4*len(kinds)
        parts = []
        for i, n in enumerate(sizes):
            packet_bytes = n * meta.get('audio_tracks', 1) if kinds[i] == 1 else n
            if cursor+packet_bytes > offset+size:
                raise ValueError('Truncated source THP frame')
            parts.append(jpeg_for_thp(image) if i == video_index else data[cursor:cursor+packet_bytes])
            cursor += packet_bytes
        sizes[video_index] = len(parts[video_index])
        # THP audio sizes describe one track; the payload contains every track.
        frame = bytearray(8) + struct.pack('>'+'I'*len(parts), *sizes) + b''.join(parts)
        frame += bytes((-len(frame)) % 32)
        frames.append(frame)
        offset += size
        size = next_size
    for i, frame in enumerate(frames):
        struct.pack_into('>II', frame, 0, len(frames[(i+1) % len(frames)]), len(frames[(i-1) % len(frames)]))
    struct.pack_into('>I', prefix, 8, max(map(len, frames)))
    struct.pack_into('>I', prefix, 0x18, len(frames[0]))
    struct.pack_into('>I', prefix, 0x1c, sum(map(len, frames)))
    struct.pack_into('>I', prefix, 0x2c, len(prefix)+sum(map(len, frames[:-1])))
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix('.tmp.thp')
    with tmp.open('wb') as f:
        f.write(prefix)
        for frame in frames:
            f.write(frame)
    os.replace(tmp, out)
    return {'width': width, 'height': height, 'frames': len(frames),
            'audio': 'Original compressed component packets copied without re-encoding',
            'sha256': hashlib.sha256(out.read_bytes()).hexdigest()}


def audio_packets(data):
    """Hash every track's compressed packets, independently of video size."""
    meta = metadata(data)
    comp = be32(data, 0x20)
    kinds = data[comp+4:comp+4+be32(data, comp)]
    offset, size = be32(data, 0x28), be32(data, 0x18)
    digest = hashlib.sha256()
    for _ in range(meta['frames']):
        if offset+size > len(data):
            raise ValueError('Truncated THP frame chain')
        cursor = offset+8+4*len(kinds)
        for i, kind in enumerate(kinds):
            n = be32(data, offset+8+4*i)
            if kind == 1:
                n *= meta['audio_tracks']
                digest.update(data[cursor:cursor+n])
            if cursor+n > offset+size:
                raise ValueError('THP component exceeds its frame')
            cursor += n
        offset, size = offset+size, be32(data, offset)
    return digest.hexdigest()


def upscale(a):
    original = a.input.resolve()
    meta = metadata(original.read_bytes())
    out = a.output.resolve()
    if out.exists():
        raise ValueError(f'Choose a new output filename: {out}')
    if a.thp and a.thp.exists():
        raise ValueError(f'Choose a new THP filename: {a.thp}')
    if a.no_preview and not a.thp:
        raise ValueError('--no-preview requires --thp')
    out.parent.mkdir(parents=True, exist_ok=True)
    ffmpeg = a.ffmpeg or shutil.which('ffmpeg')
    if not ffmpeg:
        raise ValueError('Install FFmpeg or pass --ffmpeg /path/to/ffmpeg')
    work = a.work.resolve() if a.work else Path(tempfile.mkdtemp(prefix='cutscene-', dir=out.parent))
    work.mkdir(parents=True, exist_ok=True)
    spec = {'source_sha256': hashlib.sha256(original.read_bytes()).hexdigest(), 'method': a.method,
            'scale': a.scale, 'model': a.model, 'models': str(a.models)}
    specfile = work / 'job.json'
    if specfile.exists() and json.loads(specfile.read_text()) != spec:
        raise ValueError('Work folder belongs to a different upscale job')
    specfile.write_text(json.dumps(spec, indent=2)+'\n')
    fps = '30000/1001' if abs(meta['fps']-30000/1001) < .0001 else str(meta['fps'])
    common = [ffmpeg, '-hide_banner', '-nostdin', '-y']
    source = ['-i', original]
    maps = ['-map', '0:v:0', '-map', '0:a:0?']
    natural = f'scale=iw*{a.scale}:ih*{a.scale}:flags=lanczos,setsar=1'
    if a.method == 'ai':
        if not a.realesrgan or not a.models:
            raise ValueError('AI mode requires --realesrgan and --models')
        native, enhanced = work/'native', work/'enhanced'
        native.mkdir(exist_ok=True); enhanced.mkdir(exist_ok=True)
        if len(list(native.glob('frame*.png'))) != meta['frames']:
            run(common+['-i', original, '-map', '0:v:0', '-fps_mode', 'passthrough', native/'frame%06d.png'], work/'extract.log')
        if len(list(enhanced.glob('frame*.png'))) != meta['frames']:
            run([a.realesrgan, '-i', native, '-o', enhanced, '-m', a.models, '-n', a.model,
                 '-s', a.scale, '-g', a.gpu, '-j', '2:2:2'], work/'ai.log')
        files = sorted(enhanced.glob('frame*.png'))
        if len(files) != meta['frames']:
            raise ValueError('AI upscale did not produce every frame')
        for image in files:
            with image.open('rb') as f:
                header = f.read(24)
            if header[:8] != b'\x89PNG\r\n\x1a\n' or struct.unpack_from('>II', header, 16) != (meta['width']*a.scale, meta['height']*a.scale):
                raise ValueError(f'Wrong or invalid AI frame: {image}')
        source = ['-framerate', fps, '-i', enhanced/'frame%06d.png', '-i', original]
        maps = ['-map', '0:v:0', '-map', '1:a:0?']
        natural = 'setsar=1'
    if not a.no_preview:
        fit = natural+',scale=1920:1080:force_original_aspect_ratio=decrease:force_divisible_by=2:flags=lanczos,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,setsar=1'
        tmp = out.with_name(out.stem+'.tmp.mp4')
        run(common+source+maps+['-vf', fit, '-c:v', 'libx264', '-preset', 'medium', '-crf', '17', '-threads', '4',
            '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '256k', '-movflags', '+faststart', '-fps_mode', 'passthrough', tmp], work/'encode.log')
        progress = work/'verify-progress.txt'
        run(common+['-v', 'error', '-i', tmp, '-progress', progress, '-nostats', '-f', 'null', '-'], work/'verify.log')
        decoded = [int(line[6:]) for line in progress.read_text().splitlines() if line.startswith('frame=')]
        if not decoded or decoded[-1] != meta['frames']:
            raise ValueError('Preview frame count differs from the source')
        os.replace(tmp, out)
    result = dict(spec, source=meta, preview=None if a.no_preview else str(out),
                  preview_sha256=None if a.no_preview else hashlib.sha256(out.read_bytes()).hexdigest(),
                  note='1920x1080 canvas, preserved framing, frame rate and dialogue. '
                       + ('AI detail is reconstructed.' if a.method == 'ai' else 'Lanczos interpolation; no reconstructed detail.'))
    if a.thp:
        mjpeg = work/'video.mjpeg'
        video_source = source[:source.index('-i')+2] if a.method == 'ai' else source
        run(common+video_source+['-an', '-vf', natural, '-c:v', 'mjpeg', '-q:v', '2', '-huffman', 'default',
            '-pix_fmt', 'yuvj420p', '-threads', '4', '-fps_mode', 'passthrough', '-f', 'mjpeg', mjpeg], work/'jpeg.log')
        result['thp'] = pack_thp(original, mjpeg, a.thp.resolve(), meta['width']*a.scale, meta['height']*a.scale)
        source_audio = audio_packets(original.read_bytes())
        if audio_packets(a.thp.read_bytes()) != source_audio:
            raise ValueError('THP audio packet verification failed')
        result['thp']['audio_packets_sha256'] = source_audio
    (out.with_suffix('.json')).write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2), flush=True)
    if not a.keep_frames:
        for name in ('native', 'enhanced'):
            shutil.rmtree(work/name, ignore_errors=True)
        (work/'video.mjpeg').unlink(missing_ok=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    e = sub.add_parser('extract');e.add_argument('--iso', type=Path, required=True);e.add_argument('--out', type=Path, required=True)
    u = sub.add_parser('upscale');u.add_argument('--input', type=Path, required=True);u.add_argument('--output', type=Path, required=True)
    u.add_argument('--ffmpeg');u.add_argument('--method', choices=('lanczos', 'ai'), default='lanczos');u.add_argument('--scale', type=int, choices=(2,3), default=3)
    u.add_argument('--realesrgan', type=Path);u.add_argument('--models', type=Path);u.add_argument('--model', default='realesr-animevideov3');u.add_argument('--gpu', default='0')
    u.add_argument('--work', type=Path);u.add_argument('--thp', type=Path, help='also write a native THP override; requires the HD playback patches')
    u.add_argument('--keep-frames', action='store_true')
    u.add_argument('--no-preview', action='store_true', help='generate only --thp and the output JSON report')
    a = p.parse_args()
    try:
        extract(a) if a.command == 'extract' else upscale(a)
    except (ValueError, OSError, subprocess.CalledProcessError) as ex:
        p.exit(1, str(ex)+'\n')


if __name__ == '__main__':
    main()
