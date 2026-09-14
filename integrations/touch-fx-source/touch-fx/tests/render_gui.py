"""Render the real PyQt5 window without a display, MIDI port, or audio devices."""
import argparse
import json
import os
from pathlib import Path
import platform
import sys
from unittest.mock import patch

os.environ['QT_QPA_PLATFORM'] = 'offscreen'
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from PyQt5.QtCore import PYQT_VERSION_STR, QT_VERSION_STR
from PyQt5.QtWidgets import QApplication
from touchfx import TouchFXWindow
from touchfx_core import TouchFXState
from qt_test_support import prepare_fonts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--zed-session', action='store_true')
    parser.add_argument('--macro', type=int, choices=range(1, 12))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    app = QApplication([])
    font_family = prepare_fonts(app)
    state = TouchFXState(lambda *message: None)
    with patch('zed_session.effect_choices', return_value=[(3, 'Filter'), (8, 'Echo')]):
        window = TouchFXWindow(state, dry_run=True, zed_session=args.zed_session)
    if args.macro is not None:
        from zed_session import MACROS
        identifier = args.macro
        window.macros = {identifier: (identifier, MACROS[identifier][0], 3,
            0 if identifier in (5, 9, 10, 11) else 8)}
        window.select_macro(identifier)
        window.macro_feedback(identifier)
    window.show()
    app.processEvents()
    captures = {}
    deck = 2 if args.zed_session else 4
    for label, latch in (('momentary-off', False), ('deck'+str(deck)+'-latch-on', True)):
        if latch:
            window.deck_buttons.button(deck).click()
            window.latch_button.click()
            state.press(94, 83)
            state.release()
            window.pad.refresh()
        app.processEvents()
        filename = args.output/(label+'.png')
        if not window.grab().save(str(filename)):
            raise RuntimeError('Could not save screenshot')
        captures[label] = {'width': window.width(), 'height': window.height(),
            'pad_width': window.pad.width(), 'pad_height': window.pad.height()}
    window.close()
    record = {'python': platform.python_version(), 'platform': platform.platform(),
        'pyqt': PYQT_VERSION_STR, 'qt': QT_VERSION_STR, 'qpa': 'offscreen', 'font_family': font_family,
        'real_midi_tested': False, 'pi_hardware_tested': False, 'zed_session': args.zed_session,
        'simulated_macro_ack': args.macro, 'captures': captures}
    (args.output/'render.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
