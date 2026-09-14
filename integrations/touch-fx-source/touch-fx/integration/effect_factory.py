"""Deterministic Mixxx 2.5.6 catalog and reviewed built-in parameter defaults."""
import xml.etree.ElementTree as ElementTree


VISIBLE_IDS = (
    'whitenoise', 'tremolo', 'reverb', 'pitchshift', 'phaser', 'parametriceq',
    'moogladder4filter', 'metronome', 'loudnesscontour', 'linkwitzrileyeq',
    'graphiceq', 'glitch', 'flanger', 'filter', 'echo', 'distortion',
    'compressor', 'bitcrusher', 'bessel8lvmixeq', 'bessel4lvmixeq',
    'balance', 'biquadfullkilleq', 'threebandbiquadeq', 'autopan',
)
DEFAULTS = {
    'filter': (('lpf', '22050', 'LINKED_LEFT'), ('q', '0.707107', 'NONE'), ('hpf', '13', 'LINKED_RIGHT')),
    'echo': (('delay_time', '0.5', 'NONE'), ('feedback_amount', '0.707946', 'NONE'),
             ('pingpong_amount', '0', 'NONE'), ('send_amount', '0.707946', 'LINKED'),
             ('quantize', '1', 'NONE'), ('triplet', '0', 'NONE')),
    'reverb': (('decay', '0.5', 'NONE'), ('bandwidth', '1', 'NONE'),
               ('damping', '0', 'NONE'), ('send_amount', '0', 'LINKED')),
    'flanger': (('speed', '8', 'NONE'), ('width', '6.39', 'NONE'), ('manual', '6.61', 'NONE'),
                ('regen', '0.25', 'NONE'), ('mix', '1', 'LINKED'), ('triplet', '0', 'NONE')),
    'glitch': (('delay_time', '0.5', 'NONE'), ('quantize', '1', 'NONE'), ('triplet', '0', 'NONE')),
    'tremolo': (('depth', '1', 'LINKED'), ('rate', '1', 'NONE'), ('width', '0.5', 'NONE'),
                ('waveform', '0.5', 'NONE'), ('phase', '0', 'NONE'), ('quantize', '1', 'NONE'), ('triplet', '0', 'NONE')),
    'whitenoise': (('dry_wet', '1', 'LINKED'),),
}


def xml_bytes(root):
    ElementTree.indent(root, space=' ')
    return ElementTree.tostring(root, encoding='utf-8', xml_declaration=True) + b'\n'


def factory_files():
    root = ElementTree.Element('MixxxEffects')
    visible = ElementTree.SubElement(root, 'VisibleEffects')
    for identifier in VISIBLE_IDS:
        effect = ElementTree.SubElement(visible, 'Effect')
        ElementTree.SubElement(effect, 'Id').text = 'org.mixxx.effects.' + identifier
        ElementTree.SubElement(effect, 'BackendType').text = 'Built-In'
    ElementTree.SubElement(root, 'HiddenEffects')
    files = {'home/pi/.mixxx/effects.xml': xml_bytes(root)}
    for identifier, definitions in DEFAULTS.items():
        effect = ElementTree.Element('Effect')
        ElementTree.SubElement(effect, 'MetaParameterValue').text = '0.5'
        ElementTree.SubElement(effect, 'Id').text = 'org.mixxx.effects.' + identifier
        ElementTree.SubElement(effect, 'BackendType').text = 'Built-In'
        parameters = ElementTree.SubElement(effect, 'Parameters')
        for name, value, link in definitions:
            parameter = ElementTree.SubElement(parameters, 'Parameter')
            for tag, text in (('Id', name), ('Value', value), ('LinkType', link), ('LinkInversion', '0'), ('Hidden', '0')):
                ElementTree.SubElement(parameter, tag).text = text
        files['home/pi/.mixxx/effects/defaults/org-mixxx-effects-' + identifier + '.xml'] = xml_bytes(effect)
    return files
