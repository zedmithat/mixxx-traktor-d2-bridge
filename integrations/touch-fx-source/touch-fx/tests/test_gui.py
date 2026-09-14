import importlib.util
import os
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

os.environ['QT_QPA_PLATFORM'] = 'offscreen'
BASE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BASE))
HAS_QT = importlib.util.find_spec('PyQt5') is not None
HAS_MIDO = importlib.util.find_spec('mido') is not None
if HAS_QT:
    from PyQt5.QtCore import QEvent, QPoint, QPointF, Qt
    from PyQt5.QtGui import QMouseEvent
    from PyQt5.QtTest import QTest
    from PyQt5.QtWidgets import QApplication, QPushButton
    from touchfx import MidiOutput, TouchFXWindow
    from touchfx_core import TouchFXState
    from qt_test_support import prepare_fonts


@unittest.skipUnless(HAS_QT, 'PyQt5 required for actual offscreen widget tests')
class GuiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])
        cls.app.setQuitOnLastWindowClosed(False)
        prepare_fonts(cls.app)

    def setUp(self):
        self.messages = []
        self.state = TouchFXState(lambda *message: self.messages.append(message))
        self.window = TouchFXWindow(self.state, dry_run=True)
        self.window.show()
        self.app.processEvents()

    def test_zed_display_failure_exits_before_qt_or_midi_creation(self):
        import touchfx
        with patch.object(sys, 'argv', ['touchfx.py', '--zed-session']), \
                patch('zed_session.prepare_wayland', side_effect=RuntimeError('not ready')), \
                patch.object(touchfx, 'QApplication') as application, \
                patch.object(touchfx, 'MidiOutput') as output, \
                patch.object(sys, 'stderr'):
            self.assertEqual(touchfx.main(), 1)
            application.assert_not_called()
            output.assert_not_called()

    def tearDown(self):
        self.window.close()
        self.window.deleteLater()
        self.app.processEvents()

    def touch(self, event_type, points):
        event = SimpleNamespace(type=lambda: event_type, touchPoints=lambda: [
            SimpleNamespace(id=lambda identifier=identifier: identifier,
                pos=lambda position=position: QPointF(*position), state=lambda phase=phase: phase)
            for identifier, position, phase in points], accept=Mock())
        self.assertTrue(self.window.pad.event(event))
        event.accept.assert_called_once()

    def test_default_window_fits_800_by_480(self):
        self.assertEqual((self.window.width(), self.window.height()), (800, 480))
        self.assertGreaterEqual(self.window.pad.height(), 200)
        self.assertLessEqual(self.window.minimumSizeHint().width(), 800)

    def test_beat_regions_follow_midi_steps_on_both_axes_and_after_resize(self):
        from touchfx_core import beat_label, macro_controls
        pad = self.window.pad
        for width, height in ((784, 202), (240, 200), (1264, 482)):
            pad.resize(width, height)
            for identifier, axis in ((1, 1), (4, 1), (6, 1), (7, 1), (8, 0), (9, 0)):
                pad.macro = identifier
                labels = macro_controls(identifier)[3+axis]
                for midi in range(128):
                    self.state.position = (midi, midi)
                    regions = pad.beat_regions()
                    self.assertEqual(len(regions), len(labels))
                    selected = [region for region in regions if region[3]]
                    self.assertEqual(len(selected), 1)
                    selected_axis, label, rectangle, _ = selected[0]
                    self.assertEqual(selected_axis, axis)
                    self.assertEqual(label, beat_label(labels, midi))
                    point = QPointF(midi/127*(width-1), (1-midi/127)*(height-1))
                    self.assertTrue(rectangle.adjusted(-0.0001, -0.0001, 0.0001, 0.0001).contains(point))

    def test_grid_paint_and_latch_highlight_do_not_send_midi(self):
        pad = self.window.pad
        for identifier in (1, 9):
            self.window.current_macro = identifier
            self.state.position = (64, 64)
            self.window.refresh()
            self.messages.clear()
            inactive = pad.grab().toImage()
            self.state.active = True
            self.state.pressed = False
            self.state.latch = True
            active = pad.grab().toImage()
            self.assertNotEqual(inactive, active)
            self.assertEqual(self.messages, [])
            self.state.active = False
        self.window.current_macro = 11
        self.window.refresh()
        self.assertEqual(pad.beat_regions(), [])
        self.window.current_macro = 0
        self.window.refresh()
        self.assertEqual(pad.beat_regions(), [])

    def zed_window(self):
        self.window.close()
        self.window.deleteLater()
        with patch('zed_session.effect_choices', return_value=[(3, 'Filter'), (8, 'Echo')]):
            self.window = TouchFXWindow(self.state, dry_run=True, zed_session=True)
        self.window.show()
        self.app.processEvents()

    def test_zed_selector_waits_for_actual_effect_ack_and_fits_screen(self):
        self.zed_window()
        self.assertFalse(self.window.effect_selector.isEnabled())
        self.window.effect_feedback(3)
        self.assertEqual(self.window.effect_selector.currentText(), 'Filter')
        self.window.select_effect(2)
        self.assertFalse(self.window.pad.isEnabled())
        self.window.effect_feedback(3)
        self.assertEqual(self.window.pending_effect, 8)
        self.window.effect_feedback(8)
        self.assertIsNone(self.window.pending_effect)
        self.assertTrue(self.window.pad.isEnabled())
        self.assertEqual(self.window.effect_selector.currentText(), 'Echo')
        self.assertEqual((self.window.width(), self.window.height()), (800, 480))
        self.assertGreaterEqual(self.window.pad.height(), 200)

    def test_zed_selector_timeout_and_disconnect_bypass(self):
        self.zed_window()
        self.window.effect_feedback(3)
        self.window.select_effect(2)
        with patch('touchfx.time.monotonic', return_value=self.window.effect_deadline+1):
            self.window.check_effect_timeout()
        self.assertFalse(self.state.active)
        self.assertFalse(self.window.pad.isEnabled())
        self.assertFalse(self.window.effect_selector.isEnabled())
        self.window.effect_feedback(3)
        self.assertTrue(self.window.pad.isEnabled())

    def test_deck_switch_clears_old_touch_ownership_for_next_press(self):
        self.touch(QEvent.TouchBegin, [(10, (80, 80), Qt.TouchPointPressed)])
        self.window.select_deck(2)
        self.assertIsNone(self.window.pad.touch_id)
        self.assertFalse(self.state.pressed)
        self.touch(QEvent.TouchBegin, [(20, (80, 80), Qt.TouchPointPressed)])
        self.assertTrue(self.state.active)
        self.assertEqual(self.messages[-1], ('note_on', 1, 60, 127))

    def test_macro_waits_for_ack_then_amount_reset_and_feedback_flash(self):
        self.zed_window()
        self.window.macros = {1: (1, 'Filter Echo', 3, 8)}
        self.window.macro_feedback(0)
        self.assertTrue(self.window.macro_buttons[1].isEnabled())
        self.window.select_macro(1)
        self.assertFalse(self.window.pad.isEnabled())
        self.assertEqual(self.messages[-4:], [('control_change', 0, 16, 1),
            ('control_change', 0, 20, 3), ('control_change', 0, 21, 8), ('control_change', 0, 22, 1)])
        self.window.macro_feedback(127)
        self.assertFalse(self.window.pad.isEnabled())
        self.window.macro_feedback(1)
        self.assertTrue(self.window.pad.isEnabled())
        self.assertTrue(self.window.amount_slider.isEnabled())
        self.window.amount_slider.setValue(127)
        self.assertEqual(self.messages[-1], ('control_change', 0, 24, 127))
        self.window.actual_flags = 23
        self.window.flash = False
        self.window.check_effect_timeout()
        self.assertIn('#23b394', self.window.macro_menu.styleSheet())
        self.window.latch_button.setChecked(True)
        self.state.press(90, 90)
        self.window.reset_fx()
        self.assertFalse(self.state.active)
        self.assertFalse(self.state.latch)
        self.assertFalse(self.window.pad.isEnabled())
        self.assertEqual(self.window.amount_slider.value(), 64)
        self.assertEqual(self.messages[-1], ('control_change', 0, 26, 127))
        self.window.macro_feedback(1)
        self.assertTrue(self.window.pad.isEnabled())
        self.window.open_macro_menu()
        self.app.processEvents()
        self.assertLessEqual(self.window.macro_page.width(), 800)
        self.assertLessEqual(self.window.macro_page.height(), 480)
        self.assertFalse(self.window.macro_page.isWindow())
        self.window.close_macro_menu()

    def test_feedback_loss_clears_flash_and_disables_pad_and_amount(self):
        self.zed_window()
        self.window.macros = {1: (1, 'Filter Echo', 3, 8)}
        self.window.macro_feedback(1)
        self.window.actual_flags = 23
        with patch('touchfx.time.monotonic', return_value=self.window.effect_seen+3):
            self.window.check_effect_timeout()
        self.assertEqual(self.window.actual_flags, 0)
        self.assertFalse(self.window.pad.isEnabled())
        self.assertFalse(self.window.amount_slider.isEnabled())
        self.assertNotIn('#23b394', self.window.macro_menu.styleSheet())

    def test_macro_axes_and_extra_slider_change_only_after_ack(self):
        from touchfx_core import MACRO_CONTROLS
        self.zed_window()
        self.window.macros = {identifier: (identifier, 'Test '+str(identifier), 3,
            0 if identifier in (5, 9, 10, 11) else 8) for identifier in MACRO_CONTROLS}
        for identifier, controls in MACRO_CONTROLS.items():
            self.window.select_macro(identifier)
            self.assertFalse(self.window.amount_slider.isEnabled())
            self.assertEqual(self.window.pad.macro, 0)
            self.window.macro_feedback(identifier)
            self.assertEqual(self.window.pad.macro, identifier)
            self.assertEqual(self.window.amount_title.text(), controls[2] or 'EK PARAMETRE —')
            self.assertEqual(self.window.amount_slider.isEnabled(), controls[2] is not None)
            self.app.processEvents()
            self.assertEqual((self.window.width(), self.window.height()), (800, 480))
            self.assertGreaterEqual(self.window.pad.height(), 200)
        self.window.select_effect(1)
        self.assertEqual(self.window.pad.macro, 0)
        self.assertFalse(self.window.amount_slider.isEnabled())

    def test_zed_menu_is_same_window_and_does_not_start_nested_event_loop(self):
        self.zed_window()
        self.state.press(70, 70)
        original_handle = self.window.windowHandle()
        for cycle in range(20):
            QTest.mouseClick(self.window.macro_menu, Qt.LeftButton)
            self.assertIs(self.window.pages.currentWidget(), self.window.macro_page)
            self.assertIsNone(self.app.activeModalWidget())
            self.assertIs(self.window.macro_page.window(), self.window)
            QTest.mouseClick(self.window.macro_back, Qt.LeftButton)
            self.assertIs(self.window.pages.currentWidget(), self.window.pad_page)
            self.assertIs(self.window.windowHandle(), original_handle)
        self.assertFalse(self.state.active)

    def test_escape_from_menu_returns_to_pad_and_second_escape_hides_zed(self):
        self.zed_window()
        self.window.open_macro_menu()
        QTest.keyClick(self.window, Qt.Key_Escape)
        self.assertTrue(self.window.isVisible())
        self.assertIs(self.window.pages.currentWidget(), self.window.pad_page)
        QTest.keyClick(self.window, Qt.Key_Escape)
        self.assertFalse(self.window.isVisible())

    def test_menu_selection_returns_to_pad_without_arming_until_ack(self):
        self.zed_window()
        self.window.macros = {1: (1, 'Filter Echo', 3, 8)}
        self.window.macro_feedback(0)
        QTest.mouseClick(self.window.macro_menu, Qt.LeftButton)
        QTest.mouseClick(self.window.macro_buttons[1], Qt.LeftButton)
        self.assertIs(self.window.pages.currentWidget(), self.window.pad_page)
        self.assertFalse(self.window.pad.isEnabled())
        self.assertFalse(self.state.active)
        self.window.macro_feedback(1)
        self.assertTrue(self.window.pad.isEnabled())

    def test_single_effect_selector_opens_embedded_page_without_popup(self):
        self.zed_window()
        self.window.effect_feedback(3)
        QTest.mouseClick(self.window.effect_selector, Qt.LeftButton)
        self.assertIs(self.window.pages.currentWidget(), self.window.single_page)
        self.assertIsNone(self.app.activePopupWidget())
        self.assertIsNone(self.app.activeModalWidget())
        self.assertIs(self.window.single_page.window(), self.window)
        QTest.mouseClick(self.window.single_buttons[1], Qt.LeftButton)
        self.assertEqual(self.window.pending_effect, 8)
        self.assertIs(self.window.pages.currentWidget(), self.window.pad_page)
        self.assertFalse(self.window.pad.isEnabled())
        self.window.effect_feedback(8)
        self.assertTrue(self.window.pad.isEnabled())

    def test_repeated_touch_and_deck_switches_need_only_one_press(self):
        for cycle in range(40):
            self.window.select_deck(cycle % 2+1)
            self.touch(QEvent.TouchBegin, [(cycle, (80, 80), Qt.TouchPointPressed)])
            self.assertTrue(self.state.active)
            self.touch(QEvent.TouchEnd, [(cycle, (80, 80), Qt.TouchPointReleased)])
            self.assertFalse(self.state.active)
            self.assertIsNone(self.window.pad.touch_id)


    def test_active_status_does_not_widen_window(self):
        self.state.select_deck(4)
        self.state.set_latch(True)
        self.state.press(127, 127)
        self.window.pad.refresh()
        self.app.processEvents()
        self.assertEqual((self.window.width(), self.window.height()), (800, 480))
        self.assertGreaterEqual(self.window.pad.height(), 200)

    def test_mouse_press_and_release_send_gate(self):
        QTest.mousePress(self.window.pad, Qt.LeftButton, pos=QPoint(10, 10))
        self.assertTrue(self.state.active)
        QTest.mouseRelease(self.window.pad, Qt.LeftButton, pos=QPoint(20, 20))
        self.assertFalse(self.state.active)
        self.assertEqual(self.messages[-1], ('note_off', 0, 60, 0))

    def test_widget_coordinates_cover_all_endpoints(self):
        pad = self.window.pad
        self.assertEqual(pad.coordinates(QPointF(0, 0)), (0, 127))
        self.assertEqual(pad.coordinates(QPointF(pad.width()-1, pad.height()-1)), (127, 0))

    def test_latch_button_keeps_gate_until_unchecked(self):
        QTest.mouseClick(self.window.latch_button, Qt.LeftButton)
        QTest.mouseClick(self.window.pad, Qt.LeftButton, pos=QPoint(100, 100))
        self.assertTrue(self.state.active)
        self.assertFalse(self.state.pressed)
        QTest.mouseClick(self.window.latch_button, Qt.LeftButton)
        self.assertFalse(self.state.active)

    def test_deck_button_clears_previous_latch(self):
        QTest.mouseClick(self.window.latch_button, Qt.LeftButton)
        QTest.mouseClick(self.window.pad, Qt.LeftButton, pos=QPoint(100, 100))
        QTest.mouseClick(self.window.deck_buttons.button(4), Qt.LeftButton)
        self.assertEqual(self.state.channel, 3)
        self.assertFalse(self.state.active)
        self.assertTrue(self.window.deck_buttons.button(4).isChecked())
        self.assertIn('Deck 4', self.window.status.text())

    def test_fx_off_button_overrides_latch(self):
        self.state.set_latch(True)
        self.state.press(100, 100)
        self.state.release()
        QTest.mouseClick(self.window.findChild(QPushButton, 'panic'), Qt.LeftButton)
        self.assertFalse(self.state.active)
        self.assertIn('FX OFF', self.window.status.text())

    def test_first_touch_point_owns_pad(self):
        self.touch(QEvent.TouchBegin, [(10, (10, 10), Qt.TouchPointPressed)])
        self.touch(QEvent.TouchUpdate, [(10, (20, 30), Qt.TouchPointMoved),
            (11, (200, 200), Qt.TouchPointPressed)])
        self.assertEqual(self.state.position, self.window.pad.coordinates(QPointF(20, 30)))
        self.touch(QEvent.TouchUpdate, [(10, (20, 30), Qt.TouchPointReleased),
            (11, (200, 200), Qt.TouchPointStationary)])
        self.assertFalse(self.state.active)
        self.touch(QEvent.TouchUpdate, [(11, (300, 300), Qt.TouchPointMoved)])
        self.assertFalse(self.state.active)

    def test_touch_cancel_always_disables(self):
        self.state.set_latch(True)
        self.touch(QEvent.TouchBegin, [(10, (10, 10), Qt.TouchPointPressed)])
        self.touch(QEvent.TouchCancel, [])
        self.assertFalse(self.state.active)
        self.assertIsNone(self.window.pad.touch_id)

    def test_empty_touch_end_releases_momentary(self):
        self.touch(QEvent.TouchBegin, [(10, (10, 10), Qt.TouchPointPressed)])
        self.touch(QEvent.TouchEnd, [])
        self.assertFalse(self.state.active)

    def test_qt_synthesized_mouse_does_not_duplicate_touch(self):
        event = QMouseEvent(QEvent.MouseButtonPress, QPointF(10, 10), QPointF(10, 10), QPointF(10, 10),
            Qt.LeftButton, Qt.LeftButton, Qt.NoModifier, Qt.MouseEventSynthesizedByQt)
        self.window.pad.mousePressEvent(event)
        self.assertEqual(self.messages, [])

    def test_focus_loss_during_press_disables(self):
        self.state.press(100, 100)
        with patch.object(self.window, 'isActiveWindow', return_value=False):
            self.window.changeEvent(QEvent(QEvent.ActivationChange))
        self.assertFalse(self.state.active)

    def test_focus_loss_after_latch_release_keeps_latch(self):
        self.state.set_latch(True)
        self.state.press(100, 100)
        self.state.release()
        with patch.object(self.window, 'isActiveWindow', return_value=False):
            self.window.changeEvent(QEvent(QEvent.ActivationChange))
        self.assertTrue(self.state.active)

    def test_close_sends_off_for_every_channel(self):
        self.state.press(100, 100)
        self.messages.clear()
        self.window.close()
        self.assertEqual(self.messages, [('note_off', channel, 60, 0) for channel in range(4)])

    def test_escape_closes_window(self):
        QTest.keyClick(self.window, Qt.Key_Escape)
        self.assertFalse(self.window.isVisible())

    def test_zed_return_hides_without_destroying_pad(self):
        self.window.close()
        self.window = TouchFXWindow(self.state, dry_run=True, zed_session=True)
        self.window.open_pad()
        self.state.set_latch(True)
        self.state.press(100, 100)
        self.state.release()
        self.window.close()
        self.assertFalse(self.window.isVisible())
        self.assertFalse(self.state.active)
        self.window.open_pad()
        self.assertTrue(self.window.isVisible())

    def test_zed_shows_only_two_decks_and_rejects_unsupported_decks(self):
        self.window.close()
        self.window = TouchFXWindow(self.state, dry_run=True, zed_session=True)
        self.assertEqual([button.text() for button in self.window.deck_buttons.buttons()], ['Deck 1', 'Deck 2'])
        for deck in (3, 4):
            self.assertIsNone(self.window.deck_buttons.button(deck))
            self.window.select_deck(deck)
            self.assertEqual(self.state.channel, 0)
        self.window.select_deck(2)
        self.assertEqual(self.state.channel, 1)

    def test_zed_starts_hidden_and_uses_stable_title(self):
        self.window.close()
        self.window = TouchFXWindow(self.state, dry_run=True, zed_session=True)
        self.assertFalse(self.window.isVisible())
        self.assertEqual(self.window.windowTitle(), 'ZED Touch FX')


