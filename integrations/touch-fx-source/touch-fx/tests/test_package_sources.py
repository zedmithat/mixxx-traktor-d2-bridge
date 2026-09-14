import hashlib
import importlib.util
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

BASE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('packager', BASE/'integration/package_sources.py')
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root/'source.tar.gz'

    def test_package_contains_only_declared_sources_with_matching_hashes(self):
        manifest = packager.package(self.output)
        with tarfile.open(self.output) as archive:
            self.assertEqual(set(archive.getnames()),
                {packager.PREFIX+'/'+name for name in (*packager.FILES, 'SOURCE-MANIFEST.json')})
            stored = json.load(archive.extractfile(packager.PREFIX+'/SOURCE-MANIFEST.json'))
            self.assertEqual(stored, manifest)
            for relative, expected in manifest['files_sha256'].items():
                member = archive.getmember(packager.PREFIX+'/'+relative)
                self.assertTrue(member.isfile())
                self.assertEqual(member.mode, 0o755 if relative.endswith('.sh') else 0o644)
                self.assertEqual(hashlib.sha256(archive.extractfile(member).read()).hexdigest(), expected)
        self.assertFalse(manifest['hardware_verified'])
        self.assertFalse(manifest['public_release_ready'])

    def test_package_is_reproducible(self):
        second = self.root/'second.tar.gz'
        packager.package(self.output)
        packager.package(second)
        self.assertEqual(self.output.read_bytes(), second.read_bytes())

    def test_license_grant_and_full_text_travel_with_sources(self):
        manifest = packager.package(self.output)
        self.assertEqual(manifest['original_code_license'], 'GPL-3.0-or-later')
        with tarfile.open(self.output) as archive:
            for name in ('touch-fx/LICENSE', 'COPYING.ZED'):
                data = archive.extractfile(packager.PREFIX+'/'+name).read()
                self.assertEqual(hashlib.sha256(data).hexdigest(),
                    '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986')
            for name in ('ZED-LICENSING.md', 'touch-fx/LICENSING.md'):
                data = archive.extractfile(packager.PREFIX+'/'+name).read().decode()
                self.assertIn('GPL-3.0-or-later', data)
                self.assertIn('any later version', data)
            upstream = archive.extractfile(packager.PREFIX+'/LICENSE').read()
            self.assertEqual(upstream, (packager.ROOT/'LICENSE').read_bytes())
            self.assertIn(b'Raspberry Pi (Trading) Ltd.', upstream)
        self.assertFalse(manifest['public_release_ready'])

    def test_existing_output_not_overwritten(self):
        self.output.write_bytes(b'keep')
        with self.assertRaises(FileExistsError):
            packager.package(self.output)
        self.assertEqual(self.output.read_bytes(), b'keep')

    def test_missing_inputs_fail_before_output_creation(self):
        with self.assertRaisesRegex(ValueError, 'missing'):
            packager.package(self.output, root=self.root)
        self.assertFalse(self.output.exists())

    def test_shell_crlf_rejected(self):
        (self.root/'run.sh').write_bytes(b'#!/bin/sh\r\n')
        with self.assertRaisesRegex(ValueError, 'LF'):
            packager.source_bytes(self.root, 'run.sh')


if __name__ == '__main__':
    unittest.main()
