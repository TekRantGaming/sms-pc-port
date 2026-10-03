import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from movie_patch import apply, make, audio_packets
from install_cutscenes import install, installed, load_catalog, MOVIES, MARKER


def movie(hd=False):
    prefix = bytearray(128)
    prefix[:4] = b'THP\0'
    struct.pack_into('>IIII', prefix, 4, 0x11000, 64 if hd else 128, 2, struct.unpack('>I', struct.pack('>f', 30000/1001))[0])
    struct.pack_into('>IIIIIII', prefix, 0x14, 2, 64, 128, 48, 0, 128, 192)
    struct.pack_into('>I', prefix, 48, 2);prefix[52:54] = b'\0\1'
    struct.pack_into('>III', prefix, 68, 48 if hd else 16, 48 if hd else 16, 0)
    struct.pack_into('>IIII', prefix, 80, 2, 32000, 4, 2)
    records=[]
    for i in range(2):
        jpeg = b'\xff\xd8'+(b'enhanced frame' if hd else b'native')+bytes([i])+b'\xff\xd9'
        audio = bytes(range(i*16, i*16+16))
        frame=struct.pack('>IIII', 64, 64, len(jpeg), 8)+jpeg+audio
        records.append(frame+bytes(64-len(frame)))
    return bytes(prefix)+b''.join(records)


def disc(path):
    names = [name+'.thp' for name in MOVIES]
    strings=b'\0data\0';offsets=[]
    for name in names:offsets.append(len(strings));strings+=name.encode()+b'\0'
    count=len(names)+2
    fst=struct.pack('>III', 0x1000000, 0, count)+struct.pack('>III', 0x1000001, 0, count)
    data=bytearray(32768);data[:6]=b'GMSE01';struct.pack_into('>I', data, 0x1c, 0xc2339f3d)
    for i, offset in enumerate(offsets):
        pos=0x1000+i*256;source=movie();data[pos:pos+len(source)]=source
        fst+=struct.pack('>III', offset, pos, len(source))
    fst+=strings;struct.pack_into('>II', data, 0x424, 0x800, len(fst));data[0x800:0x800+len(fst)]=fst
    path.write_bytes(data)


class MoviePatchTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.original=self.root/'original.thp';self.target=self.root/'hd.thp'
        self.original.write_bytes(movie());self.target.write_bytes(movie(True));self.patch=self.root/'movie.smpatch'
        self.identity=make(self.original,self.target,self.patch)

    def test_roundtrip_preserves_both_audio_tracks_and_every_frame(self):
        out=self.root/'patched.thp';apply(self.original,self.patch,out)
        self.assertEqual(out.read_bytes(),self.target.read_bytes())
        self.assertEqual(audio_packets(out.read_bytes()),audio_packets(self.original.read_bytes()))
        self.assertLess(self.patch.stat().st_size,4096)

    def test_wrong_source_and_corrupt_patch_never_replace_an_existing_output(self):
        out=self.root/'previous.thp';out.write_bytes(b'previous installed movie')
        original=self.original.read_bytes();self.original.write_bytes(original[:-1]+b'X')
        with self.assertRaisesRegex(ValueError,'original disc'):apply(self.original,self.patch,out)
        self.original.write_bytes(original)
        data=self.patch.read_bytes();self.patch.write_bytes(data[:-2]+b'XX')
        with self.assertRaises(ValueError):apply(self.original,self.patch,out)
        self.assertEqual(out.read_bytes(),b'previous installed movie')
        self.assertFalse(out.with_name(out.name+'.part').exists())

    def fixture(self):
        iso=self.root/'own.iso';disc(iso)
        bundle=self.root/'bundle';bundle.mkdir();movies=[]
        for name in MOVIES:
            file=name+'.smpatch';identity=make(self.original,self.target,bundle/file)
            movies.append(dict(identity,disc_path='data/'+name+'.thp',patch_file=file,url='https://example.invalid/'+file))
        catalog=dict(schema=1,release='test-v1',movies=movies)
        manifest=bundle/'release.json';manifest.write_text(json.dumps(catalog))
        return iso,bundle,load_catalog(manifest)

    def test_installer_covers_all_21_movies_from_iso_and_ciso(self):
        iso,bundle,catalog=self.fixture()
        for ciso in (False,True):
            source=iso
            if ciso:
                source=self.root/'own.ciso';header=bytearray(32768);header[:4]=b'CISO'
                struct.pack_into('<I',header,4,32768);header[8]=1;source.write_bytes(header+iso.read_bytes())
            out=self.root/('ciso-install' if ciso else 'iso-install')
            install(source,out,catalog,self.root/'cache',bundle)
            self.assertTrue(installed(out,catalog))
            self.assertEqual(len(list((out/'files/data').glob('*.thp'))),21)
            for file in (out/'files/data').glob('*.thp'):self.assertEqual(file.read_bytes(),self.target.read_bytes())
            self.assertEqual((out/MARKER).read_text().strip(),'test-v1')

    def test_failed_install_retains_the_previous_pack_and_removes_staging(self):
        iso,bundle,catalog=self.fixture();out=self.root/'installed';out.mkdir();(out/'previous').write_bytes(b'keep me')
        patch=bundle/catalog['movies'][1]['patch_file'];patch.write_bytes(patch.read_bytes()+b'bad')
        with self.assertRaisesRegex(ValueError,'checksum'):install(iso,out,catalog,self.root/'cache',bundle)
        self.assertEqual((out/'previous').read_bytes(),b'keep me')
        self.assertFalse(list(self.root.glob('.hd-cutscenes-*')))
        self.assertFalse((out/MARKER).exists())

    def test_cancel_unwinds_and_preserves_the_previous_pack(self):
        iso,bundle,catalog=self.fixture();out=self.root/'installed';out.mkdir();(out/'previous').write_bytes(b'keep me')
        with mock.patch('install_cutscenes.apply',side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):install(iso,out,catalog,self.root/'cache',bundle)
        self.assertEqual((out/'previous').read_bytes(),b'keep me')
        self.assertFalse(list(self.root.glob('.hd-cutscenes-*')))

    def test_cancel_during_pack_swap_restores_the_previous_pack(self):
        for after_backup in (True, False):
            with self.subTest(after_backup=after_backup):
                iso,bundle,catalog=self.fixture()
                out=self.root/'installed';out.mkdir();(out/'previous').write_bytes(b'keep me')
                replace=os.replace
                def interrupt(source, target):
                    if Path(source) == out and after_backup:
                        replace(source,target)
                        raise KeyboardInterrupt
                    if Path(source).name == 'pack' and not after_backup:
                        raise KeyboardInterrupt
                    return replace(source,target)
                with mock.patch('install_cutscenes.os.replace',side_effect=interrupt):
                    with self.assertRaises(KeyboardInterrupt):install(iso,out,catalog,self.root/'cache',bundle)
                self.assertEqual((out/'previous').read_bytes(),b'keep me')
                self.assertFalse(list(self.root.glob('.hd-cutscenes-*')))
                shutil.rmtree(out);shutil.rmtree(bundle)


if __name__=='__main__':unittest.main()
