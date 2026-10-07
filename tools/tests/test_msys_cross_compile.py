#!/usr/bin/env python3
"""Exercise cache routing and dependency rewriting without a Windows toolchain."""
import importlib.util
import pathlib
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    'cross', pathlib.Path(__file__).resolve().parents[1] / 'msys_cross_compile.py')
cross = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cross)


class CrossCompileTest(unittest.TestCase):
    def invoke(self, args, status=0):
        with mock.patch.object(cross.sys, 'argv', ['cross.py', *args]), \
             mock.patch.object(cross.os, 'getcwd', return_value='C:/work/build'), \
             mock.patch.object(cross.subprocess, 'run', return_value=mock.Mock(returncode=status)) as run:
            result = cross.main()
            return result, run.call_args.args[0]

    def test_cached_compile_uses_real_compiler_and_shared_relative_paths(self):
        status, command = self.invoke([
            '--cache', 'C:/tools/sccache.exe', 'C:/tools/g++.exe',
            '-IC:/work/include with spaces', '-include', r'C:\work\forced header.h',
            '-c', 'C:/work/main.cpp', '-o', 'C:/work/build/main.obj'])
        self.assertEqual(status, 0)
        self.assertEqual(command, [
            'C:/tools/sccache.exe', 'C:/tools/g++.exe',
            '-I../include with spaces', '-include', '../forced header.h',
            '-c', '../main.cpp', '-o', 'main.obj'])

    def test_uncached_compile_preserves_msys_conversion(self):
        _, command = self.invoke(['C:/tools/g++.exe', '-c', 'C:/work/main.cpp'])
        self.assertEqual(command, ['C:/tools/g++.exe', '-c', '/c/work/main.cpp'])

    def test_link_is_not_cached(self):
        _, command = self.invoke(['--cache', 'cache', 'gcc', 'C:/work/main.obj', '-o', 'game.exe'])
        self.assertEqual(command, ['gcc', '/c/work/main.obj', '-o', 'game.exe'])

    def test_other_drive_bypasses_cache(self):
        _, command = self.invoke(['--cache', 'cache', 'gcc', '-c', 'D:/work/main.cpp'])
        self.assertEqual(command, ['gcc', '-c', '/d/work/main.cpp'])

    def test_response_file_bypasses_cache_and_is_removed(self):
        with tempfile.TemporaryDirectory() as folder:
            response = pathlib.Path(folder) / 'args with spaces.rsp'
            response.write_text('"C:/work/main.cpp" -o "C:/work/main.obj"')
            converted = response.with_name(response.name + '.msys')
            expected_path = converted.as_posix()
            if converted.drive:
                expected_path = '/' + converted.drive[0].lower() + expected_path[2:]
            def run(command):
                self.assertEqual(command, ['gcc', '-c', '@' + expected_path])
                self.assertEqual(converted.read_text(), '"/c/work/main.cpp" -o "/c/work/main.obj"')
                return mock.Mock(returncode=0)
            with mock.patch.object(cross.sys, 'argv', ['cross.py', '--cache', 'cache', 'gcc', '-c', '@' + str(response)]), \
                 mock.patch.object(cross.subprocess, 'run', side_effect=run):
                self.assertEqual(cross.main(), 0)
            self.assertFalse(converted.exists())
            self.assertTrue(response.exists())

    def test_cached_dependency_file_is_rewritten_for_native_ninja(self):
        with tempfile.TemporaryDirectory() as folder:
            depfile = pathlib.Path(folder) / 'main.d'
            depfile.write_text('main.obj: /c/work/main.cpp /c/work/include/header.h\n')
            self.invoke(['--cache', 'cache', 'gcc', '-c', 'C:/work/main.cpp', '-MF', str(depfile)])
            self.assertEqual(depfile.read_text(), 'main.obj: C:/work/main.cpp C:/work/include/header.h\n')

    def test_compile_failure_is_propagated(self):
        status, _ = self.invoke(['--cache', 'cache', 'gcc', '-c', 'main.cpp'], status=2)
        self.assertEqual(status, 2)


if __name__ == '__main__':
    unittest.main()
