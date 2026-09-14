import sys
import unittest
import xml.etree.ElementTree as ElementTree
from pathlib import Path

BASE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BASE))
from touchfx_core import TouchFXState, amount_text, midi_value


class StateTests(unittest.TestCase):
    def test_extra_parameter_readouts_show_bounded_values_not_slider_percentage(self):
        for identifier, expected in ((1, '55%'), (2, '65%'), (4, 'Q 2.50'),
                                     (5, '55%'), (6, '75%'), (7, '100%'),
                                     (8, '8%'), (9, '50%'), (10, '40%')):
            self.assertEqual(amount_text(identifier, 127), expected)

    def test_effect_selection_bypasses_latch_and_resets_axes_before_request(self):
        self.state.set_latch(True)
        self.state.press(127, 127)
        self.messages.clear()
        self.state.select_effect(19)
        self.assertFalse(self.state.active)
        self.assertFalse(self.state.pressed)
        self.assertEqual(self.state.position, (64, 0))
        self.assertEqual(self.messages[-1], ('control_change', 0, 13, 19))
        self.assertEqual([item[0] for item in self.messages[:4]], ['note_off']*4)

    def test_invalid_effect_index_does_not_send(self):
        for value in (0, 128, -1, True, 1.5, '2'):
            with self.assertRaises(ValueError):
                self.state.select_effect(value)
        self.assertEqual(self.messages, [])

    def setUp(self):
        self.messages = []
        self.state = TouchFXState(lambda *message: self.messages.append(message))

    def test_endpoints_and_inverted_y(self):
        self.assertEqual(midi_value(0, 800), 0)
        self.assertEqual(midi_value(799, 800), 127)
        self.assertEqual(midi_value(0, 480, True), 127)
        self.assertEqual(midi_value(479, 480, True), 0)

    def test_coordinate_clamping_and_tiny_widget(self):
        self.assertEqual(midi_value(-100, 100), 0)
        self.assertEqual(midi_value(9999, 100), 127)
        self.assertEqual(midi_value(0, 0), 0)

    def test_axes_sent_before_activation(self):
        self.state.press(20, 30)
        self.assertEqual(self.messages, [('control_change', 0, 10, 20),
            ('control_change', 0, 11, 30), ('note_on', 0, 60, 127)])

    def test_momentary_release_disables(self):
        self.state.press(10, 20)
        self.state.release()
        self.assertEqual(self.messages[-1], ('note_off', 0, 60, 0))
        self.assertFalse(self.state.active)

    def test_latch_holds_position_and_gate(self):
        self.state.set_latch(True)
        self.state.press(10, 20)
        self.messages.clear()
        self.state.release()
        self.state.move(100, 100)
        self.assertEqual(self.messages, [])
        self.assertEqual(self.state.position, (10, 20))
        self.assertTrue(self.state.active)

    def test_unlatch_after_release_disables(self):
        self.state.set_latch(True)
        self.state.press(10, 20)
        self.state.release()
        self.state.set_latch(False)
        self.assertFalse(self.state.active)
        self.assertEqual(self.messages[-1][0], 'note_off')

    def test_unlatch_while_pressed_waits_for_release(self):
        self.state.set_latch(True)
        self.state.press(10, 20)
        self.state.set_latch(False)
        self.assertTrue(self.state.active)
        self.state.release()
        self.assertFalse(self.state.active)

    def test_latch_does_not_activate_without_touch(self):
        self.state.set_latch(True)
        self.assertFalse(self.state.active)
        self.assertEqual(self.messages, [])

    def test_deck_switch_clears_old_gate_and_requires_new_press(self):
        self.state.set_latch(True)
        self.state.press(11, 22)
        self.messages.clear()
        self.state.select_deck(4)
        self.assertEqual(self.messages, [('note_off', 0, 60, 0),
            ('control_change', 3, 12, 127), ('control_change', 3, 10, 11),
            ('control_change', 3, 11, 22)])
        self.assertFalse(self.state.active)
        self.assertFalse(self.state.pressed)
        self.state.move(99, 99)
        self.assertEqual(self.state.position, (11, 22))
        self.state.press(12, 23)
        self.assertEqual(self.messages[-1], ('note_on', 3, 60, 127))

    def test_same_deck_keeps_latched_effect(self):
        self.state.set_latch(True)
        self.state.press(11, 22)
        self.state.release()
        self.messages.clear()
        self.state.select_deck(1)
        self.assertTrue(self.state.active)
        self.assertEqual(self.messages, [])

    def test_rejects_invalid_decks(self):
        for deck in (-1, 0, 5, 16, '1'):
            with self.assertRaises(ValueError):
                self.state.select_deck(deck)

    def test_changed_axes_only(self):
        self.state.press(10, 20)
        self.messages.clear()
        self.state.move(10, 20)
        self.assertEqual(self.messages, [])
        self.state.move(11, 20)
        self.assertEqual(self.messages, [('control_change', 0, 10, 11)])

    def test_repeat_press_does_not_duplicate_on(self):
        self.state.press(10, 20)
        self.state.press(20, 30)
        self.assertEqual(len(self.messages), 3)

    def test_axes_are_valid_midi_bytes(self):
        self.state.press(-12, 200)
        self.assertEqual(self.state.position, (0, 127))

    def test_panic_overrides_latch_on_all_channels(self):
        self.state.set_latch(True)
        self.state.press(1, 2)
        self.messages.clear()
        self.state.panic()
        self.assertEqual(self.messages, [('note_off', channel, 60, 0) for channel in range(4)])
        self.assertFalse(self.state.active)
        self.assertFalse(self.state.pressed)

    def test_heartbeat_carries_current_gate_and_channel(self):
        self.state.select_deck(3)
        self.state.heartbeat()
        self.assertEqual(self.messages[-1], ('control_change', 2, 119, 0))
        self.state.press(1, 2)
        self.state.heartbeat()
        self.assertEqual(self.messages[-1], ('control_change', 2, 119, 127))

    def test_start_resets_before_route_and_axes(self):
        self.state.start()
        self.assertEqual(self.messages[:4], [('note_off', channel, 60, 0) for channel in range(4)])
        self.assertEqual(self.messages[4:], [('control_change', 0, 12, 127),
            ('control_change', 0, 10, 64), ('control_change', 0, 11, 0)])


