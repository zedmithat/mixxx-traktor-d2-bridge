"""Keep tab carrier backgrounds opaque without overriding pad/button colors."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TabBackgroundTests(unittest.TestCase):
    def test_native_waveform_carrier_has_explicit_black_background(self):
        source = (ROOT / 'skin/zed/style.qss').read_text()
        blocks = re.findall(r'([^{}]+)\{([^{}]+)\}', re.sub(r'/\*.*?\*/', '', source, flags=re.S))
        for selector in ('#Waveforms QWidget',):
            matching = [body for selectors, body in blocks
                        if selector in [s.strip() for s in selectors.split(',')]]
            self.assertTrue(matching, selector)
            self.assertIn('background-color: #000000;', matching[-1])
            self.assertIn('background-image: none;', matching[-1])

    def test_control_panel_color_is_preserved(self):
        source = (ROOT / 'skin/zed/style.qss').read_text()
        self.assertIn('background-color: #0d1116;', source)
        self.assertIn('background-color: #1b1e24;', source)
        self.assertNotIn('* { background-color:', source)


if __name__ == '__main__':
    unittest.main()
