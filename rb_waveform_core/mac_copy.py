"""Explicitly prepare a separately signed playback copy, never patch the vendor app.

Call only after the user chooses this route. No library or credentials are copied.
A new private Rekordbox profile requires its own normal onboarding/login.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import tempfile
import uuid

# LaunchServices can start nested vendor helpers outside this inherited sandbox.
# Do not enable until the complete helper topology is independently isolated.
MAC_COPY_ISOLATION_VERIFIED = False


def _require_verified_isolation():
    if not MAC_COPY_ISOLATION_VERIFIED:
        raise RuntimeError('Mac playback-copy setup is disabled: vendor helpers can escape profile isolation')


BUNDLE_ID = 'dev.rekordbox-wave.calibration'
MARKER = Path('Contents/Resources/rekordbox-wave-copy.json')
BINARY = Path('Contents/MacOS/rekordbox')


def _digest(path: Path) -> str:
    with path.open('rb') as source:
        digest = hashlib.sha256()
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
        return digest.hexdigest()


def _run(argv):
    return subprocess.run([str(x) for x in argv], check=True, capture_output=True)


def _write_json_atomic(path: Path, value) -> None:
    with tempfile.TemporaryDirectory(prefix='.rekordbox-wave-json-', dir=path.parent) as staging:
        pending = Path(staging)/'value.json'
        pending.write_text(json.dumps(value, indent=2))
        pending.replace(path)


def _contains(parent: Path, child: Path) -> bool:
    return child == parent or parent in child.parents


def _paths(source_app, copy_app, profile_root):
    source, copy, profile = [Path(p).expanduser().resolve() for p in (source_app, copy_app, profile_root)]
    if source.suffix != '.app' or copy.suffix != '.app':
        raise ValueError('Source and playback copy must be app bundles')
    if any((_contains(source, copy), _contains(copy, source), _contains(source, profile), _contains(profile, source),
            _contains(copy, profile), _contains(profile, copy))):
        raise ValueError('Source app, playback copy and private profile must be separate paths')
    if not (source / BINARY).is_file():
        raise ValueError('Select the installed Rekordbox app')
    return source, copy, profile


def isolation_policy(profile: Path, user_home: Path, darwin_temp: Path | None = None) -> str:
    """Local tracks are readable; the real Pioneer database/preferences are not."""
    protected = [user_home/'Library/Pioneer', user_home/'Library/Application Support/Pioneer',
                 user_home/'Library/Preferences/com.pioneerdj.rekordboxdj.plist']
    if any(_contains(path, profile) for path in protected):
        raise ValueError('Private profile cannot be inside the original Pioneer library')
    quote = lambda value: json.dumps(str(value))
    rules = [
        '(version 1)', '(allow default)', '(deny file-write*)',
        '(allow file-write* (subpath '+quote(profile)+') (literal "/dev/null") (literal "/dev/zero"))',
        '(deny file-read* '+' '.join('(subpath '+quote(path)+')' for path in protected)+')',
    ]
    if darwin_temp is not None:
        # Foundation ignores TMPDIR for atomic-save staging on macOS. Permit
        # only this process name's random NSIRD directories, never all user temp.
        temporary_items = darwin_temp.resolve()/'TemporaryItems'
        pattern = '^'+re.escape(str(temporary_items/'NSIRD_rekordbox_'))+'[^/]+(/.*)?$'
        rules.extend([
            '(allow file-write-create (literal '+quote(temporary_items)+'))',
            '(allow file-write* (regex '+json.dumps(pattern)+'))',
        ])
    return '\n'.join(rules)+'\n'


@dataclass(frozen=True)
class MacCopyLaunch:
    executable: Path
    profile_root: Path
    argv: tuple[str, ...]
    environment: dict[str, str]
    log_path: Path

    def start(self) -> int:
        """Start this exact owned copy; leave any existing copy/original running."""
        _require_verified_isolation()
        import psutil
        for process in psutil.process_iter(['pid', 'exe']):
            try:
                if process.info.get('exe') and Path(process.info['exe']).resolve() == self.executable:
                    return process.info['pid']
            except psutil.Error:
                continue
        prefix = self.argv[:3]
        env = dict(os.environ, **self.environment)
        # Confirm the actual OS policy denies an outside write before launching.
        probe = self.profile_root.parent / ('.rekordbox-wave-isolation-'+uuid.uuid4().hex)
        check = subprocess.run([*prefix, '/usr/bin/touch', str(probe)], env=env, capture_output=True)
        if check.returncode == 0:
            probe.unlink(missing_ok=True)
            raise RuntimeError('macOS sandbox failed the outside-profile write check')
        _run([*prefix, '/usr/bin/touch', self.profile_root/'isolation-probe'])
        with self.log_path.open('ab') as log:
            process = subprocess.Popen(self.argv, env=env, stdout=log, stderr=subprocess.STDOUT)
        return process.pid


def prepare_rekordbox_copy(source_app, copy_app, profile_root) -> MacCopyLaunch:
    """Create once, or verify an already owned copy. Never overwrite or update one."""
    _require_verified_isolation()
    source, copy, profile = _paths(source_app, copy_app, profile_root)
    policy = isolation_policy(profile, Path.home().resolve())
    _run(['/usr/bin/codesign', '--verify', '--deep', '--strict', source])
    original_hash = _digest(source / BINARY)
    info = plistlib.loads((source/'Contents/Info.plist').read_bytes())
    if info.get('CFBundleIdentifier') != 'com.pioneerdj.rekordboxdj':
        raise ValueError('Source must be the original installed Rekordbox app')
    identity = {'format': 1, 'source_app': str(source), 'source_sha256': original_hash,
                'source_version': info.get('CFBundleShortVersionString'),
                'copy_app': str(copy), 'profile_root': str(profile)}
    manifest_path = profile/'copy-manifest.json'
    profile_marker = profile/'rekordbox-wave-profile.json'
    if profile.exists() and any(profile.iterdir()):
        try:
            owned_profile = json.loads(profile_marker.read_text()) == identity
        except (OSError, ValueError):
            owned_profile = False
        if not owned_profile:
            raise ValueError('Choose a new empty private profile folder; existing user data is never reused')
    if copy.exists():
        try:
            marker = json.loads((copy/MARKER).read_text())
        except (OSError, ValueError) as error:
            raise ValueError('Existing app is not an owned Rekordbox Wave playback copy; choose a new path') from error
        if marker != identity:
            raise ValueError('Playback copy/source/profile changed; choose a new copy path')
        _run(['/usr/bin/codesign', '--verify', '--deep', '--strict', copy])
        if manifest_path.exists():
            manifest = json.loads(manifest_path.read_text())
            if manifest.get('identity') != identity or manifest.get('copy_sha256') != _digest(copy/BINARY):
                raise ValueError('Playback copy changed; choose a new copy path')
        # A verified owned copy without a profile manifest can result from a
        # failed profile commit. Resume setup; do not delete any profile data.
    else:
        copy.parent.mkdir(parents=True, exist_ok=True)
        # Stage beside the destination: ditto works across volumes and the final
        # rename is atomic. TemporaryDirectory removes only our own staging data.
        with tempfile.TemporaryDirectory(prefix='.rekordbox-wave-', dir=copy.parent) as staging:
            staged = Path(staging)/'Playback.app'
            _run(['/usr/bin/ditto', source, staged])
            (staged/MARKER).parent.mkdir(parents=True, exist_ok=True)
            (staged/MARKER).write_text(json.dumps(identity, indent=2))
            raw = _run(['/usr/bin/codesign', '-d', '--entitlements', ':-', source]).stdout
            entitlements = plistlib.loads(raw)
            entitlements['com.apple.security.get-task-allow'] = True
            entitlement_path = Path(staging)/'playback-entitlements.plist'
            entitlement_path.write_bytes(plistlib.dumps(entitlements))
            info['CFBundleIdentifier'] = BUNDLE_ID
            info['CFBundleDisplayName'] = 'Rekordbox Wave Playback Copy'
            (staged/'Contents/Info.plist').write_bytes(plistlib.dumps(info))
            _run(['/usr/bin/codesign', '--force', '--deep', '--sign', '-', '--options', 'runtime',
                  '--entitlements', entitlement_path, staged])
            _run(['/usr/bin/codesign', '--verify', '--deep', '--strict', staged])
            if _digest(source/BINARY) != original_hash:
                raise RuntimeError('Installed Rekordbox changed during copy preparation')
            if copy.exists():
                raise ValueError('Playback destination appeared during preparation; choose a new path')
            staged.rename(copy)
    # No profile files are created until the app is completely verified. A
    # profile commit failure is recoverable through the owned-copy branch above.
    profile.mkdir(parents=True, exist_ok=True)
    if not profile_marker.exists():
        _write_json_atomic(profile_marker, identity)
    _write_json_atomic(manifest_path, {'identity': identity, 'copy_sha256': _digest(copy/BINARY)})
    for name in ('home', 'tmp', 'settings'):
        (profile/name).mkdir(exist_ok=True)
    temporary = _run(['/usr/bin/getconf', 'DARWIN_USER_TEMP_DIR']).stdout.decode().strip()
    if not temporary or not Path(temporary).is_absolute():
        raise RuntimeError('Cannot determine macOS atomic-save staging directory')
    sandbox_path = profile/'isolation.sb'
    sandbox_path.write_text(isolation_policy(profile, Path.home().resolve(), Path(temporary)))
    executable = copy/BINARY
    return MacCopyLaunch(executable, profile,
        ('/usr/bin/sandbox-exec', '-f', str(sandbox_path), str(executable), '--settings-root-path='+str(profile/'settings')),
        {'HOME': str(profile/'home'), 'CFFIXED_USER_HOME': str(profile/'home'), 'TMPDIR': str(profile/'tmp')+'/'}, profile/'playback.log')