class XmlTests(unittest.TestCase):
    def setUp(self):
        self.root = ElementTree.parse(BASE/'TouchFX_Virtual.midi.xml').getroot()

    def test_script_file_exists_and_prefix_matches(self):
        script = self.root.find('./controller/scriptfiles/file')
        self.assertTrue((BASE/script.attrib['filename']).is_file())
        self.assertEqual(script.attrib['functionprefix'], 'TouchFX')

    def test_complete_unique_protocol_and_bindings(self):
        expected = {}
        for channel in range(4):
            for number in (16, 20, 21, 22, 24, 26):
                expected[(0xB0+channel, number)] = 'TouchFX.macroMessage'
            for status, number, handler in ((0xB0, 10, 'axis'), (0xB0, 11, 'axis'),
                    (0xB0, 12, 'selectDeck'), (0xB0, 119, 'heartbeat'),
                    (0x90, 60, 'gate'), (0x80, 60, 'gate')):
                expected[(status+channel, number)] = 'TouchFX.'+handler
        actual = {}
        for control in self.root.findall('./controller/controls/control'):
            address = (int(control.findtext('status'), 16), int(control.findtext('midino'), 16))
            self.assertNotIn(address, actual)
            self.assertIsNotNone(control.find('./options/script-binding'))
            actual[address] = control.findtext('key')
        self.assertEqual(actual, expected)


if __name__ == '__main__':
    unittest.main()
