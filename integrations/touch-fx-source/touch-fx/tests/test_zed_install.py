import importlib.util
import json
import os
import re
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ElementTree

BASE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('installer', BASE/'integration/install_rootfs.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)/'rootfs'
        self.put('etc/os-release', b'ID=debian\n')
        self.put(installer.UNITS+'mixxx-d2.service', b'[Service]\nExecStart=/bin/true\n')
        self.put(installer.CFG, b'[Config]\nResizableSkin zed\nOther keep\n[Controller]\nD2_MIDI 1\n[ControllerPreset]\nD2_MIDI Traktor-Kontrol-D2.midi.xml\n')
        for relative in installer.SWAY:
            self.put(relative, b'exec mixxx\n')
        self.put(installer.CONTROLLERS+'Traktor-Kontrol-D2-scripts.js', b'synthetic D2\n')
        identity = patch.object(installer, 'D2_SHA256', installer.digest(b'synthetic D2\n'))
        identity.start()
        self.addCleanup(identity.stop)
        self.put(installer.SKIN+'skin.xml', b'<skin><manifest><title>zed</title><attributes><attribute config_key="[Master],num_decks">2</attribute></attributes></manifest></skin>')
        self.put(installer.SKIN+'topbar.xml', b'<Template><Children><PushButton><ObjectName>ZedWifiButton</ObjectName></PushButton></Children></Template>')
        self.put(installer.SKIN+'style.qss', b'unchanged style')

    def put(self, relative, data):
        path = self.root/relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def test_installs_variant_and_preserves_d2_and_original_skin(self):
        receipt = installer.install(self.root)
        for relative, expected in receipt['preserved_sha256'].items():
            self.assertEqual(installer.digest((self.root/relative).read_bytes()), expected)
        config = (self.root/installer.CFG).read_text()
        for setting in ('ResizableSkin zed-touchfx', 'D2_MIDI 1', 'Other keep',
                        'TouchFX_Virtual TouchFX-ZED.midi.xml'):
            self.assertIn(setting, config)
        skin = ElementTree.parse(self.root/installer.VARIANT/'skin.xml')
        self.assertIsNotNone(skin.find("./manifest/attributes/attribute[@config_key='[Skin],touch_fx']"))
        self.assertEqual(skin.find("./manifest/attributes/attribute[@config_key='[Master],num_decks']").text, '2')
        topbar = ElementTree.parse(self.root/installer.VARIANT/'topbar.xml')
        self.assertEqual([node.text for node in topbar.findall('.//ObjectName')], ['ZedTouchFXButton', 'ZedWifiButton'])
        mapping = ElementTree.parse(self.root/installer.CONTROLLERS/'TouchFX-ZED.midi.xml')
        self.assertEqual(mapping.find('./controller/scriptfiles/file').get('filename'), 'TouchFX-ZED-scripts.js')
        self.assertTrue(all(node.text == '[EffectRack1_EffectUnit3]' for node in mapping.findall('.//control/group')))
        self.assertFalse((self.root/installer.RUNTIME/'.test-venv').exists())
        self.assertFalse(receipt['hardware_verified'])
        self.assertFalse(receipt['public_release_ready'])

    def test_repeated_install_verifies_without_rewriting(self):
        first = installer.install(self.root)
        stamp = (self.root/installer.CFG).stat().st_mtime_ns
        self.assertEqual(installer.install(self.root), first)
        self.assertEqual((self.root/installer.CFG).stat().st_mtime_ns, stamp)

    def test_public_first_boot_installs_catalog_before_mixxx(self):
        self.put('etc/zed/image-flavor', b'public\n')
        receipt = installer.install(self.root)
        self.assertIn('home/pi/.mixxx/effects.xml', receipt['installed_sha256'])
        self.assertEqual(len(list((self.root/'home/pi/.mixxx/effects/defaults').glob('*.xml'))), 7)

    def test_private_first_boot_catalog_is_not_changed(self):
        self.put('etc/zed/image-flavor', b'private-owner-test\n')
        installer.install(self.root)
        self.assertFalse((self.root/'home/pi/.mixxx/effects.xml').exists())

    def test_existing_catalog_is_not_replaced(self):
        self.put('etc/zed/image-flavor', b'public\n')
        original = b'<MixxxEffects><VisibleEffects/></MixxxEffects>'
        self.put('home/pi/.mixxx/effects.xml', original)
        installer.install(self.root)
        self.assertEqual((self.root/'home/pi/.mixxx/effects.xml').read_bytes(), original)

    def test_partial_defaults_are_rejected(self):
        self.put('etc/zed/image-flavor', b'public\n')
        self.put('home/pi/.mixxx/effects/defaults/org-mixxx-effects-filter.xml', b'keep')
        with self.assertRaisesRegex(ValueError, 'Partial existing'):
            installer.install(self.root)

    def test_fullscreen_rule_never_matches_modal_macro_menu(self):
        installer.install(self.root)
        for relative in installer.SWAY:
            lines = (self.root/relative).read_text().splitlines()
            rules = [line for line in lines if 'org.zed.TouchFX' in line]
            fullscreen = [line for line in rules if 'fullscreen enable' in line]
            self.assertEqual(len(fullscreen), 1)
            pattern = re.search(r'title="([^"]+)"', fullscreen[0]).group(1)
            self.assertIsNotNone(re.search(pattern, 'ZED Touch FX'))
            self.assertIsNone(re.search(pattern, 'Touch FX · Makrolar'))
            floating = next(line for line in rules if 'floating enable' in line)
            pattern = re.search(r'title="([^"]+)"', floating).group(1)
            self.assertIsNotNone(re.search(pattern, 'Touch FX · Makrolar'))

    def test_only_new_directories_receive_explicit_permissions(self):
        original_chmod = Path.chmod
        changes = []
        def track(path, mode):
            changes.append((path, mode))
            original_chmod(path, mode)
        with patch.object(Path, 'chmod', track):
            installer.install(self.root)
        self.assertIn((self.root/installer.RUNTIME, 0o755), changes)
        self.assertIn((self.root/installer.VARIANT, 0o755), changes)
        self.assertIn((self.root/installer.RECEIPT, 0o644), changes)
        self.assertNotIn((self.root/'home/pi', 0o755), changes)

    @unittest.skipUnless(os.name == 'posix', 'POSIX permissions require Linux')
    def test_restrictive_umask_keeps_runtime_readable(self):
        home = self.root/'home/pi'
        home.chmod(0o700)
        previous = os.umask(0o077)
        try:
            receipt = installer.install(self.root)
        finally:
            os.umask(previous)
        for relative in receipt['installed_sha256']:
            self.assertEqual(stat.S_IMODE((self.root/relative).stat().st_mode), 0o644)
        for relative in (installer.RUNTIME, installer.VARIANT, 'opt/zed', 'opt/zed/touch-fx-backup'):
            self.assertEqual(stat.S_IMODE((self.root/relative).stat().st_mode), 0o755)
        self.assertEqual(stat.S_IMODE(home.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE((self.root/installer.RECEIPT).stat().st_mode), 0o644)

    def test_modified_install_rejected(self):
        installer.install(self.root)
        self.put(installer.RUNTIME+'touchfx.py', b'changed')
        with self.assertRaisesRegex(ValueError, 'drift'):
            installer.install(self.root)

    def test_new_source_revision_rejected_on_old_install(self):
        installer.install(self.root)
        receipt = json.loads((self.root/installer.RECEIPT).read_text())
        receipt['source_sha256']['touchfx.py'] = 'old'
        self.put(installer.RECEIPT, json.dumps(receipt).encode())
        with self.assertRaisesRegex(ValueError, 'sources changed'):
            installer.install(self.root)

    def test_missing_skin_anchor_fails_before_config_write(self):
        before = (self.root/installer.CFG).read_bytes()
        self.put(installer.SKIN+'topbar.xml', b'<Template/>')
        with self.assertRaisesRegex(ValueError, 'anchor'):
            installer.install(self.root)
        self.assertEqual((self.root/installer.CFG).read_bytes(), before)
        self.assertFalse((self.root/installer.RUNTIME).exists())

    def test_unreviewed_d2_fails_before_config_write(self):
        before = (self.root/installer.CFG).read_bytes()
        self.put(installer.CONTROLLERS+'Traktor-Kontrol-D2-scripts.js', b'unknown')
        with self.assertRaisesRegex(ValueError, 'D2'):
            installer.install(self.root)
        self.assertEqual((self.root/installer.CFG).read_bytes(), before)

    def test_existing_runtime_destination_is_not_overwritten(self):
        before = (self.root/installer.CFG).read_bytes()
        self.put(installer.RUNTIME+'touchfx.py', b'keep')
        with self.assertRaisesRegex(ValueError, 'existing'):
            installer.install(self.root)
        self.assertEqual((self.root/installer.CFG).read_bytes(), before)
        self.assertEqual((self.root/installer.RUNTIME/'touchfx.py').read_bytes(), b'keep')

    def test_refuses_live_root_and_parent_escape(self):
        with self.assertRaises(ValueError):
            installer.install(Path(Path.cwd().anchor))
        with self.assertRaises(ValueError):
            installer.safe(self.root, '../outside')

    def test_symlink_target_refused(self):
        destination = self.root/installer.VARIANT.rstrip('/')
        try:
            destination.symlink_to(self.root/installer.SKIN, target_is_directory=True)
        except OSError:
            self.skipTest('This host cannot create symlinks; run this test on Linux')
        with self.assertRaisesRegex(ValueError, 'Linked'):
            installer.install(self.root)

    def test_stage_and_service_ordering(self):
        stage = BASE.parent/'stage-zed/20-touch-fx'
        self.assertIn('integration/install_rootfs.py', (stage/'00-run.sh').read_text())
        packages = (stage/'00-packages-nr').read_text().splitlines()
        for package in ('python3-mido', 'python3-pyqt5', 'python3-rtmidi', 'qtwayland5'):
            self.assertIn(package, packages)
        service = (BASE/'integration/zed-touch-fx.service').read_text()
        self.assertIn('Type=notify', service)
        self.assertIn('--zed-session', service)
        self.assertNotIn('Requires=mixxx', service)
        dropin = (BASE/'integration/50-touch-fx.conf').read_text()
        self.assertIn('After=zed-touch-fx.service', dropin)
        self.assertIn('Wants=zed-touch-fx.service', dropin)
        self.assertNotIn('Requires=', dropin)

    def test_standard_os_identity_link_is_read_inside_rootfs(self):
        identity = self.root/'etc/os-release'
        identity.unlink()
        self.put('usr/lib/os-release', b'ID=debian\n')
        try:
            identity.symlink_to('../usr/lib/os-release')
        except OSError:
            self.skipTest('Host cannot create symlinks')
        receipt = installer.install(self.root)
        self.assertTrue(identity.is_symlink())
        self.assertFalse(receipt['hardware_verified'])

    def test_unexpected_os_identity_link_is_rejected(self):
        identity = self.root/'etc/os-release'
        identity.unlink()
        try:
            identity.symlink_to('../../outside')
        except OSError:
            self.skipTest('Host cannot create symlinks')
        with self.assertRaisesRegex(ValueError, 'OS identity'):
            installer.install(self.root)


if __name__ == '__main__':
    unittest.main()
