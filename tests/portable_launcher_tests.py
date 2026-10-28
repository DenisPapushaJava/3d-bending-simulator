"""Exercise the actual native bootstrap, without launching CAD or replacing user files."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

launcher, child = map(pathlib.Path, sys.argv[1:3])
old, new = '2026.1.0-alpha.1', '2026.1.0-alpha.2'
token = 'a' * 32

def until(condition):
    end = time.monotonic() + 6
    while not condition():
        if time.monotonic() > end:
            raise AssertionError('Process fixture timed out')
        time.sleep(.01)

def make(root):
    root.mkdir()
    (root / '.updates').mkdir()
    (root / 'portable-format.txt').write_text('1\n')
    (root / 'current.txt').write_text(old + '\n')
    (root / 'data').mkdir()
    (root / 'data/settings.ini').write_bytes(b'personal settings')
    (root / 'Part.bend').write_bytes(b'personal project')
    for version in (old, new):
        folder = root / 'runtime' / version
        (folder / 'bin').mkdir(parents=True)
        shutil.copy2(child, folder / 'bin/3d-bending-simulator.exe')
        (folder / '.ready').write_text(version + '\n')
        (folder / 'VERSION').write_text(version + '\n')
        (folder / 'private.step').write_bytes(b'personal CAD in old runtime')

def call(root, *args):
    return subprocess.run([str(launcher), '--root', str(root), *args], capture_output=True,
                          timeout=8, encoding='utf-8')

def activate(root, *extra):
    return ['--root', str(root), '--activate', new, '--from', old, '--token', token,
            '--wait-pid', '0', *extra]

def preserved(root):
    assert (root / 'data/settings.ini').read_bytes() == b'personal settings'
    assert (root / 'Part.bend').read_bytes() == b'personal project'
    assert (root / 'runtime' / old / 'private.step').read_bytes() == b'personal CAD in old runtime'

with tempfile.TemporaryDirectory(prefix='portable update ') as temp:
    root = pathlib.Path(temp) / 'Программа с пробелами'
    make(root)
    arg = 'Деталь с "кавычкой" и пробелом\\'
    result = call(root, '--version', arg)
    assert result.returncode == 0 and old in result.stdout, (result.stdout, result.stderr)
    assert (root / 'arguments.txt').read_text(encoding='utf-8').splitlines()[-1] == arg
    reopen = str(root / 'Деталь с пробелом.bend')
    result = subprocess.run([str(launcher), *activate(root, '--reopen', reopen)], timeout=8)
    assert result.returncode == 0
    assert (root / 'current.txt').read_text().strip() == new
    assert not (root / '.updates/pending.txt').exists()
    assert (root / 'arguments.txt').read_text(encoding='utf-8').splitlines()[-1] == reopen
    preserved(root)
    # Invalid paths never change the active version or write outside the installation.
    result = call(root, '--activate', '../../outside', '--from', new, '--token', token)
    assert result.returncode != 0 and (root / 'current.txt').read_text().strip() == new
    # An interrupted switch is recovered before loading any Qt/OCCT runtime.
    (root / '.updates/pending.txt').write_text(old + '\n' + new + '\n' + token + '\n')
    result = call(root, '--version')
    assert result.returncode == 0 and (root / 'current.txt').read_text().strip() == old
    preserved(root)
    # A confirmed start survives a helper interruption before journal cleanup.
    (root / 'current.txt').write_text(new + '\n')
    (root / '.updates/pending.txt').write_text(old + '\n' + new + '\n' + token + '\n')
    (root / '.updates' / ('ack-' + token)).write_text('ready\n')
    result = call(root, '--version')
    assert result.returncode == 0 and (root / 'current.txt').read_text().strip() == new
    # An incomplete candidate must not prevent recovery to the intact old runtime.
    (root / '.updates/pending.txt').write_text(old + '\n' + new + '\n' + token + '\n')
    (root / 'runtime' / new / '.ready').unlink()
    result = call(root, '--version')
    assert result.returncode == 0 and (root / 'current.txt').read_text().strip() == old
    (root / 'runtime' / new / '.ready').write_text(new + '\n')
    # Early startup failure automatically restores and restarts the old version.
    (root / 'runtime' / new / 'fixture-mode').write_text('fail')
    result = subprocess.run([str(launcher), *activate(root)], timeout=8)
    assert result.returncode != 0 and (root / 'current.txt').read_text().strip() == old
    preserved(root)
    (root / 'runtime' / new / 'fixture-mode').write_text('delay')
    # A second updater/launcher cannot act during the transaction.
    running = subprocess.Popen([str(launcher), *activate(root)])
    until(lambda: (root / '.updates/pending.txt').exists())
    assert call(root, '--version').returncode != 0
    assert running.wait(timeout=8) == 0
    # The parent is allowed to exit normally; no process is terminated by the updater.
    (root / 'current.txt').write_text(old + '\n')
    parent = subprocess.Popen([str(root / 'runtime' / old / 'bin/3d-bending-simulator.exe'), '--hold'], cwd=root)
    parameters = activate(root)
    parameters[parameters.index('--wait-pid') + 1] = str(parent.pid)
    running = subprocess.Popen([str(launcher), *parameters])
    time.sleep(.2)
    assert running.poll() is None and (root / 'current.txt').read_text().strip() == old
    (root / 'end-hold').write_text('done')
    assert parent.wait(timeout=4) == 0 and running.wait(timeout=8) == 0
    preserved(root)
print('Native bootstrap: activation, quoting, parent wait, rollback, recovery and lock passed')
