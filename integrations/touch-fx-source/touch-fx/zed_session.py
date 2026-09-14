"""ZED session helpers: no shell commands, audio access, or D2 MIDI forwarding."""
import os
from pathlib import Path
import socket
import time
import xml.etree.ElementTree as ElementTree


EFFECT_NAMES = {
    'filter': 'Filter', 'echo': 'Echo', 'reverb': 'Reverb',
    'flanger': 'Flanger', 'phaser': 'Phaser', 'bitcrusher': 'Bitcrusher',
    'autopan': 'Autopan', 'tremolo': 'Tremolo', 'glitch': 'Glitch', 'whitenoise': 'White Noise',
}

MACROS = {
    1: ('Filter Echo', 'Filter', 'Echo'), 2: ('Reverb + Flanger', 'Reverb', 'Flanger'),
    3: ('Filter Reverb', 'Filter', 'Reverb'), 4: ('Filter Roll · Glitch', 'Filter', 'Glitch'),
    5: ('LFO Echo', 'Echo', None), 6: ('Filter Dub Echo', 'Filter', 'Echo'),
    7: ('Filter Gate · Tremolo', 'Filter', 'Tremolo'), 8: ('Noise Gate · Tremolo', 'White Noise', 'Tremolo'),
    9: ('Flanger', 'Flanger', None), 10: ('LFO Filter', 'Filter', None), 11: ('Filter', 'Filter', None),
}

PRESET_PARAMETERS = {
    'filter': ('lpf', 'q', 'hpf'),
    'echo': ('delay_time', 'feedback_amount', 'pingpong_amount', 'send_amount', 'quantize', 'triplet'),
    'reverb': ('decay', 'bandwidth', 'damping', 'send_amount'),
    'flanger': ('speed', 'width', 'manual', 'regen', 'mix', 'triplet'),
    'glitch': ('delay_time', 'quantize', 'triplet'),
    'tremolo': ('depth', 'rate', 'width', 'waveform', 'phase', 'quantize', 'triplet'),
    'whitenoise': ('dry_wet',),
}


def compatible_preset(identifier, directory=None):
    directory = Path(directory) if directory is not None else Path.home()/'.mixxx/effects/defaults'
    try:
        root = ElementTree.parse(directory/('org-mixxx-effects-'+identifier+'.xml')).getroot()
    except (OSError, ElementTree.ParseError):
        return False
    parameters = root.findall('./Parameters/Parameter')
    if (root.findtext('Id') != 'org.mixxx.effects.'+identifier
            or root.findtext('BackendType') != 'Built-In'
            or tuple(item.findtext('Id') for item in parameters) != PRESET_PARAMETERS[identifier]
            or any(item.findtext('Hidden') != '0' for item in parameters)):
        return False
    if identifier == 'filter':
        return (parameters[0].findtext('LinkType') == 'LINKED_LEFT'
            and parameters[2].findtext('LinkType') == 'LINKED_RIGHT'
            and parameters[0].findtext('LinkInversion') == '0'
            and parameters[2].findtext('LinkInversion') == '0')
    return True


def macro_choices(choices=None):
    catalog = {label: index for index, label in (effect_choices(include_generators=True) if choices is None else choices)}
    if choices is None:
        catalog = {label: index for label, index in catalog.items()
            if any(name == label and identifier in PRESET_PARAMETERS and compatible_preset(identifier)
                for identifier, name in EFFECT_NAMES.items())}
    return [(identifier, label, catalog[first], catalog[second] if second else 0)
        for identifier, (label, first, second) in MACROS.items()
        if first in catalog and (second is None or second in catalog)]


def effect_choices(path=None, include_generators=False):
    path = Path(path) if path is not None else Path.home()/'.mixxx/effects.xml'
    try:
        visible = ElementTree.parse(path).getroot().find('VisibleEffects')
    except (OSError, ElementTree.ParseError):
        return []
    if visible is None:
        return []
    effects = visible.findall('Effect')
    identifiers = [effect.findtext('Id', '') for effect in effects]
    if len(identifiers) != len(set(identifiers)) or any(
            effect.findtext('BackendType') != 'Built-In' for effect in effects):
        return []
    choices = []
    for identifier, label in EFFECT_NAMES.items():
        if identifier == 'whitenoise' and not include_generators:
            continue
        full_id = 'org.mixxx.effects.'+identifier
        if full_id in identifiers:
            index = identifiers.index(full_id)+1
            if index <= 127:
                choices.append((index, label))
    return choices


def prepare_wayland(timeout=10):
    runtime = os.environ.get('XDG_RUNTIME_DIR')
    if not runtime or not Path(runtime).is_absolute():
        raise RuntimeError('ZED Touch FX requires an absolute XDG_RUNTIME_DIR')
    display = os.environ.get('WAYLAND_DISPLAY')
    names = (display,) if display else ('wayland-0', 'wayland-1')
    deadline = time.monotonic()+timeout
    while True:
        available = [name for name in names if (Path(runtime)/name).is_socket()]
        if len(available) > 1:
            raise RuntimeError('Multiple Wayland displays; set WAYLAND_DISPLAY explicitly')
        if available:
            os.environ['WAYLAND_DISPLAY'] = available[0]
            return available[0]
        if time.monotonic() >= deadline:
            raise RuntimeError('Wayland socket not ready before Touch FX startup timeout')
        time.sleep(0.1)


def is_open_request(message):
    return (message.type == 'control_change' and message.channel == 0
            and message.control == 120 and message.value == 127)


def notify_ready():
    address = os.environ.get('NOTIFY_SOCKET')
    if address:
        if address.startswith('@'):
            address = '\0'+address[1:]
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as connection:
            connection.connect(address)
            connection.sendall(b'READY=1')
