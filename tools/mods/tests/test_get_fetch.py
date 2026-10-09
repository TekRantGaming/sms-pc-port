"""get.py's downloads: a mod's url first, then each of its fallback_urls when one
fails or sends a file that is not the release (Eclipse: SMS Launcher's mirror,
then GameBanana)."""
import contextlib
import functools
import hashlib
import http.server
import importlib.util
import io
import pathlib
import tempfile
import threading
import unittest

GET = pathlib.Path(__file__).resolve().parents[1] / 'get.py'
RELEASE = b'the release archive'


def load_get():
    spec = importlib.util.spec_from_file_location('sms_get', GET)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


class FetchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='sms get fetch ')
        root = pathlib.Path(self.tmp.name)
        self.served = root / 'served'
        self.served.mkdir()
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Quiet, directory=str(self.served)))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.url = 'http://127.0.0.1:%d/' % server.server_address[1]
        self.get = load_get()
        self.get.MODS, self.get.DOWNLOADS = str(root / 'mods'), str(root / 'mods' / '.downloads')
        self.mod = {'name': 'Test mod', 'page': 'https://example.invalid/mod', 'file': 'mod.7z',
                    'url': self.url + 'mirror.7z', 'fallback_urls': [self.url + 'origin.7z'],
                    'md5': hashlib.md5(RELEASE).hexdigest()}

    def tearDown(self):
        self.tmp.cleanup()

    def serve(self, name, data):
        (self.served / name).write_bytes(data)

    def fetch(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            dest = self.get.fetch(self.mod)
        return pathlib.Path(dest), out.getvalue()

    def test_downloads_from_the_mirror_first(self):
        self.serve('mirror.7z', RELEASE)
        self.serve('origin.7z', RELEASE)
        dest, out = self.fetch()
        self.assertEqual(dest.read_bytes(), RELEASE)
        self.assertIn('from %smirror.7z' % self.url, out)
        self.assertNotIn('origin.7z', out)

    def test_falls_back_when_the_mirror_is_down(self):
        self.serve('origin.7z', RELEASE)
        dest, out = self.fetch()
        self.assertEqual(dest.read_bytes(), RELEASE)
        self.assertIn('failed:', out)
        self.assertIn('from %sorigin.7z' % self.url, out)

    def test_falls_back_when_the_mirror_sends_another_file(self):
        self.serve('mirror.7z', b'something else')
        self.serve('origin.7z', RELEASE)
        dest, out = self.fetch()
        self.assertEqual(dest.read_bytes(), RELEASE)
        self.assertIn('expected %s' % self.mod['md5'], out)

    def test_fails_when_no_url_has_the_release(self):
        self.serve('mirror.7z', b'something else')
        with self.assertRaises(self.get.Failure) as caught:
            self.fetch()
        message = str(caught.exception)
        self.assertIn('no download matched the expected release', message)
        self.assertIn('mirror.7z', message)
        self.assertIn('origin.7z', message)
        self.assertFalse((pathlib.Path(self.get.DOWNLOADS) / 'mod.7z').exists())

    def test_one_url_keeps_its_messages(self):
        del self.mod['fallback_urls']
        self.serve('mirror.7z', b'something else')
        with self.assertRaises(self.get.Failure) as caught:
            self.fetch()
        self.assertIn('the download does not match the expected release', str(caught.exception))
        (self.served / 'mirror.7z').unlink()
        with self.assertRaises(self.get.Failure) as caught:
            self.fetch()
        self.assertTrue(str(caught.exception).startswith('download failed: '))

    def test_eclipse_comes_from_the_mirror_then_gamebanana(self):
        eclipse = self.get.ECLIPSE
        self.assertEqual(eclipse['url'], 'https://sms-eclipse.tekrantgaming.com/files/super_mario_eclipse_v110.7z')
        self.assertEqual(eclipse['fallback_urls'], ['https://gamebanana.com/dl/1729332'])
        self.assertEqual(eclipse['md5'], '37c1805ab88b2a3bb2a96bcdf6884433')


if __name__ == '__main__':
    unittest.main()
