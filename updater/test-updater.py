#!/usr/bin/env python3
"""Exercise the real signed updater against a disposable runtime-only bundle."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--updater', required=True, type=Path)
parser.add_argument('--runtime', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
updater, runtime = args.updater.resolve(), args.runtime.resolve()
assert updater.name == 'shadPS4-update.app'
assert runtime.name == 'shadPS4-lbp3.app'
payload = updater / 'Contents/Resources/Runtime'
executable = updater / 'Contents/MacOS/shadps4-runtime-updater'
items = {name: 'Contents/MacOS/' + name for name in (
    'shadps4', 'shadps4-core', 'partychat', 'libvulkan.dylib',
    'libvulkan_kosmickrisp.dylib', 'kosmickrisp_mesa_icd.json')}
items.update({'Info.plist': 'Contents/Info.plist'})
items.update({name: 'Contents/Resources/' + name for name in (
    'LICENSE-shadPS4.txt', 'LICENSE-PartyChat.txt', 'BuildInfo.json')})


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify(bundle):
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(bundle)], check=True)


verify(updater)
hashes = {}
for name, relative in items.items():
    hashes[name] = digest(payload / name)
    assert hashes[name] == digest(runtime / relative), name
logs = []
with tempfile.TemporaryDirectory(prefix='shadps4-updater-validation-') as directory:
    target = Path(directory) / 'shadPS4-lbp3.app'
    for name, relative in items.items():
        if name == 'shadps4-core':
            continue  # Exercise installation of a missing core without running it.
        path = target / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['ditto', str(payload / name), str(path)], check=True)
    (target / items['BuildInfo.json']).write_text('{"previous_runtime": true}\n')
    icd = target / items['kosmickrisp_mesa_icd.json']
    previous_icd = json.loads(icd.read_text())
    previous_icd['updater_fixture_previous_runtime'] = True
    icd.write_text(json.dumps(previous_icd) + '\n')
    subprocess.run(['codesign', '--force', '--sign', '-', str(icd)], check=True)
    protected = [target / 'Contents/Resources' / relative for relative in (
        'Game/CUSA00063/test-preserved', 'Addons/CUSA00063/test-preserved',
        'dry.db', 'home/savedata/test-preserved')]
    for path in protected:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'preserve this local game/DLC/save content\n')
    before = {str(path): digest(path) for path in protected}
    subprocess.run(['codesign', '--force', '--sign', '-', str(target)], check=True)
    verify(target)

    result = subprocess.run([str(executable), '--target', str(target)],
                            capture_output=True, text=True, timeout=120)
    logs.append(result.stdout + result.stderr)
    assert result.returncode == 0, logs[-1]
    assert 'Runtime updated (3 files)' in result.stdout, logs[-1]
    verify(target)
    for name, relative in items.items():
        if name != 'shadps4':  # The enclosing bundle seal belongs to the launcher.
            assert digest(target / relative) == hashes[name], name
    for path in protected:
        assert digest(path) == before[str(path)], path
    assert not list(target.rglob('*.update-*'))
    assert not list(target.rglob('*.rollback-*'))

    snapshot = {relative: digest(target / relative) for relative in items.values()}
    result = subprocess.run([str(executable), '--target', str(target)],
                            capture_output=True, text=True, timeout=120)
    logs.append(result.stdout + result.stderr)
    assert result.returncode == 0 and 'already current' in result.stdout, logs[-1]
    assert snapshot == {relative: digest(target / relative) for relative in items.values()}
    verify(target)

args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps({'passed': True, 'updater': str(updater),
    'payload_byte_identical_to_runtime': hashes, 'output': logs,
    'checks': ['signed payload', 'missing core installed', 'BuildInfo updated',
               'ICD updated with its extended-attribute signature',
               'game/DLC/dry.db/save preserved', 'full target signature',
               'second update changes nothing', 'temporary staging removed'],
    'game_launched': False}, indent=2) + '\n')
print('PASS: signed runtime update, preserved game/DLC/saves, exact payload, idempotent second update')
