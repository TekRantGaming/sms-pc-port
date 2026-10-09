"""get.py's texture extras: installed from a local server, checked, recorded so
--if-outdated (and SMS Launcher) installs them again only for a new release, and
optional for `get.py textures`, whose UHD pack stays installed when they fail."""
import contextlib
import functools
import hashlib
import http.server
import importlib.util
import io
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import threading
import unittest
import zipfile

GET = pathlib.Path(__file__).resolve().parents[1] / 'get.py'


def load_get():
    spec = importlib.util.spec_from_file_location('sms_get', GET)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def make_zip(files):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as z:
        for name, data in files.items():
            z.writestr(name, data)
    return buf.getvalue()


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


class ExtrasTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='sms get extras ')
        root = pathlib.Path(self.tmp.name)
        self.served, self.mods = root / 'served', root / 'mods'
        self.served.mkdir()
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Quiet, directory=str(self.served)))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.url = 'http://127.0.0.1:%d/' % server.server_address[1]
        self.get = load_get()
        self.get.MODS, self.get.DOWNLOADS = str(self.mods), str(self.mods / '.downloads')
        self.get.ROOT = str(root)
        self.extras = self.mods / 'textures' / 'sms-hd-texture-extras'
        self.get.EXTRAS.update(install=str(self.extras))
        self.get.TEXTURES.update(install=str(self.mods / 'textures' / 'GMS'))

    def tearDown(self):
        self.tmp.cleanup()

    def serve(self, mod, name, data, md5=None):
        (self.served / name).write_bytes(data)
        mod.update(url=self.url + name, file=name, size=len(data), md5=md5 or hashlib.md5(data).hexdigest())

    def run_quietly(self, fn, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            fn(*args)
        return out.getvalue()

    def installed(self, base):
        return sorted(p.relative_to(base).as_posix() for p in base.rglob('*') if p.is_file())

    def test_installs_and_replaces(self):
        self.extras.mkdir(parents=True)
        (self.extras / 'tex1_old_5.png').write_bytes(b'stale')
        self.serve(self.get.EXTRAS, 'extras.zip', make_zip({'GMS/gui/icons/tex1_38x49_a_5.png': b'png'}))
        out = self.run_quietly(self.get.get_extras, False)
        self.assertEqual(self.installed(self.extras), ['.release', 'gui/icons/tex1_38x49_a_5.png'])
        self.assertIn('Installed 1 textures in mods/textures/sms-hd-texture-extras.', out)
        self.assertFalse((self.mods / '.downloads' / 'extras.zip').exists())
        record = json.loads((self.extras / '.release').read_text())
        self.assertEqual(record['md5'], self.get.EXTRAS['md5'])

    def test_if_outdated_installs_only_a_new_release(self):
        self.serve(self.get.EXTRAS, 'v1.zip', make_zip({'GMS/tex1_a_5.png': b'one'}))
        out = self.run_quietly(self.get.get_extras, False, True)  # nothing installed yet
        self.assertIn('Installed 1 textures', out)
        (self.served / 'v1.zip').unlink()  # a second download would fail
        out = self.run_quietly(self.get.get_extras, False, True)
        self.assertIn('The texture extras are up to date', out)
        self.serve(self.get.EXTRAS, 'v2.zip', make_zip({'GMS/tex1_b_5.png': b'two'}))  # a new pin
        out = self.run_quietly(self.get.get_extras, False, True)
        self.assertIn('Installed 1 textures', out)
        self.assertEqual(self.installed(self.extras), ['.release', 'tex1_b_5.png'])

    def test_pin_file(self):
        pin = json.loads((GET.parent / 'texture-extras.json').read_text())
        self.assertEqual(sorted(pin), ['file', 'md5', 'name', 'page', 'size', 'url'])
        self.assertTrue(pin['url'].endswith('/' + pin['file']))
        self.assertRegex(pin['md5'], '^[0-9a-f]{32}$')

    def test_rejects_wrong_checksum(self):
        self.serve(self.get.EXTRAS, 'extras.zip', make_zip({'GMS/tex1_a_5.png': b'png'}), md5='0' * 32)
        with self.assertRaises(self.get.Failure):
            self.run_quietly(self.get.get_extras, False)
        self.assertFalse(self.extras.exists())

    def test_rejects_other_layouts(self):
        for files in ({'tex1_a_5.png': b'png'}, {'GMS/../../tex1_a_5.png': b'png'}):
            self.serve(self.get.EXTRAS, 'extras.zip', make_zip(files))
            with self.assertRaises(self.get.Failure):
                self.run_quietly(self.get.get_extras, False)
            self.assertFalse(self.extras.exists())
            self.assertFalse((self.mods / 'tex1_a_5.png').exists())

    def test_textures_keeps_the_pack_when_extras_fail(self):
        seven = shutil.which('7z') or shutil.which('7za') or shutil.which('7zz')
        if not seven:
            self.skipTest('7-Zip is required')
        pack = pathlib.Path(self.tmp.name) / 'pack'
        (pack / 'GMS' / 'Textures' / 'GMS' / 'gui').mkdir(parents=True)
        (pack / 'GMS' / 'Textures' / 'GMS' / 'gui' / 'tex1_30x40_b_5.dds').write_bytes(b'dds')
        subprocess.run([seven, 'a', '-bd', str(self.served / 'GMS.7z'), 'GMS'], cwd=pack,
                       check=True, stdout=subprocess.DEVNULL)
        self.serve(self.get.TEXTURES, 'GMS.7z', (self.served / 'GMS.7z').read_bytes())
        self.get.TEXTURES.update(unpacked=1 << 20)
        self.get.EXTRAS.update(url=self.url + 'missing.zip', file='missing.zip')
        out = self.run_quietly(self.get.get_textures, False)
        self.assertEqual(self.installed(self.mods / 'textures' / 'GMS'), ['gui/tex1_30x40_b_5.dds'])
        self.assertIn('warning: the UHD pack is installed, but not the extras', out)
        self.assertFalse(self.extras.exists())


if __name__ == '__main__':
    unittest.main()
