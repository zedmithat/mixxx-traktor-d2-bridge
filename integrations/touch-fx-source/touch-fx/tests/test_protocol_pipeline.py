import importlib.util
import os
from pathlib import Path
import sys
import unittest
import xml.etree.ElementTree as ElementTree

os.environ['QT_QPA_PLATFORM'] = 'offscreen'
BASE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BASE))
AVAILABLE = all(importlib.util.find_spec(name) is not None for name in ('PyQt5', 'mido'))
AVAILABLE = AVAILABLE and importlib.util.find_spec('PyQt5.QtQml') is not None
if AVAILABLE:
    import mido
    from PyQt5.QtQml import QJSEngine
    from PyQt5.QtWidgets import QApplication
    from touchfx_core import TouchFXState


@unittest.skipUnless(AVAILABLE, 'PyQt5 and mido required for MIDI/XML/Qt JS integration')
class ProtocolPipelineTests(unittest.TestCase):
    decks = (1, 2, 3, 4)

    def test_gui_beat_labels_match_javascript_at_every_midi_position(self):
        from fractions import Fraction
        from touchfx_core import ECHO_BEATS, FLANGER_BEATS, GATE_BEATS, beat_label
        for kind, labels in (('echo', ECHO_BEATS), ('gate', GATE_BEATS), ('flanger', FLANGER_BEATS)):
            for position in range(128):
                actual = self.evaluate("TouchFX.beatStep('{}', {}/127)".format(kind, position)).toNumber()
                self.assertEqual(actual, float(Fraction(beat_label(labels, position))))

    def mapping_path(self):
        return BASE/'TouchFX_Virtual.midi.xml'

    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.javascript = QJSEngine()
        self.evaluate('''
            var clock = 10000;
            Date.now = function() { return clock; };
            var values = {};
            var writes = [];
            var outgoing = [];
            var skinConnection;
            var midi = {sendShortMsg: function(status, number, value) {
                outgoing.push([status, number, value]);
            }};
            var engine = {
                setValue: function(group, key, value) {
                    values[group+'/'+key] = value;
                    writes.push([group, key, value]);
                },
                setParameter: function(group, key, value) { values[group+'/'+key] = value; },
                getValue: function(group, key) {
                    if (key === 'num_effectslots') return 3;
                    return key === 'num_samplers' ? 4 : 0;
                },
                makeConnection: function(group, key, callback) {
                    skinConnection = {callback: callback, isConnected: true,
                        disconnect: function() { this.isConnected = false; }};
                    return skinConnection;
                },
                beginTimer: function(interval, callback) { return 1; },
                stopTimer: function(identifier) {}
            };
            var print = function(message) {};
        ''')
        mapping = self.mapping_path()
        root = ElementTree.parse(mapping).getroot()
        for script in root.findall('./controller/scriptfiles/file'):
            self.evaluate((mapping.parent/script.get('filename')).read_text(encoding='utf-8'))
        for script in root.findall('./controller/scriptfiles/file'):
            self.evaluate(script.get('functionprefix')+'.init("test", false);')
        self.bindings = {}
        for control in root.findall('./controller/controls/control'):
            address = (int(control.findtext('status'), 16), int(control.findtext('midino'), 16))
            self.bindings[address] = (control.findtext('key'), control.findtext('group'))
        self.packets = []
        self.state = TouchFXState(self.send)
        self.state.start()

    def evaluate(self, source):
        result = self.javascript.evaluate(source)
        self.assertFalse(result.isError(), result.toString())
        return result

    def dispatch(self, message):
        status, number, value = message.bytes()
        self.packets.append([status, number, value])
        name, group = self.bindings[(status, number)]
        handler = self.evaluate(name)
        self.assertTrue(handler.isCallable(), name)
        result = handler.call([status & 0x0F, number, value, status, group])
        self.assertFalse(result.isError(), result.toString())

    def send(self, kind, channel, number, value):
        fields = {'control': number, 'value': value} if kind == 'control_change' else {
            'note': number, 'velocity': value}
        self.dispatch(mido.Message(kind, channel=channel, **fields))

    def value(self, control):
        return self.evaluate('values[TouchFX.unit+"/'+control+'"];').toNumber()

    def test_momentary_for_every_deck(self):
        for deck in self.decks:
            self.state.select_deck(deck)
            self.state.press(100, 50)
            self.assertEqual(self.value('enabled'), 1)
            self.assertAlmostEqual(self.value('super1'), 100/127)
            self.assertAlmostEqual(self.value('mix'), 50/127)
            for routed_deck in self.decks:
                self.assertEqual(self.value('group_[Channel'+str(routed_deck)+']_enable'), int(deck == routed_deck))
            self.state.release()
            self.assertEqual(self.value('enabled'), 0)

    def test_latch_survives_release_and_heartbeat(self):
        self.state.set_latch(True)
        self.state.press(100, 50)
        count = len(self.packets)
        self.state.release()
        self.assertEqual(len(self.packets), count)
        for tick in range(24):
            self.evaluate('clock += 250; TouchFX.watchdog();')
            self.state.heartbeat()
        self.assertEqual(self.value('enabled'), 1)
        self.state.set_latch(False)
        self.assertEqual(self.value('enabled'), 0)

    def test_deck_switch_does_not_transfer_live_latch(self):
        self.state.set_latch(True)
        self.state.press(100, 50)
        self.state.release()
        self.state.select_deck(self.decks[-1])
        self.assertEqual(self.value('enabled'), 0)
        self.assertEqual(self.value('group_[Channel1]_enable'), 0)
        self.assertEqual(self.value('group_[Channel'+str(self.decks[-1])+']_enable'), 1)
        self.state.press(10, 20)
        self.assertEqual(self.value('enabled'), 1)

    def test_panic_disables_each_deck(self):
        for deck in self.decks:
            self.state.select_deck(deck)
            self.state.press(100, 50)
            self.state.panic()
            self.assertEqual(self.value('enabled'), 0)

    def test_watchdog_requires_new_touch_after_timeout(self):
        self.state.set_latch(True)
        self.state.press(100, 50)
        self.state.release()
        self.evaluate('clock += 1500; TouchFX.watchdog();')
        self.assertEqual(self.value('enabled'), 0)
        self.state.heartbeat()
        self.assertEqual(self.value('enabled'), 0)
        self.state.press(50, 100)
        self.assertEqual(self.value('enabled'), 1)

    def test_xml_covers_note_off_and_velocity_zero(self):
        self.state.select_deck(self.decks[-1])
        self.state.press(100, 50)
        self.dispatch(mido.Message('note_off', channel=0, note=60, velocity=0))
        self.assertEqual(self.value('enabled'), 1)
        self.dispatch(mido.Message('note_on', channel=self.decks[-1]-1, note=60, velocity=0))
        self.assertEqual(self.value('enabled'), 0)

    def test_parameter_endpoints_through_actual_encoding(self):
        self.state.press(0, 127)
        self.assertEqual(self.value('super1'), 0)
        self.assertEqual(self.value('mix'), 1)
        self.state.move(127, 0)
        self.assertEqual(self.value('super1'), 1)
        self.assertEqual(self.value('mix'), 0)


