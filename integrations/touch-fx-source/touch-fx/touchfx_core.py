"""Transport-independent Touch FX state machine; MIDI channels are zero-based."""

CC_X = 10
CC_Y = 11
CC_DECK = 12
CC_EFFECT = 13
CC_HEARTBEAT = 119
NOTE_GATE = 60

ECHO_BEATS = ('2', '1', '3/4', '1/2', '1/4', '1/8')
GATE_BEATS = ('4', '2', '1', '1/2', '1/4', '1/8')
FLANGER_BEATS = ('16', '8', '4', '2', '1', '1/2')
MACRO_CONTROLS = {
    1: ('FILTER LP / HP', 'ECHO TIME', 'FEEDBACK', None, ECHO_BEATS),
    2: ('REVERB DECAY', 'FLANGER WIDTH', 'REVERB SEND', None, None),
    3: ('FILTER LP / HP', 'REVERB DECAY', 'REVERB SEND', None, None),
    4: ('FILTER LP / HP', 'ROLL TIME', 'RESONANCE', None, ECHO_BEATS),
    5: ('LFO RATE (0.2–3 Hz)', 'ECHO SEND', 'FEEDBACK', None, None),
    6: ('FILTER LP / HP', 'ECHO TIME', 'DUB FEEDBACK', None, ECHO_BEATS),
    7: ('FILTER LP / HP', 'GATE PERIOD', 'GATE DEPTH', None, GATE_BEATS),
    8: ('GATE PERIOD', 'GATE DEPTH', 'NOISE LEVEL', GATE_BEATS, None),
    9: ('FLANGER PERIOD', 'FLANGER WIDTH', 'REGEN', FLANGER_BEATS, None),
    10: ('FILTER CENTER', 'LFO RATE (0.2–3 Hz)', 'LFO DEPTH', None, None),
    11: ('FILTER LP / HP', 'RESONANCE', None, None, None),
}


def macro_controls(identifier):
    return MACRO_CONTROLS.get(identifier, ('SUPER1', 'DRY / WET', None, None, None))


def beat_index(labels, midi):
    return min(len(labels)-1, max(0, int(midi))*len(labels)//128)


def beat_label(labels, midi):
    return labels[beat_index(labels, midi)]


def beat_boundaries(labels):
    count = len(labels)
    return (0.0, *(((128*index+count-1)//count-0.5)/127
                   for index in range(1, count)), 1.0)


def amount_text(identifier, midi):
    fraction = max(0, min(127, midi))/127
    if identifier == 4:
        return 'Q {:.2f}'.format(0.707106781+1.792893219*fraction)
    if identifier in (1, 5, 6):
        fraction = (0.35 if identifier == 6 else 0.15)+0.4*fraction
    elif identifier in (2, 3):
        fraction *= 0.65
    elif identifier == 8:
        fraction *= 0.08
    elif identifier == 9:
        fraction *= 0.5
    elif identifier == 10:
        fraction *= 0.4
    return str(round(fraction*100))+'%'


def midi_value(position, extent, inverted=False):
    fraction = max(0.0, min(1.0, position / max(1.0, extent - 1)))
    return round(127 * (1.0 - fraction if inverted else fraction))


class TouchFXState:
    def __init__(self, send):
        self.send = send
        self.channel = 0
        self.latch = False
        self.pressed = False
        self.active = False
        self.position = (64, 0)
        self.last_axes = None
        self.amount = 64

    def start(self):
        self.panic()
        self.send('control_change', self.channel, CC_DECK, 127)
        self.axes(*self.position, force=True)

    def axes(self, horizontal, vertical, force=False):
        position = tuple(max(0, min(127, int(value))) for value in (horizontal, vertical))
        for index, control in enumerate((CC_X, CC_Y)):
            if force or self.last_axes is None or position[index] != self.last_axes[index]:
                self.send('control_change', self.channel, control, position[index])
        self.position = position
        self.last_axes = position

    def press(self, horizontal, vertical):
        if self.pressed:
            return
        self.pressed = True
        self.axes(horizontal, vertical, force=True)
        self.send('note_on', self.channel, NOTE_GATE, 127)
        self.active = True

    def move(self, horizontal, vertical):
        if self.pressed:
            self.axes(horizontal, vertical)

    def off(self):
        self.send('note_off', self.channel, NOTE_GATE, 0)
        self.active = False

    def release(self):
        if not self.pressed:
            return
        self.pressed = False
        if not self.latch:
            self.off()

    def set_latch(self, enabled):
        self.latch = bool(enabled)
        if not self.latch and not self.pressed:
            self.off()

    def select_deck(self, deck):
        if deck not in (1, 2, 3, 4):
            raise ValueError('Deck must be 1..4')
        if deck - 1 == self.channel:
            return
        self.off()
        self.pressed = False
        self.channel = deck - 1
        self.last_axes = None
        self.send('control_change', self.channel, CC_DECK, 127)
        self.axes(*self.position, force=True)

    def heartbeat(self):
        self.send('control_change', self.channel, CC_HEARTBEAT, 127 if self.active else 0)

    def select_effect(self, index):
        if type(index) is not int or not 1 <= index <= 127:
            raise ValueError('Effect index must be 1..127')
        self.panic()
        self.axes(64, 0, force=True)
        self.send('control_change', self.channel, CC_EFFECT, index)

    def select_macro(self, macro, first, second):
        if type(macro) is not int or macro not in range(1, 12) or type(first) is not int or not 1 <= first <= 127 or type(second) is not int or not 0 <= second <= 127 or first == second:
            raise ValueError('Invalid macro or effect indices')
        if (macro in (5, 9, 10, 11)) != (second == 0):
            raise ValueError('Wrong slot count for macro')
        self.panic()
        self.amount = 64
        self.axes(64, 0, force=True)
        for control, value in ((16, macro), (20, first), (21, second), (22, macro)):
            self.send('control_change', self.channel, control, value)

    def set_amount(self, value):
        self.amount = max(0, min(127, int(value)))
        self.send('control_change', self.channel, 24, self.amount)

    def reset_fx(self):
        self.panic()
        self.latch = False
        self.amount = 64
        self.axes(64, 0, force=True)
        self.send('control_change', self.channel, 26, 127)

    def panic(self):
        self.pressed = False
        self.active = False
        for channel in range(4):
            self.send('note_off', channel, NOTE_GATE, 0)
