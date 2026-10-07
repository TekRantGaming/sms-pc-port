#!/usr/bin/env python3
"""Require a real cache hit with the selected private Windows compiler."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile


def hits(cache):
    report = json.loads(subprocess.check_output(
        [cache, '--show-stats', '--stats-format', 'json'], text=True))
    return sum(report['stats']['cache_hits']['counts'].values())


def main():
    cache = os.environ['SMS_COMPILER_CACHE']
    compiler = os.environ.get('CXX') or str(
        pathlib.Path(os.environ['MSYS2_ROOT']) / 'mingw64/bin/g++.exe')
    command = [cache, compiler]
    if os.environ.get('SMS_WINDOWS_32_CROSS') == '1':
        command = [sys.executable, str(pathlib.Path(__file__).resolve().parents[1] /
                   'msys_cross_compile.py'), '--cache', cache, compiler]
    with tempfile.TemporaryDirectory(prefix='SMS cache check with spaces ') as folder:
        folder = pathlib.Path(folder)
        source = folder / 'cache probe.cpp'
        header = folder / 'forced header.h'
        output = folder / 'probe.obj'
        header.write_text('#define PROBE_VALUE 42\n')
        source.write_text('int cache_probe() { return PROBE_VALUE; }\n')
        args = ['-include', str(header), '-c', str(source), '-o', str(output)]
        # Use the same drive and working directory as the launcher game build.
        subprocess.run([*command, *args], check=True)
        before = hits(cache)
        output.unlink()
        subprocess.run([*command, *args], check=True)
        if not output.is_file() or hits(cache) <= before:
            raise RuntimeError('Repeated compilation did not produce a compiler cache hit')
    print('Verified compiler cache hit with private Windows tools')


if __name__ == '__main__':
    main()
