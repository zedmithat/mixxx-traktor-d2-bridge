#!/usr/bin/env python3
"""PyQt5 X/Y pad with an ALSA virtual MIDI output, or a console dry run."""
import argparse
import signal
import sys
import threading
import queue
import time

from PyQt5.QtCore import QEvent, QPointF, QRectF, Qt, QTimer, pyqtSignal
from PyQt5.QtGui import QColor, QPainter, QPen
from PyQt5.QtWidgets import (
    QApplication, QButtonGroup, QComboBox, QGridLayout, QHBoxLayout, QLabel, QMainWindow,
    QPushButton, QSlider, QStackedWidget, QVBoxLayout, QWidget,
)

from touchfx_core import TouchFXState, amount_text, beat_boundaries, beat_index, beat_label, macro_controls, midi_value


class MidiOutput:
    def __init__(self, dry_run=False, control_callback=None):
        self.port = None
        self.control_port = None
        self.failed = False
        self.dry_run = dry_run
        if not dry_run:
            import mido
            self.mido = mido
            backend = mido.Backend('mido.backends.rtmidi/LINUX_ALSA')
            client_name = 'TouchFX_Virtual' if control_callback is not None else 'TouchFX'
            self.port = backend.open_output(
                'TouchFX_Virtual', virtual=True, client_name=client_name)
            if control_callback is not None:
                try:
                    self.control_port = backend.open_input('TouchFX_Virtual', virtual=True,
                        client_name=client_name, callback=control_callback)
                except Exception:
                    self.close()
                    raise

    def send(self, kind, channel, number, value):
        if self.failed:
            return
        if self.dry_run:
            print(kind, 'channel='+str(channel), 'number='+str(number), 'value='+str(value), flush=True)
            return
        fields = {'control': number, 'value': value} if kind == 'control_change' else {
            'note': number, 'velocity': value}
        try:
            self.port.send(self.mido.Message(kind, channel=channel, **fields))
        except Exception as error:
            self.failed = True
            print('MIDI bağlantısı kesildi: '+str(error), file=sys.stderr, flush=True)
            QTimer.singleShot(0, QApplication.instance().quit)

    def close(self):
        if self.control_port is not None:
            self.control_port.close()
            self.control_port = None
        if self.port is not None:
            self.port.close()
            self.port = None