@unittest.skipUnless(HAS_QT and HAS_MIDO, 'PyQt5 and mido required; ALSA port is mocked')
class TransportTests(unittest.TestCase):
    def test_zed_ports_have_matching_names_and_close_together(self):
        callback = Mock()
        with patch('mido.Backend') as backend:
            output = MidiOutput(control_callback=callback)
            backend.return_value.open_input.assert_called_once_with('TouchFX_Virtual', virtual=True,
                client_name='TouchFX_Virtual', callback=callback)
            backend.return_value.open_output.assert_called_once_with('TouchFX_Virtual', virtual=True,
                client_name='TouchFX_Virtual')
            output.close()
            backend.return_value.open_input.return_value.close.assert_called_once()
            backend.return_value.open_output.return_value.close.assert_called_once()

    def test_input_port_failure_closes_output_before_raising(self):
        with patch('mido.Backend') as backend:
            backend.return_value.open_input.side_effect = OSError('test failure')
            with self.assertRaises(OSError):
                MidiOutput(control_callback=Mock())
            backend.return_value.open_output.return_value.close.assert_called_once()

    def test_virtual_port_configuration_and_actual_midi_encoding(self):
        with patch('mido.Backend') as backend:
            output = MidiOutput()
            backend.assert_called_once_with('mido.backends.rtmidi/LINUX_ALSA')
            backend.return_value.open_output.assert_called_once_with(
                'TouchFX_Virtual', virtual=True, client_name='TouchFX')
            port = output.port
            state = TouchFXState(output.send)
            state.select_deck(4)
            state.press(127, 0)
            state.release()
            messages = [call.args[0].bytes() for call in port.send.call_args_list]
            self.assertEqual(messages[-4:], [[0xB3, 10, 127], [0xB3, 11, 0],
                [0x93, 60, 127], [0x83, 60, 0]])
            output.close()
            output.close()
            port.close.assert_called_once()

    def test_send_failure_schedules_quit_and_stops_sending(self):
        with patch('mido.Backend'), patch('touchfx.QTimer.singleShot') as schedule, \
                patch('touchfx.QApplication.instance') as app, patch('builtins.print'):
            output = MidiOutput()
            output.port.send.side_effect = OSError('disconnected')
            output.send('note_on', 0, 60, 127)
            output.send('note_off', 0, 60, 0)
            self.assertTrue(output.failed)
            self.assertEqual(output.port.send.call_count, 1)
            schedule.assert_called_once_with(0, app.return_value.quit)


if __name__ == '__main__':
    unittest.main()