@unittest.skipUnless(AVAILABLE, 'PyQt5 and mido required for generated ZED mapping integration')
class ZedProtocolPipelineTests(ProtocolPipelineTests):
    decks = (1, 2)

    def test_selector_encoding_reaches_generated_mapping_and_reports_ack(self):
        self.evaluate('''
            var previousGet = engine.getValue;
            engine.getValue = function(group, key) {
                if (key === 'loaded') return 1;
                if (key === 'loaded_effect') return values[group+'/'+key] || 0;
                if (key === 'enabled') return values[group+'/'+key] || 0;
                return previousGet(group, key);
            };
        ''')
        self.state.select_deck(2)
        self.state.select_effect(19)
        self.assertEqual(self.packets[-1], [0xB1, 13, 19])
        self.assertEqual(self.value('enabled'), 0)
        self.assertEqual(self.value('mix'), 0)
        self.assertEqual(self.evaluate('outgoing[outgoing.length-1]').toVariant(), [0xB0, 14, 19])

    def mapping_path(self):
        import test_zed_install
        fixture = test_zed_install.InstallTests()
        self.addCleanup(fixture.doCleanups)
        fixture.setUp()
        test_zed_install.installer.install(fixture.root)
        return fixture.root/test_zed_install.installer.CONTROLLERS/'TouchFX-ZED.midi.xml'

    def test_generated_mapping_uses_unit_three_without_legacy_rack_count(self):
        self.assertEqual(self.evaluate('TouchFX.unit').toString(), '[EffectRack1_EffectUnit3]')
        self.assertEqual(self.evaluate("engine.getValue('[EffectRack1]', 'num_effectunits')").toNumber(), 0)
        self.state.press(127, 64)
        self.assertEqual(self.value('enabled'), 1)
        self.assertTrue(self.evaluate("writes.every(function(write) { return /^\\[EffectRack1_EffectUnit3(?:_Effect[1-3])?\\]$/.test(write[0]); })").toBool())

    def test_skin_command_decodes_as_python_open_request(self):
        from zed_session import is_open_request
        self.assertEqual(self.evaluate('outgoing.length').toNumber(), 0)
        self.evaluate('skinConnection.callback(1);')
        packet = self.evaluate('outgoing[0]').toVariant()
        self.assertTrue(is_open_request(mido.Message.from_bytes(packet)))

    def test_generated_mapping_rejects_other_deck_channels(self):
        self.state.press(80, 64)
        count = self.evaluate('writes.length').toNumber()
        for channel in (2, 3):
            self.dispatch(mido.Message('control_change', channel=channel, control=12, value=127))
            self.dispatch(mido.Message('note_on', channel=channel, note=60, velocity=127))
            self.dispatch(mido.Message('note_off', channel=channel, note=60, velocity=0))
        self.assertEqual(self.evaluate('writes.length').toNumber(), count)
        self.assertEqual(self.value('enabled'), 1)


if __name__ == '__main__':
    unittest.main()
