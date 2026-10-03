#!/usr/bin/env python3
"""Install HD movie patches from the player's own GMSE01 disc, transactionally."""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import signal
import sys
import tempfile
import time
import urllib.request

from cutscenes import extract
from movie_patch import apply, sha256

MOVIES = ('autodemoA', 'bath', 'demogeso', 'Entrance', 'epilogue', 'EX128x144_q0',
          'kagemario', 'KuppaJr', 'MechaKuppa', 'NozuruA', 'NozuruB', 'omakeA',
          'omakeB', 'openingA', 'openingBA', 'openingBB', 'openingBC', 'Pakkun',
          'staffroll', 'stolennozuru', 'stolenpeach')
MARKER = 'sms-hd-cutscenes-v1.complete'
CATALOG = Path(__file__).with_name('cutscene-release.json')


def load_catalog(path):
    catalog = json.loads(Path(path).read_text())
    if catalog.get('schema') != 1 or not re.fullmatch(r'[A-Za-z0-9.-]+', catalog.get('release', '')):
        raise ValueError('Unsupported HD cutscene release')
    movies = catalog.get('movies', [])
    expected = {'data/'+name+'.thp' for name in MOVIES}
    if len(movies) != 21 or {m.get('disc_path') for m in movies} != expected:
        raise ValueError('The HD cutscene release must cover all 21 original movies')
    for movie in movies:
        path = PurePosixPath(movie['patch_file'])
        if len(path.parts) != 1 or path.name in ('.', '..') or not path.name.endswith('.smpatch'):
            raise ValueError('Invalid movie patch filename')
        for field in ('patch_sha256', 'source_sha256', 'target_sha256'):
            if not re.fullmatch(r'[a-f0-9]{64}', movie.get(field, '')):
                raise ValueError('Missing movie patch identity')
        if not 0 < movie.get('patch_bytes', 0) < 2 << 30 or not 0 < movie.get('target_bytes', 0) < 2 << 30:
            raise ValueError('Invalid movie patch size')
        if not movie.get('url', '').startswith('https://'):
            raise ValueError('Movie patch download must use HTTPS')
    return catalog


def installed(out, catalog):
    try:
        if (out/MARKER).read_text().strip() != catalog['release']:
            return False
        record = json.loads((out/'installed.json').read_text())
        if record != catalog:
            return False
        return all(sha256(out/'files'/m['disc_path']) == m['target_sha256'] for m in catalog['movies'])
    except (OSError, ValueError):
        return False


def fetch(movie, cache, bundle=None, number=None):
    if bundle:
        source = Path(bundle)/movie['patch_file']
        if source.stat().st_size != movie['patch_bytes'] or sha256(source) != movie['patch_sha256']:
            raise ValueError('The local HD movie patch failed its checksum: '+source.name)
        return source
    path = cache/movie['patch_file']
    if path.exists() and path.stat().st_size == movie['patch_bytes'] and sha256(path) == movie['patch_sha256']:
        return path
    cache.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name+'.part')
    request = urllib.request.Request(movie['url'], headers={'User-Agent': 'sms-pc-port HD cutscenes'})
    received, reported = 0, 0.0
    try:
        with urllib.request.urlopen(request, timeout=60) as response, tmp.open('wb') as target:
            while True:
                block = response.read(1 << 20)
                if not block:break
                target.write(block);received += len(block)
                if received > movie['patch_bytes']:
                    raise ValueError('HD movie patch exceeds its expected size')
                if time.monotonic()-reported >= .5:
                    suffix = ' (movie %d/21)' % number if number else ''
                    print('HD movie download: %d/%d bytes%s' % (received, movie['patch_bytes'], suffix), flush=True)
                    reported = time.monotonic()
        if received != movie['patch_bytes'] or sha256(tmp) != movie['patch_sha256']:
            raise ValueError('The HD movie download failed its checksum: '+path.name)
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)
    return path


def install(iso, out, catalog, cache, bundle=None):
    out = Path(out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    print('Checking HD cutscenes', flush=True)
    if installed(out, catalog):
        print('HD cutscenes installed: 21/21 movies', flush=True)
        return
    movies = catalog['movies']
    needed = sum(m['target_bytes'] for m in movies) + (900 << 20) + max(m['patch_bytes'] for m in movies) + (64 << 20)
    if shutil.disk_usage(out.parent).free < needed:
        raise ValueError('HD cutscene setup needs %.1f GB free in the game installation folder' % (needed/1e9))
    transaction = Path(tempfile.mkdtemp(prefix='.hd-cutscenes-', dir=out.parent))
    staging, backup = transaction/'pack', transaction/'previous'
    committed = False
    try:
        print('Preparing HD cutscenes from your disc', flush=True)
        extract(argparse.Namespace(iso=Path(iso), out=transaction/'source'))
        originals = transaction/'source/originals'
        for i, movie in enumerate(movies, 1):
            original = originals/PurePosixPath(movie['disc_path']).name
            if sha256(original) != movie['source_sha256']:
                raise ValueError('Your disc does not match the HD patch for '+original.name)
            print('Installing HD cutscenes: %d/21 movies' % (i-1), flush=True)
            patch = fetch(movie, Path(cache), bundle, i)
            target = staging/'files'/movie['disc_path']
            identity = apply(original, patch, target)
            if identity['target_sha256'] != movie['target_sha256'] or target.stat().st_size != movie['target_bytes']:
                raise ValueError('The installed HD movie differs from its release identity')
            if not bundle:patch.unlink()
            print('Installing HD cutscenes: %d/21 movies' % i, flush=True)
        (staging/'installed.json').write_text(json.dumps(catalog, indent=2)+'\n')
        (staging/MARKER).write_text(catalog['release']+'\n')
        try:
            if out.exists():os.replace(out, backup)
            os.replace(staging, out)
            committed = True
        except BaseException:
            if backup.exists():
                if out.exists():shutil.rmtree(out)
                os.replace(backup, out)
            raise
        print('HD cutscenes installed: 21/21 movies', flush=True)
    finally:
        # If restoring an old pack fails, retain its backup for recovery.
        if committed or not backup.exists():shutil.rmtree(transaction)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parents[2]/'mods/hd-cutscenes')
    parser.add_argument('--manifest', type=Path, default=CATALOG)
    parser.add_argument('--bundle', type=Path, help='use a verified local patch folder instead of downloading')
    parser.add_argument('--cache', type=Path)
    a = parser.parse_args()
    # The launcher's Stop button sends SIGTERM on Unix. Unwind the active
    # transaction so cancellation leaves the installed pack and disc intact.
    def stop(_signal, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    cache = a.cache or a.out.resolve().parent/'.downloads/hd-cutscenes'
    try:install(a.iso, a.out, load_catalog(a.manifest), cache, a.bundle)
    except (OSError, ValueError, KeyError) as error:parser.exit(1, str(error)+'\n')
    except KeyboardInterrupt:parser.exit(1, 'HD cutscene setup stopped. You can retry from the launcher.\n')


if __name__ == '__main__':main()
