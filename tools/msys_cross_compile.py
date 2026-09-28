#!/usr/bin/env python3
"""Pass native CMake/Ninja paths to the x64-host MSYS cross compiler.

MSYS GCC's preprocessing and linker processes expect POSIX drive paths.
The launcher still uses native x64 CMake, Ninja and Python programs.
"""
import pathlib
import re
import subprocess
import sys


def argument(value):
    value = re.sub(r'([A-Za-z]):[/\\]', lambda m: '/' + m[1].lower() + '/', value)
    if not value.startswith('-'):
        value = value.replace('\\', '/')
    return value


def main():
    compiler, *original = sys.argv[1:]
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
        result = subprocess.run([compiler, *converted])
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
