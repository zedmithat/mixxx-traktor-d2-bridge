"""Install a separate Touch FX skin/runtime into a staged rootfs, never the live root."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import xml.etree.ElementTree as ElementTree

BASE = Path(__file__).resolve().parents[1]
RECEIPT = 'opt/zed/touch-fx-integration.json'
USER = 'home/pi/'
CFG = USER+'.mixxx/mixxx.cfg'
SWAY = ('etc/zed/sway-config', USER+'.config/sway/config')
RUNTIME = 'usr/local/lib/zed-touch-fx/'
CONTROLLERS = USER+'.mixxx/controllers/'
UNITS = USER+'.config/systemd/user/'
SKIN = 'usr/share/mixxx/skins/zed/'
VARIANT = 'usr/share/mixxx/skins/zed-touchfx/'
D2_SHA256 = 'b82e96670f6b59de60cf837265e1e048dfcaf1c69de8f3447331d35197734ee3'
SOURCES = ('touchfx.py', 'touchfx_core.py', 'zed_session.py', 'TouchFX_Virtual.midi.xml',
    'TouchFX-scripts.js', 'TouchFX-macros.js', 'integration/zed-extension.js', 'integration/zed-touch-fx.service',
    'integration/50-touch-fx.conf', 'integration/install_rootfs.py')
SOURCES += ('integration/effect_factory.py',)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe(root, relative):
    parts = Path(relative).parts
    if Path(relative).is_absolute() or Path(relative).drive or '..' in parts:
        raise ValueError('Unsafe relative path')
    path = root
    for index, part in enumerate(parts):
        path = path/part
        if path.is_symlink():
            raise ValueError('Linked installation path: '+relative)
        if index < len(parts)-1 and path.exists() and not path.is_dir():
            raise ValueError('Non-directory installation parent: '+relative)
    return path


def read(root, relative):
    path = safe(root, relative)
    if not path.is_file():
        raise ValueError('Required regular file missing: '+relative)
    return path.read_bytes()


def setting(text, section, key, value):
    header = '['+section+']'
    lines = text.splitlines()
    starts = [index for index, line in enumerate(lines) if line.strip() == header]
    if len(starts) != 1:
        raise ValueError('Expected one config section: '+section)
    start = starts[0]+1
    end = next((index for index in range(start, len(lines)) if lines[index].startswith('[')), len(lines))
    found = [index for index in range(start, end) if re.match(r'^'+re.escape(key)+r'\s', lines[index])]
    if len(found) > 1:
        raise ValueError('Duplicate config key: '+key)
    if found:
        lines[found[0]] = key+' '+value
    else:
        lines.insert(end, key+' '+value)
    return '\n'.join(lines)+'\n'


def xml_bytes(root):
    return ElementTree.tostring(root, encoding='utf-8', xml_declaration=True)+b'\n'


def plan(root):
    identity = root/'etc/os-release'
    if identity.is_symlink():
        if os.readlink(identity) not in ('../usr/lib/os-release', '/usr/lib/os-release'):
            raise ValueError('Unexpected OS identity link')
        read(root, 'usr/lib/os-release')
    else:
        read(root, 'etc/os-release')
    read(root, UNITS+'mixxx-d2.service')
    d2 = read(root, CONTROLLERS+'Traktor-Kontrol-D2-scripts.js')
    if digest(d2) != D2_SHA256:
        raise ValueError('Unreviewed D2 FX ownership; expected two-surface mapping')
    files, preserved = {}, {CONTROLLERS+'Traktor-Kontrol-D2-scripts.js': digest(d2)}
    original_cfg = read(root, CFG)
    config = original_cfg.decode('utf-8')
    if not re.search(r'^ResizableSkin zed\s*$', config, re.MULTILINE):
        raise ValueError('Expected fresh ZED profile configuration')
    config = setting(config, 'Config', 'ResizableSkin', 'zed-touchfx')
    config = setting(config, 'Controller', 'TouchFX_Virtual', '1')
    config = setting(config, 'ControllerPreset', 'TouchFX_Virtual', 'TouchFX-ZED.midi.xml')
    files[CFG] = config.encode()
    files['opt/zed/touch-fx-backup/mixxx.cfg'] = original_cfg
    source_skin = safe(root, SKIN)
    for path in source_skin.rglob('*'):
        relative = path.relative_to(root).as_posix()
        safe(root, relative)
        if path.is_dir():
            continue
        data = read(root, relative)
        preserved[relative] = digest(data)
        files[VARIANT+path.relative_to(source_skin).as_posix()] = data
    skin = ElementTree.fromstring(read(root, SKIN+'skin.xml'))
    attributes = skin.find('./manifest/attributes')
    if attributes is None or attributes.find("attribute[@config_key='[Skin],touch_fx']") is not None:
        raise ValueError('Unexpected skin attributes')
    ElementTree.SubElement(attributes, 'attribute', {'config_key': '[Skin],touch_fx'}).text = '0'
    skin.find('./manifest/title').text = 'ZED Touch FX'
    files[VARIANT+'skin.xml'] = xml_bytes(skin)
    topbar = ElementTree.fromstring(read(root, SKIN+'topbar.xml'))
    matches = [(parent, child) for parent in topbar.iter() for child in parent
               if child.findtext('ObjectName') == 'ZedWifiButton']
    if len(matches) != 1:
        raise ValueError('Expected one existing Wi-Fi button anchor')
    parent, anchor = matches[0]
    button = ElementTree.fromstring('''<PushButton>
        <ObjectName>ZedTouchFXButton</ObjectName><Size>72f,60f</Size><NumberStates>2</NumberStates>
        <State><Number>0</Number><Text>TOUCH\nFX</Text></State>
        <State><Number>1</Number><Text>TOUCH\nFX</Text></State>
        <Connection><ConfigKey>[Skin],touch_fx</ConfigKey></Connection>
    </PushButton>''')
    parent.insert(list(parent).index(anchor), button)
    files[VARIANT+'topbar.xml'] = xml_bytes(topbar)
    for relative in SWAY:
        data = read(root, relative)
        files['opt/zed/touch-fx-backup/'+relative.replace('/', '_')] = data
        files[relative] = data.rstrip()+b'\nfor_window [app_id="org.zed.TouchFX" title="^ZED Touch FX$"] fullscreen enable global\nfor_window [app_id="org.zed.TouchFX" title="^Touch FX .*Makrolar$"] floating enable, move position center\n'
    for name in ('touchfx.py', 'touchfx_core.py', 'zed_session.py'):
        files[RUNTIME+name] = read(BASE, name)
    controller = ElementTree.fromstring(read(BASE, 'TouchFX_Virtual.midi.xml'))
    controller.find('./info/name').text = 'ZED Touch FX — Unit 3'
    controller.find('./info/description').text = 'ZED two-deck Touch FX on dedicated Effect Unit 3; D2 units 1/2 preserved.'
    controller.find('./controller/scriptfiles/file').set('filename', 'TouchFX-ZED-scripts.js')
    for channel in range(2):
        control = ElementTree.SubElement(controller.find('./controller/controls'), 'control')
        for tag, value in (('group', '[EffectRack1_EffectUnit3]'),
                ('key', 'TouchFX.selectEffect'), ('status', hex(0xB0+channel)), ('midino', '0x0D')):
            ElementTree.SubElement(control, tag).text = value
        ElementTree.SubElement(ElementTree.SubElement(control, 'options'), 'script-binding')
    for group in controller.findall('./controller/controls/control/group'):
        group.text = '[EffectRack1_EffectUnit3]'
    files[CONTROLLERS+'TouchFX-ZED.midi.xml'] = xml_bytes(controller)
    files[CONTROLLERS+'TouchFX-ZED-scripts.js'] = (read(BASE, 'TouchFX-scripts.js').rstrip()+b'\n\n'
        +read(BASE, 'integration/zed-extension.js'))
    files[CONTROLLERS+'TouchFX-macros.js'] = read(BASE, 'TouchFX-macros.js')
    files[UNITS+'zed-touch-fx.service'] = read(BASE, 'integration/zed-touch-fx.service')
    files[UNITS+'mixxx-d2.service.d/50-touch-fx.conf'] = read(BASE, 'integration/50-touch-fx.conf')
    flavor = safe(root, 'etc/zed/image-flavor')
    if flavor.is_file() and flavor.read_bytes().strip() == b'public' and not safe(root, USER+'.mixxx/effects.xml').exists():
        factory_spec = importlib.util.spec_from_file_location('effect_factory', BASE/'integration/effect_factory.py')
        factory = importlib.util.module_from_spec(factory_spec)
        factory_spec.loader.exec_module(factory)
        for relative, data in factory.factory_files().items():
            if safe(root, relative).exists():
                raise ValueError('Partial existing effect defaults require review: '+relative)
            files[relative] = data
    for relative in files:
        path = safe(root, relative)
        if path.exists() and relative not in (CFG, *SWAY):
            raise ValueError('Refusing existing Touch FX destination: '+relative)
    return files, preserved


def install(root):
    absolute = root.absolute()
    if absolute != root.resolve() or absolute.parent == absolute or absolute.name != 'rootfs':
        raise ValueError('Expected a non-linked, separate directory named rootfs')
    root = absolute
    receipt_path = safe(root, RECEIPT)
    sources = {relative: digest(read(BASE, relative)) for relative in SOURCES}
    if receipt_path.exists():
        receipt = json.loads(read(root, RECEIPT))
        if receipt.get('source_sha256') != sources:
            raise ValueError('Touch FX sources changed; prepare a fresh staged rootfs')
        for collection in ('installed_sha256', 'preserved_sha256'):
            for relative, expected in receipt[collection].items():
                if digest(read(root, relative)) != expected:
                    raise ValueError('Integration drift: '+relative)
        print('Touch FX integration already present and verified')
        return receipt
    files, preserved = plan(root)
    for relative, data in files.items():
        path = safe(root, relative)
        missing = []
        parent = path.parent
        while not parent.exists():
            missing.append(parent)
            parent = parent.parent
        path.parent.mkdir(parents=True, exist_ok=True)
        for directory in missing:
            directory.chmod(0o755)
        path.write_bytes(data)
        path.chmod(0o644)
        if getattr(os, 'geteuid', lambda: -1)() == 0 and relative.startswith(USER):
            for owned in (*missing, path):
                os.chown(owned, 1000, 1000)
    receipt = {'schema': 1, 'effect_unit': 3, 'decks': [1, 2],
        'installed_sha256': {relative: digest(data) for relative, data in files.items()},
        'preserved_sha256': preserved, 'source_sha256': sources,
        'hardware_verified': False, 'public_release_ready': False}
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    receipt_path.write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
    receipt_path.chmod(0o644)
    print('Touch FX integrated into staged rootfs: '+str(len(files))+' files; no hardware acceptance')
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs', type=Path, required=True)
    args = parser.parse_args()
    try:
        install(args.rootfs)
    except (OSError, ValueError, KeyError, ElementTree.ParseError) as error:
        parser.exit(1, 'Touch FX integration stopped: '+str(error)+'\n')
