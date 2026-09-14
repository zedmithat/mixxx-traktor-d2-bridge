"""Stage reviewed public component assets in a temporary root; never use a device image."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

BASE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('installer', BASE/'integration/install_rootfs.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    candidate = BASE.parent/'vendor/public-candidates/public-components-20260908-a'
    manifest = json.loads((candidate/'manifest.json').read_text())
    identities = manifest['candidate_files_sha256']
    args.output.mkdir(parents=True, exist_ok=False)
    source_hashes = {}
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)/'rootfs'
        def put(relative, data):
            path = root/relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        def verified(relative):
            data = installer.read(candidate/'components', relative)
            actual = installer.digest(data)
            if actual != identities[relative]:
                raise ValueError('Component drift: '+relative)
            source_hashes[relative] = actual
            return data
        put('etc/os-release', b'ID=debian\n')
        stage = (BASE.parent/'stage-zed/10-zed-profiles/01-run-chroot.sh').read_text()
        config = stage.split("<<'EOF'\n", 1)[1].split('\nEOF', 1)[0]+'\n'
        put(installer.CFG, config.encode())
        put(installer.CONTROLLERS+'Traktor-Kontrol-D2-scripts.js',
            verified('mixxx-controller/Traktor-Kontrol-D2-scripts.js'))
        put(installer.UNITS+'mixxx-d2.service', verified('systemd/mixxx-d2.service'))
        for relative in installer.SWAY:
            put(relative, verified('profiles/d2-xone96/sway-config'))
        for relative in identities:
            if relative.startswith('skin/zed/'):
                put('usr/share/mixxx/skins/zed/'+relative.removeprefix('skin/zed/'), verified(relative))
        receipt = installer.install(root)
        if receipt != installer.install(root):
            raise ValueError('Repeated install changed receipt')
        for relative, expected in receipt['installed_sha256'].items():
            if installer.digest(installer.read(root, relative)) != expected:
                raise ValueError('Installed file drift: '+relative)
        for relative in (installer.VARIANT+'skin.xml', installer.VARIANT+'topbar.xml',
                installer.CONTROLLERS+'TouchFX-ZED.midi.xml', installer.CONTROLLERS+'TouchFX-ZED-scripts.js',
                installer.RECEIPT):
            shutil.copyfile(root/relative, args.output/Path(relative).name)
        node = shutil.which('node')
        if not node:
            raise RuntimeError('Node required to validate the generated mapping syntax')
        subprocess.run([node, '--check', str(args.output/'TouchFX-ZED-scripts.js')], check=True)
        report = {'source_candidate': candidate.name, 'source_files_verified': len(source_hashes),
            'source_sha256': source_hashes, 'installed_files_verified': len(receipt['installed_sha256']),
            'original_skin_and_d2_preserved': True, 'repeat_install_verified': True,
            'generated_mapping_syntax_verified': True, 'image_built': False, 'hardware_verified': False}
        (args.output/'review.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
        print(json.dumps({key: value for key, value in report.items() if key != 'source_sha256'}, indent=2))


if __name__ == '__main__':
    main()
