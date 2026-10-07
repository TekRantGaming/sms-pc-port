#!/usr/bin/env python3
"""Pass native CMake/Ninja paths to the x64-host MSYS cross compiler.

MSYS GCC's preprocessing and linker processes expect POSIX drive paths.
The launcher still uses native x64 CMake, Ninja and Python programs.
"""
import pathlib
import ntpath
import os
import re
import subprocess
import sys


def argument(value):
    value = re.sub(r'([A-Za-z]):[/\\]', lambda m: '/' + m[1].lower() + '/', value)
    if not value.startswith('-'):
        value = value.replace('\\', '/')
    return value


def cache_argument(value, cwd):
    """Use paths readable by both native sccache and the MSYS compiler.

    Native sccache cannot open /c/... paths. Relative forward-slash paths
    work for both programs, including -I paths and filenames with spaces.
    ntpath raises ValueError for another drive; that compile bypasses caching.
    """
    drive = re.search(r'[A-Za-z]:[/\\]', value)
    if drive:
        value = value[:drive.start()] + ntpath.relpath(value[drive.start():], cwd)
    return value.replace('\\', '/')


def main():
    args = sys.argv[1:]
    cache = None
    if args[0] == '--cache':
        _, cache, *args = args
    compiler, *original = args
    converted = []
    response_files = []
    for item in original:
        if item.startswith('@') and pathlib.Path(item[1:]).is_file():
            response = pathlib.Path(item[1:])
            portable = response.with_name(response.name + '.msys')
            contents = response.read_text(encoding='utf-8')
            contents = re.sub(r'([A-Za-z]):[/\\]', lambda m: '/' + m[1].lower() + '/', contents)
            portable.write_text(contents, encoding='utf-8')
            response_files.append(portable)
            converted.append('@' + argument(str(portable)))
        else:
            converted.append(argument(item))
    try:
        command = [compiler, *converted]
        # Link commands and response files keep the existing MSYS conversion.
        # Ninja normally passes compile arguments directly. Bypass caching for
        # response files or paths on another drive rather than breaking them.
        if cache and '-c' in original and not response_files:
            try:
                cached = [cache_argument(item, os.getcwd()) for item in original]
            except ValueError:
                pass
            else:
                command = [cache, compiler, *cached]
        result = subprocess.run(command)
    finally:
        for response in response_files:
            response.unlink(missing_ok=True)
    # Native Ninja needs native drive names in GCC's generated dependencies.
    if result.returncode == 0 and '-MF' in original:
        depfile = pathlib.Path(original[original.index('-MF') + 1])
        if depfile.is_file():
            contents = depfile.read_text(encoding='utf-8')
            contents = re.sub(r'(?<![\w])\/([a-zA-Z])\/', lambda m: m[1].upper() + ':/', contents)
            depfile.write_text(contents, encoding='utf-8')
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
