"""Create an allowlisted Touch FX source overlay, not a bootable or live-install image."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[2]
PREFIX = 'touchfx-source-overlay'
FILES = (
    'LICENSE',
    'COPYING.ZED',
    'ZED-LICENSING.md',
    'touch-fx/LICENSE',
    'touch-fx/LICENSING.md',
    'touch-fx/.gitignore',
    'touch-fx/README.md',
    'touch-fx/MACROS.md',
    'touch-fx/requirements.txt',
    'touch-fx/touchfx.py',
    'touch-fx/touchfx_core.py',
    'touch-fx/zed_session.py',
    'touch-fx/TouchFX_Virtual.midi.xml',
    'touch-fx/TouchFX-scripts.js',
    'touch-fx/TouchFX-macros.js',
    'touch-fx/integration/README.md',
    'touch-fx/integration/install_rootfs.py',
    'touch-fx/integration/effect_factory.py',
    'touch-fx/integration/package_sources.py',
    'touch-fx/integration/zed-extension.js',
    'touch-fx/integration/zed-touch-fx.service',
    'touch-fx/integration/50-touch-fx.conf',
    'touch-fx/tests/qt_test_support.py',
    'touch-fx/tests/render_gui.py',
    'touch-fx/tests/review_zed_staging.py',
    'touch-fx/tests/test_gui.py',
    'touch-fx/tests/test_mapping.cjs',
    'touch-fx/tests/test_macros.cjs',
    'touch-fx/tests/test_protocol_pipeline.py',
    'touch-fx/tests/test_touchfx.py',
    'touch-fx/tests/test_zed_install.py',
    'touch-fx/tests/test_zed_session.py',
    'touch-fx/tests/test_effect_factory.py',
    'touch-fx/tests/test_package_sources.py',
    'stage-zed/20-touch-fx/00-packages-nr',
    'stage-zed/20-touch-fx/00-run.sh',
    'tools/test-touchfx-wayland-menu.py',
)


def source_bytes(root, relative):
    path = root
    for part in Path(relative).parts:
        path = path/part
        if path.is_symlink():
            raise ValueError('Linked source refused: '+relative)
    if not path.is_file():
        raise ValueError('Required source missing: '+relative)
    data = path.read_bytes()
    if relative.endswith('.sh') and b'\r' in data:
        raise ValueError('Shell source requires LF line endings: '+relative)
    return data


def package(output, root=ROOT):
    entries = {relative: source_bytes(root, relative) for relative in FILES}
    manifest = {
        'schema': 1,
        'kind': 'source-overlay-not-installable-image',
        'original_code_license': 'GPL-3.0-or-later',
        'license_scope': 'Original ZED and Touch FX contributions only; upstream licenses unchanged',
        'hardware_verified': False,
        'public_release_ready': False,
        'requires': 'Existing reviewed ZED pi-gen checkout for integrated image builds',
        'files_sha256': {relative: hashlib.sha256(data).hexdigest()
                         for relative, data in entries.items()},
    }
    entries['SOURCE-MANIFEST.json'] = (json.dumps(manifest, indent=2)+'\n').encode('utf-8')
    with output.open('xb') as destination:
        with gzip.GzipFile(filename='', mode='wb', fileobj=destination, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w') as archive:
                for relative, data in sorted(entries.items()):
                    member = tarfile.TarInfo(PREFIX+'/'+relative)
                    member.size = len(data)
                    member.mode = 0o755 if relative.endswith('.sh') else 0o644
                    archive.addfile(member, io.BytesIO(data))
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = package(args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, 'Source packaging stopped: '+str(error)+'\n')
    print('Packaged '+str(len(result['files_sha256']))+' source files: '+str(args.output))
    print('SHA256 '+hashlib.sha256(args.output.read_bytes()).hexdigest())
