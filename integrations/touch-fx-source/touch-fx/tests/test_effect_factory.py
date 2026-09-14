import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ElementTree

BASE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BASE))
import zed_session

spec = importlib.util.spec_from_file_location('factory', BASE/'integration/effect_factory.py')
factory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(factory)


class FactoryTests(unittest.TestCase):
    def test_catalog_covers_all_builtins_without_changing_chains(self):
        files = factory.factory_files()
        self.assertEqual(len(files), 8)
        catalog = ElementTree.fromstring(files['home/pi/.mixxx/effects.xml'])
        self.assertIsNone(catalog.find('Rack'))
        self.assertIsNone(catalog.find('QuickEffectChains'))
        identifiers = [effect.findtext('Id') for effect in catalog.findall('./VisibleEffects/Effect')]
        self.assertEqual(len(identifiers), 24)
        self.assertEqual(len(set(identifiers)), 24)
        self.assertEqual(files, factory.factory_files())

    def test_first_start_has_single_effects_and_all_macros(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative, data in factory.factory_files().items():
                path = root/relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            with patch.object(Path, 'home', return_value=root/'home/pi'):
                choices = zed_session.effect_choices()
                self.assertEqual(len(choices), 9)
                self.assertEqual(dict(choices)[14], 'Filter')
                self.assertEqual({item[0] for item in zed_session.macro_choices()}, set(range(1, 12)))
                for identifier in zed_session.PRESET_PARAMETERS:
                    self.assertTrue(zed_session.compatible_preset(identifier), identifier)


if __name__ == '__main__':
    unittest.main()
