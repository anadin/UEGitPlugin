#!/usr/bin/env python3
"""Build an editor package of the plugin for this machine's platform, then audit it.

    python3 Scripts/package.py /absolute/new/output-folder         (macOS, Linux)
    py Scripts\\package.py C:\\absolute\\new\\output-folder           (Windows)
    python3 Scripts/package.py --audit-only PACKAGE --report FILE   (check an existing package)

The output folder must not exist yet; it receives Plugin/ (the package), BuildPlugin.log and
PackageAudit.json. Nothing here contacts a Git remote. The engine comes from UE_ENGINE_ROOT, else
the Epic Games Launcher's install of Unreal Engine 5.8, else the default install location.
"""
import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
from pathlib import Path

PLUGIN_ROOT = Path(__file__).resolve().parent.parent
ENGINE_VERSION = '5.8'
MODULE = 'GitSourceControl'
PAYLOADS = ('LICENSE.txt', 'README.md', 'CHANGELOG.md', 'Config/FilterPlugin.ini', 'Docs/Packaging.md', 'Scripts/package.py')
LIBRARIES = {
    'Win64': f'UnrealEditor-{MODULE}.dll',
    'Mac': f'libUnrealEditor-{MODULE}.dylib',
    'Linux': f'libUnrealEditor-{MODULE}.so',
}


def host_platform():
    """Unreal's name for this machine's platform."""
    names = {'Windows': 'Win64', 'Darwin': 'Mac', 'Linux': 'Linux'}
    system = platform.system()
    if system not in names:
        sys.exit(f'Unsupported platform: {system}')
    return names[system]


HOST = host_platform()


def engine_root():
    configured = os.environ.get('UE_ENGINE_ROOT')
    if configured:
        candidates = [configured]
    else:
        candidates = []
        if HOST == 'Win64':
            record = Path(os.environ.get('PROGRAMDATA', 'C:/ProgramData')) / 'Epic/UnrealEngineLauncher/LauncherInstalled.dat'
        else:
            record = Path.home() / 'Library/Application Support/Epic/UnrealEngineLauncher/LauncherInstalled.dat'
        try:
            installs = json.loads(record.read_text(encoding='utf-8')).get('InstallationList', [])
            candidates += [i.get('InstallLocation') for i in installs if i.get('AppName') == f'UE_{ENGINE_VERSION}']
        except (OSError, ValueError):
            pass
        candidates.append({'Win64': f'C:/Program Files/Epic Games/UE_{ENGINE_VERSION}', 'Mac': f'/Users/Shared/Epic Games/UE_{ENGINE_VERSION}'}.get(HOST))
    for candidate in candidates:
        if candidate and (Path(candidate) / 'Engine').is_dir():
            return Path(candidate)
    sys.exit(f'Unreal Engine {ENGINE_VERSION} was not found. Set UE_ENGINE_ROOT to its install folder (the one containing Engine/).')


def digest(path):
    hasher = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            hasher.update(block)
    return hasher.hexdigest()


def audit(package, engine, source=None):
    """Checks a packaged plugin; returns the report as a dictionary."""
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
            files.append({'path': relative, 'bytes': path.stat().st_size, 'sha256': digest(path)})
    for name in PAYLOADS:
        path = package / name
        if not path.is_file() or path.stat().st_size == 0:
            errors.append('Missing or empty required file: ' + name)
        elif source is not None and digest(path) != digest(source / name):
            errors.append('File differs from the source: ' + name)
    try:
        descriptor = json.loads((package / 'GitSourceControl.uplugin').read_text(encoding='utf-8'))
        if not descriptor.get('Installed') or MODULE not in {m['Name'] for m in descriptor['Modules']}:
            errors.append(f'The installed descriptor does not list the {MODULE} module')
    except (OSError, ValueError, KeyError, TypeError) as error:
        errors.append(f'Invalid plugin descriptor: {error}')
    build_id = None
    architectures = None
    try:
        manifest = json.loads((package / f'Binaries/{HOST}/UnrealEditor.modules').read_text(encoding='utf-8'))
        engine_manifest = json.loads((engine / f'Engine/Binaries/{HOST}/UnrealEditor.modules').read_text(encoding='utf-8'))
        build_id = engine_manifest.get('BuildId')
        if not build_id or manifest.get('BuildId') != build_id:
            errors.append('The module BuildId does not match this engine')
        library = manifest.get('Modules', {}).get(MODULE)
        if library != LIBRARIES[HOST] or not (package / f'Binaries/{HOST}' / library).is_file():
            errors.append(f'Missing or unexpected {HOST} module library: {library}')
        elif HOST == 'Mac':
            output = subprocess.check_output(['/usr/bin/lipo', '-archs', str(package / 'Binaries/Mac' / library)], text=True, timeout=15)
            architectures = output.split()
            if 'arm64' not in architectures:
                errors.append('The Mac module library has no arm64 code')
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        errors.append(f'Cannot inspect the module binaries: {error}')
    return {
        'schema': 2,
        'passed': not errors,
        'platform': HOST,
        'scope': 'Editor package for this platform only; other platforms need their own package.',
        'engineBuildId': build_id,
        'architectures': architectures,
        'errors': errors,
        'files': files,
    }


def write_report(result, report):
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(('PASS' if result['passed'] else 'FAIL') + ': ' + str(report))
    for error in result['errors']:
        print(error)
    return 0 if result['passed'] else 1


def package_plugin(output):
    engine = engine_root()
    if output.exists():
        sys.exit(f'Output already exists; choose a new folder: {output}')
    if output == PLUGIN_ROOT or PLUGIN_ROOT in output.parents or (engine / 'Engine') in output.parents:
        sys.exit('The output must be outside the plugin and engine folders.')
    output.mkdir(parents=True)
    script = engine / ('Engine/Build/BatchFiles/RunUAT.bat' if HOST == 'Win64' else 'Engine/Build/BatchFiles/RunUAT.sh')
    command = [str(script), 'BuildPlugin', f'-Plugin={PLUGIN_ROOT / "GitSourceControl.uplugin"}', f'-Package={output / "Plugin"}', '-NoTargetPlatforms']
    if HOST == 'Mac':
        command.append('-Architecture_Mac=arm64')
    print(f'Building the {HOST} editor package; log: {output / "BuildPlugin.log"}')
    with (output / 'BuildPlugin.log').open('w', encoding='utf-8') as log:
        if subprocess.call(command, stdout=log, stderr=subprocess.STDOUT) != 0:
            print(f'BuildPlugin failed; see {output / "BuildPlugin.log"}', file=sys.stderr)
            return 1
    return write_report(audit(output / 'Plugin', engine, PLUGIN_ROOT), output / 'PackageAudit.json')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', nargs='?', type=Path, help='New, absolute folder for the package and its reports.')
    parser.add_argument('--audit-only', type=Path, metavar='PACKAGE', help='Audit an existing package instead of building one.')
    parser.add_argument('--report', type=Path, help='Where --audit-only writes its JSON report.')
    arguments = parser.parse_args()
    if arguments.audit_only:
        if not arguments.report:
            parser.error('--audit-only needs --report')
        package = arguments.audit_only.resolve()
        report = arguments.report.resolve()
        if report == package or package in report.parents:
            parser.error('--report must be outside the package')
        return write_report(audit(package, engine_root()), report)
    if not arguments.output or not arguments.output.is_absolute():
        parser.error('give a new, absolute output folder')
    return package_plugin(arguments.output.resolve())


if __name__ == '__main__':
    sys.exit(main())
