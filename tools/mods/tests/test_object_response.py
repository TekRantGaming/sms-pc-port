"""Compile and partially link a list too long for Windows CMD, with spaced paths."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class ObjectResponseTest(unittest.TestCase):
    def test_long_object_list(self):
        cmake = shutil.which('cmake')
        if not cmake:
            self.skipTest('CMake is required')
        helper = pathlib.Path(__file__).resolve().parents[3] / 'platform/mods/eclipse/lib/object_response.cmake'
        with tempfile.TemporaryDirectory(prefix='sms response files with spaces-') as temporary:
            root = pathlib.Path(temporary) / ('long source and build paths ' * 3)
            source, build = root / 'source', root / 'build'
            source.mkdir(parents=True)
            count = 80
            for number in range(count):
                (source / f'object{number}.c').write_text(f'int object{number}(void){{return {number};}}\n')
            (source / 'external').mkdir()
            (source / 'external/extra.c').write_text('int extra(void){return 0;}\n')
            (source / 'CMakeLists.txt').write_text(f'''
                cmake_minimum_required(VERSION 3.20)
                project(response_probe C)
                include("{helper.as_posix()}")
                file(GLOB sources "${{CMAKE_CURRENT_SOURCE_DIR}}/*.c")
                add_library(objects OBJECT ${{sources}})
                add_custom_command(OUTPUT extra.o
                  COMMAND "${{CMAKE_C_COMPILER}}" -c "${{CMAKE_CURRENT_SOURCE_DIR}}/external/extra.c" -o extra.o
                  DEPENDS "${{CMAKE_CURRENT_SOURCE_DIR}}/external/extra.c" VERBATIM)
                sms_object_response(response "${{CMAKE_CURRENT_BINARY_DIR}}/objects.rsp"
                  $<TARGET_OBJECTS:objects> "${{CMAKE_CURRENT_BINARY_DIR}}/extra.o")
                add_custom_command(OUTPUT merged.o
                  COMMAND "${{CMAKE_C_COMPILER}}" -r -nostdlib -o merged.o "${{response}}"
                  DEPENDS objects $<TARGET_OBJECTS:objects> extra.o "${{CMAKE_CURRENT_BINARY_DIR}}/objects.rsp" VERBATIM)
                add_custom_target(partial ALL DEPENDS merged.o)
            ''')
            generator = 'Ninja' if shutil.which('ninja') else 'Unix Makefiles'
            subprocess.run([cmake, '-S', str(source), '-B', str(build), '-G', generator], check=True)
            response = build / 'objects.rsp'
            entries = response.read_text().splitlines()
            self.assertEqual(len(entries), count + 1)
            self.assertGreater(len(' '.join(entries)), 8191)
            for entry in entries:
                self.assertTrue(entry.startswith('"') and entry.endswith('"'))
            subprocess.run([cmake, '--build', str(build), '--parallel', '2'], check=True)
            self.assertTrue((build / 'merged.o').is_file())
            nm = subprocess.run([shutil.which('nm') or 'nm', str(build / 'merged.o')],
                                check=True, text=True, stdout=subprocess.PIPE)
            for number in range(count):
                self.assertRegex(nm.stdout, rf'\b_?object{number}\b')
            self.assertRegex(nm.stdout, r'\b_?extra\b')


if __name__ == '__main__':
    unittest.main()
