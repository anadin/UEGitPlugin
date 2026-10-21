#!/usr/bin/env python3
"""Audit a Mac arm64 editor package without opening a project or contacting a remote."""
import argparse
import hashlib
import json
from pathlib import Path
import stat
import subprocess


PAYLOADS = (
    'LICENSE.txt', 'README.md', 'CHANGELOG.md', 'Config/FilterPlugin.ini', 'Docs/Packaging.md',
    'Scripts/package-mac.sh', 'Scripts/check_mac_package.py',
)
MODULES = ('GitSourceControl',)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inspect_package(package, engine, source=None):
    package = Path(package).resolve(strict=True)
    engine = Path(engine).resolve(strict=True)
    errors, files = [], []
    for path in sorted(package.rglob('*')):
        relative = path.relative_to(package).as_posix()
        parts = path.relative_to(package).parts
        if path.is_symlink():
            errors.append('Linked package entry: ' + relative)
            continue
        if any(p in ('.git', 'Saved', 'HostProject', '__pycache__') for p in parts) or path.suffix == '.pyc':
            errors.append('Development state in package: ' + relative)
        if path.is_file():
            files.append(dict(path=relative, bytes=path.stat().st_size, mode=stat.S_IMODE(path.stat().st_mode), sha256=digest(path)))
    for name in PAYLOADS:
        path = package / name
        if not path.is_file() or path.is_symlink() or path.stat().st_size == 0:
            errors.append('Missing or invalid required payload: ' + name)
        elif source is not None:
            original = Path(source) / name
            if not original.is_file() or original.is_symlink() or digest(path) != digest(original):
                errors.append('Payload differs from source: ' + name)
    descriptor = package / 'GitSourceControl.uplugin'
    try:
        data = json.loads(descriptor.read_text())
        names = {m['Name'] for m in data['Modules']}
        if not data.get('Installed') or not set(MODULES).issubset(names):
            errors.append('Installed descriptor does not contain the plugin modules')
    except (OSError, ValueError, KeyError, TypeError) as exc:
        errors.append('Invalid plugin descriptor: ' + str(exc))
    architecture = {}
    try:
        manifest = json.loads((package / 'Binaries/Mac/UnrealEditor.modules').read_text())
        host = json.loads((engine / 'Engine/Binaries/Mac/UnrealEditor.modules').read_text())
        if manifest.get('BuildId') != host.get('BuildId') or not host.get('BuildId'):
            errors.append('Module BuildId does not match this engine')
        for name in MODULES:
            library = manifest.get('Modules', {}).get(name)
            if library != 'libUnrealEditor-' + name + '.dylib':
                errors.append('Missing or unexpected module library: ' + name)
                continue
            binary = package / 'Binaries/Mac' / library
            architecture[name] = subprocess.check_output(['/usr/bin/lipo', '-archs', str(binary)], text=True, timeout=15).strip().split()
            if architecture[name] != ['arm64']:
                errors.append('Expected arm64-only module: ' + name)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        errors.append('Cannot inspect module binaries: ' + str(exc))
    return dict(schema=1, passed=not errors, platform='Mac', architecture='arm64', scope='Editor-only candidate; runtime/Windows acceptance not implied.', engineBuildId=host.get('BuildId') if 'host' in locals() else None, binaryArchitectures=architecture, errors=errors, files=files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--source', type=Path, help='Compare required payload hashes to this source checkout')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    # Do not insert an audit report into the payload it describes.
    package = args.package.resolve()
    report = args.report.resolve()
    if report == package or package in report.parents:
        parser.error('--report must be outside the package')
    try:
        result = inspect_package(package, args.engine, args.source)
    except (OSError, ValueError) as exc:
        result = dict(schema=1, passed=False, errors=[str(exc)])
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(result, indent=2) + '\n')
    print(('PASS' if result['passed'] else 'FAIL') + ': ' + str(report))
    for error in result['errors']:
        print(error)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
