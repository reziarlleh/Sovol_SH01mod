"""Package tracked sources and pinned, accepted artifacts; never include device dumps."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args])


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def archive(path, entries, timestamp):
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for name, data in sorted(entries):
            info = zipfile.ZipInfo(name, timestamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            output.writestr(info, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--owner-confirmed-on', required=True, help='Explicit owner acceptance YYYY-MM-DD')
    parser.add_argument('--ci-url', required=True)
    args = parser.parse_args()
    datetime.strptime(args.owner_confirmed_on, '%Y-%m-%d')
    if git('status', '--porcelain').strip():
        raise ValueError('Commit source/doc/tool changes before packaging')
    version = re.search(r'#define FW_VERSION "([0-9.]+)"', (ROOT / 'src/config.h').read_text())[1]
    stem = 'sovol_sh01mod_v' + version.replace('.', '_')
    build = ROOT / 'build' / ('v' + version)
    out = ROOT / 'build/release' / ('v' + version)
    out.mkdir(parents=True, exist_ok=True)
    document_names = ['USER_GUIDE_RU.md', 'FLASHING_RU.md', 'TOOLS_RU.md', 'MODIFICATIONS_RU.md',
                      'BASELINE_' + version.replace('.', '_') + '_RU.md']
    allowed = set(document_names + ['SHA256SUMS.txt', 'release-manifest.json', 'XHSC_ISP_V2.23.zip',
                  'Sovol_SH01mod_v' + version + '_sources.zip',
                  'Sovol_SH01mod_v' + version + '_windows.zip'] +
                  [stem + '.' + ext for ext in ['hex', 'bin', 'elf', 'map', 'audit.json']])
    if any(p.name not in allowed for p in out.iterdir()):
        raise ValueError('Unexpected file in release folder; refusing to publish it')
    workflow = (ROOT / '.github/workflows/build.yml').read_text(encoding='utf-8')
    for ext in ['bin', 'hex']:
        expected = re.search(r"'" + ext + r"'\s*=\s*'([a-f0-9]{64})'", workflow)[1]
        if digest(build / (stem + '.' + ext)) != expected:
            raise ValueError('Pinned firmware mismatch: ' + ext)
    app_audit = json.loads((build / (stem + '.audit.json')).read_text())
    if not app_audit['ok'] or app_audit['version'] != version:
        raise ValueError('Artifact audit missing/wrong version')
    for relative, expected in app_audit['source_sha256'].items():
        if digest(ROOT / relative) != expected:
            raise ValueError('Artifact source hash differs: ' + relative)
    names = git('ls-files', '-z').decode().strip('\0').split('\0')
    for name in names:
        if name.startswith(('build/', 'downloads/', 'hardware-local/', '.tools/', '.repowise/')):
            raise ValueError('Private/generated file tracked: ' + name)
    commit = git('rev-parse', 'HEAD').decode().strip()
    epoch = int(git('show', '-s', '--format=%ct', 'HEAD'))
    timestamp = datetime.fromtimestamp(epoch, timezone.utc).timetuple()[:6]
    prefix = 'Sovol_SH01mod_v' + version + '/'
    # Read committed bytes, not working-tree CRLF substitutions.
    sources = [(prefix + name, git('show', 'HEAD:' + name)) for name in names]
    archive(out / ('Sovol_SH01mod_v' + version + '_sources.zip'), sources, timestamp)
    accepted = []
    for ext in ['hex', 'bin', 'elf', 'map', 'audit.json']:
        name = stem + '.' + ext
        shutil.copyfile(build / name, out / name)
        accepted.append((prefix + 'build/v' + version + '/' + name, (build / name).read_bytes()))
    programmer = ROOT / 'downloads/XHSC_ISP_V2.23.zip'
    if digest(programmer) != '2f729500a72fce0e6eacf5be018c2489419bd2cc0623c02a196837bb441501ae':
        raise ValueError('Programmer ZIP mismatch')
    shutil.copyfile(programmer, out / programmer.name)
    accepted.append((prefix + 'downloads/' + programmer.name, programmer.read_bytes()))
    archive(out / ('Sovol_SH01mod_v' + version + '_windows.zip'), sources + accepted, timestamp)
    for name in document_names:
        shutil.copyfile(ROOT / 'docs' / name, out / name)
    assets = [p for p in out.iterdir() if p.name not in ('SHA256SUMS.txt', 'release-manifest.json')]
    manifest = dict(version=version, source_commit=commit, ci_url=args.ci_url,
                    owner_confirmed_on=args.owner_confirmed_on, firmware_changed_after_acceptance=False,
                    application_bytes=app_audit['application_bytes'],
                    assets={p.name: dict(bytes=p.stat().st_size, sha256=digest(p)) for p in sorted(assets)})
    (out / 'release-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (out / 'SHA256SUMS.txt').write_text(''.join(digest(p) + '  ' + p.name + '\n'
        for p in sorted(out.iterdir()) if p.name != 'SHA256SUMS.txt'), encoding='ascii')
    print(json.dumps(dict(output=str(out), source_commit=commit, version=version,
                          assets=len(list(out.iterdir()))), indent=2))


if __name__ == '__main__':
    main()
