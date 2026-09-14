"""Exercise a separate, MIDI-free Qt window against the real owner compositor."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--legacy', action='store_true')
args = parser.parse_args()
if args.output.exists():
    raise RuntimeError('Existing evidence directory')
args.output.mkdir()
sys.path.insert(0, str(args.source))
os.environ['QT_QPA_PLATFORM'] = 'wayland'
os.environ['XDG_RUNTIME_DIR'] = '/run/user/1000'
os.environ['WAYLAND_DISPLAY'] = 'wayland-1'
sockets = list(Path('/run/user/1000').glob('sway-ipc.*.sock'))
assert len(sockets) == 1
os.environ['SWAYSOCK'] = str(sockets[0])
from PyQt5.QtCore import QPoint, Qt, QTimer
from PyQt5.QtTest import QTest
from PyQt5.QtWidgets import QApplication
from touchfx import TouchFXWindow
from touchfx_core import TouchFXState

app = QApplication([])
app.setDesktopFileName('org.zed.TouchFX')
app.setQuitOnLastWindowClosed(False)
packets = []
state = TouchFXState(lambda *message: packets.append(message))
window = TouchFXWindow(state, dry_run=True, zed_session=True)
window.macros = {1: (1, 'Filter Echo', 14, 15)}
window.macro_feedback(0)
observations = []


def snapshot(label):
    tree = json.loads(subprocess.run(['swaymsg', '-t', 'get_tree', '-r'],
        text=True, capture_output=True, check=True, timeout=5).stdout)
    def own(node):
        if node.get('pid') == os.getpid() and node.get('app_id'):
            yield {key: node.get(key) for key in ('id', 'name', 'visible', 'focused', 'fullscreen_mode', 'rect')}
        for child in node.get('nodes', [])+node.get('floating_nodes', []):
            yield from own(child)
    result = {'label': label, 'windows': list(own(tree)), 'modal': app.activeModalWidget() is not None}
    observations.append(result)
    return result


result = {'status': 'running', 'real_wayland': True, 'midi_ports_created': 0, 'observations': observations}
try:
    window.showFullScreen()
    QTest.qWait(450)
    if args.legacy:
        def capture_legacy():
            snapshot('legacy menu')
            subprocess.run(['grim', str(args.output/'menu.png')], check=True, timeout=5)
            window.macro_dialog.reject()
        QTimer.singleShot(350, capture_legacy)
        QTest.mouseClick(window.macro_menu, Qt.LeftButton)
        QTest.qWait(150)
        assert observations[-1]['modal'] and len(observations[-1]['windows']) > 1
        result['status'] = 'legacy-modal-reproduced'
    else:
        for cycle in range(20):
            QTest.mouseClick(window.macro_menu, Qt.LeftButton)
            QTest.qWait(70)
            menu = snapshot('menu '+str(cycle))
            assert len(menu['windows']) == 1 and not menu['modal']
            assert menu['windows'][0]['visible'] and menu['windows'][0]['fullscreen_mode'] == 2
            assert window.pages.currentWidget() is window.macro_page
            if cycle == 0:
                subprocess.run(['grim', str(args.output/'menu.png')], check=True, timeout=5)
            if cycle % 2:
                QTest.mouseClick(window.macro_back, Qt.LeftButton)
            else:
                QTest.mouseClick(window.macro_buttons[1], Qt.LeftButton)
                window.macro_feedback(1)
            QTest.qWait(70)
            pad = snapshot('pad '+str(cycle))
            assert len(pad['windows']) == 1 and not pad['modal']
            assert pad['windows'][0]['visible'] and pad['windows'][0]['fullscreen_mode'] == 2
            assert window.pages.currentWidget() is window.pad_page
            assert not state.active
        QTest.mousePress(window.pad, Qt.LeftButton, pos=QPoint(120, 120))
        assert state.active
        QTest.mouseRelease(window.pad, Qt.LeftButton, pos=QPoint(120, 120))
        assert not state.active
        window.current_macro = 0
        window.effect_feedback(14)
        window.effect_selector.showPopup()
        QTest.qWait(100)
        selection = snapshot('single effect menu')
        assert len(selection['windows']) == 1 and not selection['modal']
        assert selection['windows'][0]['visible'] and selection['windows'][0]['fullscreen_mode'] == 2
        assert app.activePopupWidget() is None
        subprocess.run(['grim', str(args.output/'single-effect.png')], check=True, timeout=5)
        window.effect_selector.hidePopup()
        QTest.qWait(100)
        restored = snapshot('single effect closed')
        assert len(restored['windows']) == 1 and not restored['modal']
        assert restored['windows'][0]['visible'] and restored['windows'][0]['fullscreen_mode'] == 2
        subprocess.run(['grim', str(args.output/'pad.png')], check=True, timeout=5)
        result['status'] = 'passed'
except Exception as error:
    result['status'] = 'failed'
    result['error'] = repr(error)
finally:
    window.hide()
    app.processEvents()
    (args.output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({key: value for key, value in result.items() if key != 'observations'},
        indent=2))
    print('Window observations:', len(observations))
if result['status'] == 'failed':
    sys.exit(1)
