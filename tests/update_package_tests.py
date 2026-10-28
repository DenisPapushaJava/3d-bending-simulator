"""Actual package writer/extractor/validator against controlled hostile ZIP entries."""
import hashlib
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

validator = pathlib.Path(sys.argv[1])
extractor = pathlib.Path(sys.argv[2])
pwsh = sys.argv[3] if len(sys.argv) > 3 else ''
repository = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package_windows', repository/'scripts/package_windows.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)
version = '2026.1.0-alpha.2'
prefix = f'3d-bending-simulator-{version}-windows-x86_64'

def installed(root):
    root.mkdir()
    (root/'portable-format.txt').write_text('1\n')
    (root/'current.txt').write_text('2026.1.0-alpha.1\n')
    (root/'data').mkdir()
    (root/'data/settings.ini').write_bytes(b'personal')
    (root/'Part.bend').write_bytes(b'project')

def preserved(root):
    assert (root/'data/settings.ini').read_bytes() == b'personal'
    assert (root/'Part.bend').read_bytes() == b'project'
    assert (root/'current.txt').read_text().strip() == '2026.1.0-alpha.1'

with tempfile.TemporaryDirectory(prefix='package verification ') as folder:
    base = pathlib.Path(folder)
    source = base/'distribution'
    (source/'bin').mkdir(parents=True)
    (source/'share').mkdir()
    executable = bytearray(128)
    executable[0:2] = b'MZ'; executable[60:64] = (64).to_bytes(4,'little')
    executable[64:70] = b'PE\x00\x00\x64\x86'
    (source/'bin/3d-bending-simulator.exe').write_bytes(executable)
    (source/'share/notice.txt').write_text('MIT test fixture')
    bootstrap = base/'bootstrap.exe'; bootstrap.write_bytes(b'fixture, never executed')
    package.configure_portable(source, version, bootstrap)
    runtime = source/'runtime'/version
    expected = json.loads((runtime/'runtime-manifest.json').read_text())
    assert all(hashlib.sha256((runtime/f['path']).read_bytes()).hexdigest() == f['sha256'] for f in expected['files'])
    root = base/'установка'; installed(root)
    def stage(archive=source, target=root, extra=()):
        return subprocess.run([str(validator), str(archive), str(target), 'stage', *extra], capture_output=True, timeout=8)
    cancelled = base/'cancelled'; installed(cancelled)
    assert stage(target=cancelled, extra=('--cancel',)).returncode != 0
    assert not (cancelled/'runtime'/version).exists(); preserved(cancelled)
    assert stage().returncode == 0
    assert (root/'runtime'/version/'.ready').read_text().strip() == version
    (root/'current.txt').write_text(version+'\n')
    assert subprocess.run([str(validator), str(source), str(root), 'discover'], timeout=8).returncode == 0
    (root/'current.txt').write_text('2026.1.0-alpha.1\n')
    # Reuse a verified staged version without modifying private files or running binaries.
    (root/'runtime'/version/'private.step').write_bytes(b'user file')
    assert stage().returncode == 0
    assert (root/'runtime'/version/'private.step').read_bytes() == b'user file'
    preserved(root)
    for name, mutate in [
        ('corrupt', lambda d: (d/'runtime'/version/'bin/3d-bending-simulator.exe').write_bytes(b'changed')),
        ('extra', lambda d: (d/'runtime'/version/'bin/unlisted.dll').write_bytes(b'extra')),
        ('foreign', lambda d: (d/'runtime'/version/'VERSION').write_text('2027.1.0')),
    ]:
        bad = base/name; shutil.copytree(source,bad)
        mutate(bad)
        target = base/(name+' install'); installed(target)
        assert stage(bad, target).returncode != 0
        preserved(target)
    bad = base/'traversal'; shutil.copytree(source,bad)
    manifest_path=bad/'runtime'/version/'runtime-manifest.json'
    manifest=json.loads(manifest_path.read_text());manifest['files'][0]['path']='../../data/settings.ini'
    manifest_path.write_text(json.dumps(manifest))
    assert stage(bad).returncode != 0; preserved(root)
    # Probes can create local settings/locks; the actual ZIP writer must exclude them.
    bundle = base/prefix
    shutil.copytree(source, bundle)
    (bundle/'data').mkdir(); (bundle/'data/settings.ini').write_bytes(b'private probe settings')
    (bundle/'.updates').mkdir(); (bundle/'.updates/install.lock').write_bytes(b'probe lock')
    archive = base/'valid.zip'
    package.archive_tree(archive, bundle, excluded_roots={'data', '.updates'})
    with zipfile.ZipFile(archive) as z:
        assert not any(path.startswith((prefix+'/data/', prefix+'/.updates/')) for path in z.namelist())
    # Exercise the same System.IO.Compression extraction used on Windows.
    if pwsh:
        out=base/'unpacked'
        def extract(path, dest):
            return subprocess.run([pwsh,'-NoProfile','-NonInteractive','-File',str(extractor),'-Archive',str(path),'-Destination',str(dest),'-Prefix',prefix],capture_output=True,timeout=15)
        result=extract(archive,out)
        assert result.returncode==0, result.stderr.decode(errors='replace')
        target=base/'extracted install';installed(target)
        assert stage(out,target).returncode==0;preserved(target)
        for index,path in enumerate(['../escape.txt','runtime/'+version+'/bin/COM1.txt','bin\\evil.dll','/absolute.txt']):
            badzip=base/f'hostile{index}.zip'
            with zipfile.ZipFile(badzip,'w') as z:z.writestr(prefix+'/'+path,b'bad')
            assert extract(badzip,base/f'hostile{index}').returncode !=0
        for kind in ['duplicate','symlink']:
            badzip=base/(kind+'.zip')
            with zipfile.ZipFile(badzip,'w') as z:
                if kind=='duplicate': z.writestr(prefix+'/bin/A.dll',b'a');z.writestr(prefix+'/bin/a.dll',b'b')
                else:
                    info=zipfile.ZipInfo(prefix+'/bin/link');info.create_system=3;info.external_attr=0o120777<<16
                    z.writestr(info,'../../outside')
            assert extract(badzip,base/kind).returncode !=0
        assert not (base/'escape.txt').exists()
print('Package hashes, paths, architecture, cancellation and data preservation passed; ZIP extractor exercised:', bool(pwsh))