class XYPad(QWidget):
    def __init__(self, state, changed):
        super().__init__()
        self.state = state
        self.changed = changed
        self.touch_id = None
        self.mouse_down = False
        self.macro = 0
        self.setAttribute(Qt.WA_AcceptTouchEvents, True)
        self.setMinimumSize(240, 200)

    def coordinates(self, position):
        return (midi_value(position.x(), self.width()),
                midi_value(position.y(), self.height(), inverted=True))

    def refresh(self):
        self.update()
        self.changed()

    def cancel(self):
        self.touch_id = None
        self.mouse_down = False
        self.state.panic()
        self.refresh()

    def event(self, event):
        if event.type() == QEvent.TouchCancel:
            self.cancel()
            event.accept()
            return True
        if event.type() in (QEvent.TouchBegin, QEvent.TouchUpdate, QEvent.TouchEnd):
            points = event.touchPoints()
            if event.type() == QEvent.TouchBegin and self.touch_id is None and not self.mouse_down:
                point = next((point for point in points if point.state() & Qt.TouchPointPressed), None)
                if point is not None:
                    self.touch_id = point.id()
                    self.state.press(*self.coordinates(point.pos()))
            point = next((point for point in points if point.id() == self.touch_id), None)
            if point is not None:
                self.state.move(*self.coordinates(point.pos()))
                if point.state() & Qt.TouchPointReleased:
                    self.state.release()
                    self.touch_id = None
            if event.type() == QEvent.TouchEnd:
                self.state.release()
                self.touch_id = None
            self.refresh()
            event.accept()
            return True
        return super().event(event)

    def mousePressEvent(self, event):
        if event.source() != Qt.MouseEventSynthesizedByQt and event.button() == Qt.LeftButton and self.touch_id is None:
            self.mouse_down = True
            self.state.press(*self.coordinates(event.localPos()))
            self.refresh()
        event.accept()

    def mouseMoveEvent(self, event):
        if self.mouse_down and event.source() != Qt.MouseEventSynthesizedByQt:
            self.state.move(*self.coordinates(event.localPos()))
            self.refresh()
        event.accept()

    def mouseReleaseEvent(self, event):
        if self.mouse_down and event.button() == Qt.LeftButton and event.source() != Qt.MouseEventSynthesizedByQt:
            self.state.move(*self.coordinates(event.localPos()))
            self.state.release()
            self.mouse_down = False
            self.refresh()
        event.accept()

    def beat_regions(self):
        controls = macro_controls(self.macro)
        regions = []
        for axis, labels in ((0, controls[3]), (1, controls[4])):
            if not labels:
                continue
            boundaries = beat_boundaries(labels)
            selected = beat_index(labels, self.state.position[axis])
            for index, label in enumerate(labels):
                lower, upper = boundaries[index:index+2]
                if axis == 0:
                    rectangle = QRectF(lower*(self.width()-1), 0,
                                       (upper-lower)*(self.width()-1), self.height())
                else:
                    rectangle = QRectF(0, (1-upper)*(self.height()-1), self.width(),
                                       (upper-lower)*(self.height()-1))
                regions.append((axis, label, rectangle, index == selected))
        return regions

    def paint_grid(self, painter):
        controls = macro_controls(self.macro)
        for fraction in (0.25, 0.5, 0.75):
            painter.setPen(QPen(QColor('#293b4d'), 1))
            if not controls[3]:
                painter.drawLine(QPointF((self.width()-1)*fraction, 0), QPointF((self.width()-1)*fraction, self.height()))
            if not controls[4]:
                painter.drawLine(QPointF(0, (self.height()-1)*fraction), QPointF(self.width(), (self.height()-1)*fraction))
        painter.save()
        font = painter.font()
        font.setPixelSize(15)
        painter.setFont(font)
        for axis, label, rectangle, selected in self.beat_regions():
            if selected:
                painter.fillRect(rectangle, QColor('#17483e' if self.state.active else '#223445'))
            painter.setPen(QPen(QColor('#385368'), 1))
            if axis == 0:
                painter.drawLine(rectangle.topLeft(), rectangle.bottomLeft())
                text_rectangle = QRectF(rectangle.left(), self.height()/2-13, rectangle.width(), 26)
            else:
                painter.drawLine(rectangle.topLeft(), rectangle.topRight())
                text_rectangle = QRectF(self.width()-66, rectangle.top(), 58, rectangle.height())
            if selected:
                painter.fillRect(text_rectangle, QColor('#206854' if self.state.active else '#344e66'))
            painter.setPen(QColor('#f2fbff' if selected else '#a8bfce'))
            painter.drawText(text_rectangle, Qt.AlignCenter, label)
        painter.restore()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        painter.fillRect(self.rect(), QColor('#101923'))
        self.paint_grid(painter)
        horizontal, vertical = self.state.position
        center = QPointF(horizontal/127*(self.width()-1), (1-vertical/127)*(self.height()-1))
        color = QColor('#32e0b5' if self.state.active else '#8195aa')
        painter.setPen(QPen(color, 2))
        painter.drawLine(QPointF(center.x(), 0), QPointF(center.x(), self.height()))
        painter.drawLine(QPointF(0, center.y()), QPointF(self.width(), center.y()))
        painter.setBrush(color)
        painter.drawEllipse(center, 12, 12)
        horizontal_name, vertical_name, _, horizontal_beats, vertical_beats = macro_controls(self.macro)
        if vertical_beats:
            vertical_name += ' · '+beat_label(vertical_beats, vertical)+' beat*'
        if horizontal_beats:
            horizontal_name += ' · '+beat_label(horizontal_beats, horizontal)+' beat*'
        if self.macro == 5:
            horizontal_name = 'LFO RATE · {:.2f} Hz'.format(0.2+2.8*horizontal/127)
        if self.macro == 10:
            vertical_name = 'LFO RATE · {:.2f} Hz'.format(0.2+2.8*vertical/127)
        if self.macro == 11:
            vertical_name += ' · Q {:.2f}'.format(0.707106781+1.792893219*vertical/127)
        painter.drawText(self.rect().adjusted(18, 14, -18, -14), Qt.AlignTop | Qt.AlignLeft, 'Y ↑ '+vertical_name)
        painter.drawText(self.rect().adjusted(18, 14, -80 if vertical_beats else -18, -14), Qt.AlignBottom | Qt.AlignRight, horizontal_name+' → X')
        if horizontal_beats or vertical_beats:
            font = painter.font()
            font.setPixelSize(13)
            painter.setFont(font)
            painter.drawText(self.rect().adjusted(18, 40, -18, -14), Qt.AlignTop | Qt.AlignLeft,
                             '* BPM/beatgrid gerekir; yoksa zaman tabanı')


