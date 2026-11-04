# Copyright (c) Rekordbox Wave contributors
"""Wave updates itself from its GitHub releases: it finds the latest release, downloads this
computer's file, checks it against the SHA-256 GitHub publishes for it, swaps it in and restarts."""
import hashlib
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path

from . import __version__

REPO = 'palermostest25/rkbx_wave_den'
LATEST = f'https://api.github.com/repos/{REPO}/releases/latest'
RELEASES = f'https://github.com/{REPO}/releases/latest'
BUNDLE_ID = 'io.github.palermostest25.rekordbox-wave'
DOWNLOAD_HOSTS = ('https://github.com/', 'https://objects.githubusercontent.com/',
                  'https://release-assets.githubusercontent.com/')
HEADERS = {'Accept': 'application/vnd.github+json', 'User-Agent': 'Rekordbox-Wave-updater'}


@dataclass(frozen=True)
class Update:
    version: str
    url: str          # this computer's file in the release
    size: int
    sha256: str
    page: str         # the release's page, for its notes


def version_tuple(text):
    """'v2.3.10' -> (2, 3, 10); None when it isn't a plain version."""
    parts = str(text).strip().lstrip('vV').split('.')
    return tuple(int(p) for p in parts) if parts and all(p.isdigit() for p in parts) else None


def asset_name(system=sys.platform, machine=None):
    machine = machine or platform.machine()
    if system == 'win32':
        return 'Rekordbox-Wave-Windows-x64.exe'
    if system == 'darwin':
        return f"Rekordbox-Wave-macOS-{'AppleSilicon' if machine == 'arm64' else 'Intel'}.dmg"
    return None


def can_install():
    """Only a packaged Wave replaces itself; from source, update with git."""
    return bool(getattr(sys, 'frozen', False)) and asset_name() is not None


def check(current=__version__, fetch=None, name=None):
    """The newer release for this computer, or None. Raises OSError/ValueError when GitHub can't be read."""
    fetch = fetch or (lambda url: urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=10))
    with fetch(LATEST) as answer:
        release = json.load(answer)
    newer, mine = version_tuple(release.get('tag_name', '')), version_tuple(current)
    if release.get('draft') or release.get('prerelease') or not newer or not mine or newer <= mine:
        return None
    name = name or asset_name()
    asset = next((a for a in release.get('assets', ()) if a.get('name') == name), None)
    if not asset:
        return None
    url, digest = asset.get('browser_download_url', ''), str(asset.get('digest') or '')
    if not url.startswith(DOWNLOAD_HOSTS) or not digest.startswith('sha256:') or len(digest) != 71:
        return None  # only GitHub's own files, and only ones it publishes a checksum for
    return Update('.'.join(map(str, newer)), url, int(asset.get('size') or 0), digest[7:].lower(),
                  release.get('html_url') or RELEASES)


def download(update, folder, progress=None, fetch=None):
    """Fetch the update into ``folder``; the file is kept only if its size and SHA-256 match GitHub's."""
    fetch = fetch or (lambda url: urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': HEADERS['User-Agent']}), timeout=30))
    target = Path(folder) / update.url.rsplit('/', 1)[-1]
    part = target.with_name(target.name + '.part')
    digest, done = hashlib.sha256(), 0
    try:
        with fetch(update.url) as answer, open(part, 'wb') as out:
            if not answer.geturl().startswith(DOWNLOAD_HOSTS):
                raise OSError('The update was redirected away from GitHub')
            while chunk := answer.read(1 << 20):
                out.write(chunk)
                digest.update(chunk)
                done += len(chunk)
                if progress:
                    progress(done, update.size)
        if done != update.size or digest.hexdigest() != update.sha256:
            raise OSError('The downloaded update did not match GitHub’s checksum')
        part.replace(target)
        return target
    finally:
        part.unlink(missing_ok=True)


def install(file):
    """Put the downloaded release in place of this Wave and start it once this one has quit.
    The caller quits Wave right after; a failure leaves the current Wave untouched."""
    file = Path(file)
    if sys.platform == 'win32':
        return _install_windows(file)
    if sys.platform == 'darwin':
        return _install_mac(file)
    raise OSError('Updating is only built for Windows and macOS')


def _install_windows(file):
    exe = Path(sys.executable)
    old = exe.with_name(exe.stem + '.old.exe')
    old.unlink(missing_ok=True)
    exe.rename(old)  # Windows lets a running program be renamed, not replaced
    try:
        shutil.move(str(file), str(exe))
    except OSError:
        old.rename(exe)
        raise
    import ctypes
    ctypes.windll.kernel32.SetDllDirectoryW(None)  # as for Rekordbox: the new Wave must not inherit this one's
    env = {k: v for k, v in os.environ.items() if not k.startswith(('_PYI', '_MEIPASS'))}
    subprocess.Popen([str(exe)], cwd=exe.parent, env=env, close_fds=True,
                     creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)


def _install_mac(file):
    app = Path(sys.executable).resolve().parents[2]
    if app.suffix != '.app' or not os.access(app.parent, os.W_OK):
        raise OSError(f'Wave can’t replace itself in {app.parent}: download the update from GitHub instead')
    fresh = app.with_name('.' + app.name + '.new')
    shutil.rmtree(fresh, ignore_errors=True)
    mount = Path(tempfile.mkdtemp(prefix='wave-update-'))
    subprocess.run(['/usr/bin/hdiutil', 'attach', '-nobrowse', '-readonly', '-noautoopen', '-mountpoint', str(mount), str(file)],
                   check=True, capture_output=True, timeout=120)
    try:
        source = next(mount.glob('*.app'))
        subprocess.run(['/usr/bin/ditto', str(source), str(fresh)], check=True, timeout=300)
    finally:
        subprocess.run(['/usr/bin/hdiutil', 'detach', str(mount), '-quiet'], timeout=60)
    signed = subprocess.run(['/usr/bin/codesign', '-dv', str(fresh)], capture_output=True, text=True)
    verified = subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(fresh)], capture_output=True)
    if verified.returncode or f'Identifier={BUNDLE_ID}' not in signed.stderr:
        shutil.rmtree(fresh, ignore_errors=True)
        raise OSError('The downloaded Wave is not signed as Rekordbox Wave')
    old = app.with_name('.' + app.name + '.old')
    a, f, o = (shlex.quote(str(p)) for p in (app, fresh, old))
    # Waits for this Wave to quit, swaps the bundles (back again if the swap fails), then opens the new one.
    script = (f'while kill -0 {os.getpid()} 2>/dev/null; do sleep 0.2; done; rm -rf {o}; '
              f'mv {a} {o} && {{ mv {f} {a} || mv {o} {a}; }}; rm -rf {o}; open {a}')
    subprocess.Popen(['/bin/sh', '-c', script], start_new_session=True, close_fds=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def clean_up():
    """Remove what an earlier update left beside Wave."""
    if not getattr(sys, 'frozen', False):
        return
    if sys.platform == 'win32':
        exe = Path(sys.executable)
        exe.with_name(exe.stem + '.old.exe').unlink(missing_ok=True)
    elif sys.platform == 'darwin':
        app = Path(sys.executable).resolve().parents[2]
        for leftover in ('.new', '.old'):
            shutil.rmtree(app.with_name('.' + app.name + leftover), ignore_errors=True)
