#!/usr/bin/env python3
"""Build and round-trip verify a complete 21-movie HD patch release."""
import argparse
import json
import os
from pathlib import Path

from install_cutscenes import MOVIES
from movie_patch import apply, make, sha256


def build_movie(original, enhanced, output, url_base):
    patch = output/(original.stem+'.smpatch')
    if patch.exists():
        # Identity is checked by applying, including the exact source hash.
        from movie_patch import patch_header
        with patch.open('rb') as f:identity=patch_header(f)
        if identity['target_sha256'] != sha256(enhanced):
            raise ValueError('An existing patch belongs to a different HD movie: '+patch.name)
        identity.update(patch_bytes=patch.stat().st_size, patch_sha256=sha256(patch))
    else:
        identity = make(original, enhanced, patch)
    source = identity['source']
    if identity['width'] != source['width'] * 3 or identity['height'] != source['height'] * 3:
        raise ValueError('HD release requires a 3x upscale of every original movie')
    check = output/(original.stem+'.roundtrip.thp')
    try:
        apply(original, patch, check)
        if sha256(check) != sha256(enhanced):
            raise ValueError('Movie patch round-trip differs from the verified HD movie')
    finally:
        check.unlink(missing_ok=True)
    return dict(disc_path='data/'+original.name, patch_file=patch.name,
                url=url_base.rstrip('/')+'/'+patch.name,
                source_sha256=identity['source_sha256'], target_sha256=identity['target_sha256'],
                target_bytes=identity['target_bytes'], patch_sha256=identity['patch_sha256'],
                patch_bytes=identity['patch_bytes'], width=identity['width'], height=identity['height'],
                frames=identity['frames'], audio_packets_sha256=identity['audio_packets_sha256'])


def build(source, pack, output, url_base, release):
    output.mkdir(parents=True, exist_ok=True)
    movies=[]
    for i, name in enumerate(MOVIES, 1):
        original, enhanced=source/(name+'.thp'),pack/'files/data'/(name+'.thp')
        if not original.exists() or not enhanced.exists():
            raise ValueError('The full HD set is not ready: '+name+'.thp is missing')
        print('Verifying movie patch: %d/21 %s' % (i,name),flush=True)
        movies.append(build_movie(original,enhanced,output,url_base))
    catalog=dict(schema=1,release=release,method='realesr-animevideov3',scale=3,movies=movies)
    target=output/'cutscene-release.json';tmp=target.with_suffix('.tmp.json')
    tmp.write_text(json.dumps(catalog,indent=2)+'\n');os.replace(tmp,target)
    print('HD release verified: 21/21 movie patches',flush=True)
    return catalog


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True,help='extracted originals folder')
    parser.add_argument('--pack',type=Path,required=True,help='HD pack folder containing files/data')
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--url-base',required=True,help='release asset URL prefix')
    parser.add_argument('--release',default='ai-hd-v1')
    a=parser.parse_args()
    try:build(a.source,a.pack,a.out,a.url_base,a.release)
    except (ValueError,OSError,KeyError) as error:parser.exit(1,str(error)+'\n')


if __name__=='__main__':main()