class InlineEffectSelector(QComboBox):
    popupRequested = pyqtSignal()
    popupClosed = pyqtSignal()

    def showPopup(self):
        self.popupRequested.emit()

    def hidePopup(self):
        self.popupClosed.emit()


class TouchFXWindow(QMainWindow):
    def __init__(self, state, dry_run=False, zed_session=False):
        super().__init__()
        self.state = state
        self.zed_session = zed_session
        self.pending_effect = None
        self.effect_deadline = 0
        self.effect_seen = 0
        self.pending_macro = None
        self.current_macro = 0
        self.actual_flags = 0
        self.flash = False
        from zed_session import macro_choices
        self.macros = {item[0]: item for item in macro_choices()}
        self.macro_buttons = {}
        self.setWindowTitle('ZED Touch FX' if zed_session else 'Touch FX — Mixxx')
        self.setStyleSheet('''
            QWidget { background: #0b121a; color: #edf5ff; font-size: 18px; }
            QPushButton { background: #233446; border: 1px solid #486078; border-radius: 8px; padding: 8px; min-height: 40px; }
            QPushButton:checked { background: #13775f; border-color: #32e0b5; }
            QPushButton:disabled { background: #14202c; color: #60758a; border-color: #26394a; }
            QPushButton#panic { background: #912c3d; }
        ''')
        container = QWidget()
        self.pages = QStackedWidget()
        self.pad_page = container
        self.pages.addWidget(container)
        layout = QVBoxLayout(container)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(4)
        deck_row = QHBoxLayout()
        self.deck_buttons = QButtonGroup(self)
        self.deck_buttons.setExclusive(True)
        for deck in range(1, 3 if zed_session else 5):
            button = QPushButton('Deck '+str(deck))
            button.setCheckable(True)
            button.setChecked(deck == 1)
            self.deck_buttons.addButton(button, deck)
            deck_row.addWidget(button)
        self.deck_buttons.buttonClicked[int].connect(self.select_deck)
        layout.addLayout(deck_row)
        if zed_session:
            from zed_session import effect_choices
            effect_row = QHBoxLayout()
            self.effect_selector = InlineEffectSelector()
            self.effect_selector.setMinimumHeight(44)
            self.effect_selector.setStyleSheet('QComboBox QAbstractItemView { min-height: 44px; }')
            self.effect_selector.addItem('Efekt seç…', None)
            for index, label in effect_choices():
                self.effect_selector.addItem(label, index)
            self.effect_selector.setEnabled(False)
            self.effect_selector.activated[int].connect(self.select_effect)
            self.add_single_effect_page()
            self.effect_selector.popupRequested.connect(self.open_single_effect_menu)
            self.effect_selector.popupClosed.connect(self.close_macro_menu)
            self.effect_label = QLabel('Mixxx efekt bilgisi bekleniyor')
            self.effect_label.setWordWrap(True)
            self.effect_label.setMaximumWidth(170)
            effect_row.addWidget(self.effect_selector, 1)
            self.add_macro_buttons(effect_row)
            effect_row.addWidget(self.effect_label, 1)
            layout.addLayout(effect_row)
        else:
            macro_row = QHBoxLayout()
            self.add_macro_buttons(macro_row)
            layout.addLayout(macro_row)
        self.status = QLabel()
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        amount_row = QHBoxLayout()
        self.amount_title = QLabel('EK PARAMETRE —')
        self.amount_title.setStyleSheet('font-size: 14px;')
        amount_row.addWidget(self.amount_title)
        self.amount_slider = QSlider(Qt.Horizontal)
        self.amount_slider.setRange(0, 127)
        self.amount_slider.setValue(64)
        self.amount_slider.setEnabled(False)
        self.amount_slider.setMinimumHeight(28)
        self.amount_slider.valueChanged.connect(self.set_amount)
        amount_row.addWidget(self.amount_slider, 1)
        self.amount_label = QLabel('50%')
        amount_row.addWidget(self.amount_label)
        self.pad = XYPad(state, self.refresh)
        layout.addWidget(self.pad, 1)
        layout.addLayout(amount_row)
        actions = QHBoxLayout()
        self.latch_button = QPushButton('LATCH')
        self.latch_button.setCheckable(True)
        self.latch_button.toggled.connect(self.toggle_latch)
        actions.addWidget(self.latch_button)
        panic = QPushButton('FX OFF')
        panic.setObjectName('panic')
        panic.clicked.connect(self.pad.cancel)
        actions.addWidget(panic)
        reset = QPushButton('RESET FX')
        reset.clicked.connect(self.reset_fx)
        actions.addWidget(reset)
        close = QPushButton("Mixxx'e dön" if zed_session else 'Çıkış')
        close.clicked.connect(self.close)
        actions.addWidget(close)
        layout.addLayout(actions)
        layout.addWidget(QLabel('MIDI YOK — DENEME' if dry_run else
            'TouchFX_Virtual · FX Unit 3 · ZED' if zed_session else 'TouchFX_Virtual · FX Unit 1 · MIDI çift yönlü'))
        self.setCentralWidget(self.pages)
        self.resize(800, 480)
        self.refresh()

    def refresh(self):
        horizontal, vertical = self.state.position
        self.pad.macro = self.current_macro
        self.status.setText('Deck {}  |  X {:03d}  Y {:03d}  |  {}  |  {}'.format(
            self.state.channel+1, horizontal, vertical,
            'LATCH' if self.state.latch else 'MOMENTARY', 'FX ON (gönderildi)' if self.state.active else 'FX OFF'))

    def select_deck(self, deck):
        if self.zed_session and deck not in (1, 2):
            return
        self.pad.cancel()
        self.state.select_deck(deck)
        self.pad.refresh()

    def toggle_latch(self, enabled):
        self.state.set_latch(enabled)
        self.pad.refresh()

    def select_effect(self, row):
        index = self.effect_selector.itemData(row)
        if index is None:
            return
        self.pending_macro = None
        self.current_macro = 0
        self.update_parameter_labels()
        self.amount_slider.setEnabled(False)
        for button in self.macro_buttons.values():
            button.setChecked(False)
        self.pad.cancel()
        self.pending_effect = index
        self.effect_deadline = time.monotonic()+3
        self.pad.setEnabled(False)
        self.effect_label.setText('Efekt yükleniyor…')
        self.state.select_effect(index)
        self.pad.refresh()

    def effect_feedback(self, index):
        self.effect_seen = time.monotonic()
        if not self.zed_session or self.pending_macro is not None or self.current_macro:
            return
        row = self.effect_selector.findData(index)
        if self.pending_effect is not None and index != self.pending_effect:
            return
        self.pending_effect = None
        self.pad.setEnabled(index > 0)
        self.effect_selector.blockSignals(True)
        self.effect_selector.setCurrentIndex(max(0, row))
        self.effect_selector.blockSignals(False)
        self.effect_selector.setEnabled(self.effect_selector.count() > 1)
        for button in self.single_buttons:
            button.setEnabled(True)
        name = self.effect_selector.itemText(row) if row > 0 else 'Diğer / boş efekt'
        self.effect_label.setText('Unit 3 · '+name)

    def add_single_effect_page(self):
        self.single_page = QWidget()
        self.pages.addWidget(self.single_page)
        layout = QVBoxLayout(self.single_page)
        layout.addWidget(QLabel('TOUCH FX · Tek efekt'))
        grid = QGridLayout()
        self.single_buttons = []
        for row in range(1, self.effect_selector.count()):
            button = QPushButton(self.effect_selector.itemText(row))
            button.setEnabled(False)
            button.clicked.connect(lambda checked, selected=row: self.choose_single_effect(selected))
            self.single_buttons.append(button)
            grid.addWidget(button, (row-1)//3, (row-1) % 3)
        layout.addLayout(grid)
        back = QPushButton('Pad’e dön')
        back.clicked.connect(self.close_macro_menu)
        layout.addWidget(back)

    def open_single_effect_menu(self):
        self.pad.cancel()
        self.pages.setCurrentWidget(self.single_page)

    def choose_single_effect(self, row):
        self.select_effect(row)
        self.close_macro_menu()

    def add_macro_buttons(self, row):
        from zed_session import MACROS
        self.macro_menu = QPushButton('TOUCH FX · Efektler')
        self.macro_menu.setStyleSheet('font-size: 14px; padding: 4px; min-height: 34px;')
        self.macro_menu.clicked.connect(self.open_macro_menu)
        row.addWidget(self.macro_menu, 2)
        self.macro_page = QWidget()
        self.pages.addWidget(self.macro_page)
        menu_layout = QVBoxLayout(self.macro_page)
        menu_layout.addWidget(QLabel('TOUCH FX · Makrolar'))
        grid = QGridLayout()
        for position, (identifier, (label, first, second)) in enumerate(MACROS.items()):
            button = QPushButton(label)
            button.setStyleSheet('font-size: 14px; padding: 4px; min-height: 34px;')
            button.setCheckable(True)
            button.setEnabled(False)
            button.clicked.connect(lambda checked, selected=identifier: self.select_macro(selected))
            self.macro_buttons[identifier] = button
            grid.addWidget(button, position//3, position % 3)
        menu_layout.addLayout(grid)
        explanation = QLabel('Roll = Glitch; Gate = Tremolo. Gürültü seviyesi sınırlandırılmıştır.')
        explanation.setWordWrap(True)
        menu_layout.addWidget(explanation)
        self.macro_back = QPushButton('Pad’e dön')
        self.macro_back.clicked.connect(self.close_macro_menu)
        menu_layout.addWidget(self.macro_back)

    def open_macro_menu(self):
        self.pad.cancel()
        self.pages.setCurrentWidget(self.macro_page)

    def close_macro_menu(self):
        self.pages.setCurrentWidget(self.pad_page)

    def select_macro(self, identifier):
        if identifier not in self.macros:
            return
        self.close_macro_menu()
        self.pad.cancel()
        self.pending_effect = None
        self.current_macro = 0
        self.pending_macro = identifier
        self.update_parameter_labels()
        self.amount_slider.setEnabled(False)
        self.effect_deadline = time.monotonic()+3
        self.pad.setEnabled(False)
        for button in self.macro_buttons.values():
            button.setChecked(False)
        if self.zed_session:
            self.effect_label.setText('Makro yükleniyor…')
        _, label, first, second = self.macros[identifier]
        self.state.select_macro(identifier, first, second)
        self.reset_amount_display()
        self.pad.refresh()

    def macro_feedback(self, identifier):
        self.effect_seen = time.monotonic()
        for selected, button in self.macro_buttons.items():
            button.setEnabled(selected in self.macros)
        if identifier == 127:
            return
        if self.pending_macro is not None and identifier != self.pending_macro:
            return
        if self.pending_effect is not None:
            return
        self.pending_macro = None
        self.current_macro = identifier if identifier in self.macros else 0
        for selected, button in self.macro_buttons.items():
            button.setChecked(selected == self.current_macro)
        if self.current_macro:
            self.pad.setEnabled(True)
            self.macro_menu.setText(self.macros[self.current_macro][1])
            if self.zed_session:
                self.effect_selector.setCurrentIndex(0)
                self.effect_label.setText(self.macros[self.current_macro][1])
        else:
            self.macro_menu.setText('TOUCH FX · Efektler')
        self.update_parameter_labels()
        self.amount_slider.setEnabled(macro_controls(self.current_macro)[2] is not None and self.pending_macro is None)
        self.pad.refresh()

    def update_parameter_labels(self):
        _, _, amount, horizontal_beats, vertical_beats = macro_controls(self.current_macro)
        self.amount_title.setText(amount or 'EK PARAMETRE —')
        self.amount_title.setToolTip('Sürgü seçili efektin parametresini değiştirir. Yüzde çıkış ses seviyesi değildir.')
        self.amount_label.setText(amount_text(self.current_macro, self.state.amount) if amount else '—')
        beats = horizontal_beats or vertical_beats
        self.pad.setToolTip(('Ritim: '+' / '.join(beats)+'. * BPM/beatgrid gerekir; yoksa Mixxx zaman tabanına döner.')
                            if beats else 'X ve Y, seçili efektin ekranda yazan parametrelerini değiştirir.')

    def reset_amount_display(self):
        self.amount_slider.blockSignals(True)
        self.amount_slider.setValue(64)
        self.amount_slider.blockSignals(False)
        self.update_parameter_labels()

    def set_amount(self, value):
        self.state.set_amount(value)
        self.amount_label.setText(amount_text(self.current_macro, value))

    def reset_fx(self):
        self.pad.cancel()
        self.latch_button.blockSignals(True)
        self.latch_button.setChecked(False)
        self.latch_button.blockSignals(False)
        self.state.reset_fx()
        self.reset_amount_display()
        self.pending_macro = self.current_macro or None
        self.pending_effect = self.effect_selector.currentData() if self.zed_session and not self.current_macro else None
        self.effect_deadline = time.monotonic()+3
        self.pad.setEnabled(False)
        self.amount_slider.setEnabled(False)
        self.pad.refresh()

    def check_effect_timeout(self):
        now = time.monotonic()
        if self.effect_seen and now-self.effect_seen > 2:
            self.actual_flags = 0
        self.flash = not self.flash
        engaged = bool(self.actual_flags & 1) and self.pending_macro is None and self.pending_effect is None
        for identifier, button in self.macro_buttons.items():
            highlighted = engaged and identifier == self.current_macro and self.flash
            button.setStyleSheet('font-size: 14px; padding: 4px; min-height: 34px;'+
                ('background: #23b394; color: #061610;' if highlighted else ''))
        self.macro_menu.setStyleSheet('font-size: 14px; padding: 4px; min-height: 34px;'+
            ('background: #23b394; color: #061610;' if engaged and self.flash else ''))
        if (self.pending_effect is not None or self.pending_macro is not None) and now >= self.effect_deadline:
            self.pending_effect = None
            self.pending_macro = None
            self.pad.cancel()
            if self.zed_session:
                self.effect_label.setText('Seçim doğrulanamadı · tekrar seç')
        if self.effect_seen and now-self.effect_seen > 2:
            self.effect_seen = 0
            self.pad.cancel()
            self.pad.setEnabled(False)
            self.amount_slider.setEnabled(False)
            for button in self.macro_buttons.values():
                button.setEnabled(False)
            if self.zed_session:
                self.effect_selector.setEnabled(False)
                for button in self.single_buttons:
                    button.setEnabled(False)
                self.effect_label.setText('Mixxx bağlantısı bekleniyor')

    def changeEvent(self, event):
        if event.type() == QEvent.ActivationChange and not self.isActiveWindow() and hasattr(self, 'pad'):
            if self.state.pressed:
                self.pad.cancel()
        super().changeEvent(event)

    def closeEvent(self, event):
        self.pad.cancel()
        self.close_macro_menu()
        if self.zed_session:
            self.hide()
            event.ignore()
        else:
            event.accept()

    def open_pad(self):
        self.showFullScreen()
        self.raise_()
        self.activateWindow()

    def keyPressEvent(self, event):
        if event.key() == Qt.Key_Escape:
            if self.pages.currentWidget() is not self.pad_page:
                self.close_macro_menu()
            else:
                self.close()
        else:
            super().keyPressEvent(event)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--windowed', action='store_true')
    mode.add_argument('--frameless', action='store_true')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--zed-session', action='store_true')
    args = parser.parse_args()
    if args.zed_session and not args.dry_run:
        from zed_session import prepare_wayland
        try:
            prepare_wayland()
        except (OSError, RuntimeError) as error:
            print('ZED display startup failed: '+str(error), file=sys.stderr)
            return 1
    app = QApplication([sys.argv[0]])
    requested = threading.Event()
    feedback = queue.Queue(maxsize=16)
    from zed_session import is_open_request
    if args.zed_session:
        from zed_session import is_open_request, notify_ready
        app.setDesktopFileName('org.zed.TouchFX')
        app.setQuitOnLastWindowClosed(False)
    def on_control(message):
        if is_open_request(message):
            requested.set()
        elif message.type == 'control_change' and message.channel == 0 and message.control in (14, 15, 18):
            try:
                feedback.put_nowait((message.control, message.value))
            except queue.Full:
                pass
    try:
        output = MidiOutput(args.dry_run, on_control)
    except Exception as error:
        print('Sanal ALSA MIDI portu açılamadı: {}\nREADME içindeki ALSA ve venv adımlarını kontrol edin.'.format(error), file=sys.stderr)
        return 1
    state = TouchFXState(output.send)
    window = TouchFXWindow(state, args.dry_run, args.zed_session)
    timer = QTimer(app)
    timer.setInterval(250)
    def tick():
        while not feedback.empty():
            control, value = feedback.get_nowait()
            if control == 18:
                window.macro_feedback(value)
            elif control == 15:
                window.actual_flags = value
            else:
                window.effect_feedback(value)
        window.check_effect_timeout()
        if requested.is_set():
            requested.clear()
            window.open_pad()
        state.heartbeat()
    timer.timeout.connect(tick)
    def cleanup():
        timer.stop()
        state.panic()
        output.close()
    app.aboutToQuit.connect(cleanup)
    signal.signal(signal.SIGINT, lambda signum, frame: app.quit())
    signal.signal(signal.SIGTERM, lambda signum, frame: app.quit())
    state.start()
    timer.start()
    if args.zed_session:
        try:
            notify_ready()
        except OSError as error:
            cleanup()
            print('ZED readiness notification failed: '+str(error), file=sys.stderr)
            return 1
    elif args.frameless:
        window.setWindowFlag(Qt.FramelessWindowHint)
        window.showMaximized()
    elif args.windowed:
        window.show()
    else:
        window.showFullScreen()
    result = app.exec_()
    return 1 if output.failed else result


if __name__ == '__main__':
    sys.exit(main())
