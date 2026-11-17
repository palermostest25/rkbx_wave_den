"""Rekordbox for Wave: which versions are installed, and installing the one Wave's bridge supports.

Rekordbox is AlphaTheta's: Wave never ships it. When the supported version is missing, Wave gets
AlphaTheta's own installer - found on rekordbox.com, or a file the DJ downloaded - checks that it is
signed by AlphaTheta (or Pioneer DJ) and runs it. On Windows, Rekordbox installs each version in a
folder of its own, so the supported version can sit beside a newer one, and Wave starts the one it
supports.
"""
from __future__ import annotations

from dataclasses import dataclass
import json
import logging
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

log = logging.getLogger(__name__)

SUPPORTED = (7, 2, 19)        # the version Wave's bridge was checked against, on both platforms
SUPPORTED_TEXT = '.'.join(map(str, SUPPORTED))
WINDOWS_ZIP = 'Install_rekordbox_x64_{}.zip'.format('_'.join(map(str, SUPPORTED)))
MAC_ZIP = 'Install_rekordbox_{}.pkg.zip'.format('_'.join(map(str, SUPPORTED)))
DOWNLOAD_PAGE = 'https://rekordbox.com/en/download/'
# Where a maintainer can point Wave at AlphaTheta's exact download for the supported version, without a release.
SOURCES = 'https://raw.githubusercontent.com/palermostest25/rkbx_wave_den/main/docs/rekordbox-sources.json'
OFFICIAL_HOSTS = ('https://rekordbox.com/', 'https://cdn.rekordbox.com/', 'https://www.rekordbox.com/')
SIGNERS = ('alphatheta', 'pioneer dj', 'pioneer corporation')
HEADERS = {'User-Agent': 'Rekordbox-Wave-setup'}
MAC_APP = Path('/Applications/rekordbox 7/rekordbox.app')


@dataclass(frozen=True)
class Install:
    version: tuple
    executable: Path

    @property
    def text(self):
        return '.'.join(map(str, self.version))

    @property
    def supported(self):
        return self.version[:3] == SUPPORTED


def version_of(text):
    numbers = tuple(int(n) for n in re.findall(r'\d+', str(text)))
    return numbers or None


def installed(root=None, system=sys.platform):
    """Every Rekordbox found, oldest first."""
    found = []
    if system == 'win32':
        root = Path(root or Path(os.environ.get('ProgramFiles', r'C:\Program Files')) / 'rekordbox')
        for exe in root.glob('rekordbox */rekordbox.exe'):
            version = version_of(exe.parent.name)
            if version:
                found.append(Install(version, exe))
    elif system == 'darwin':
        app = Path(root) if root else MAC_APP
        try:
            info = plistlib.loads((app / 'Contents' / 'Info.plist').read_bytes())
            version = version_of(info.get('CFBundleShortVersionString', ''))
            if version:
                found.append(Install(version, app / 'Contents' / 'MacOS' / 'rekordbox'))
        except (OSError, ValueError):
            pass
    return sorted(found, key=lambda i: i.version)


def supported_install(root=None, system=sys.platform):
    """The installed Rekordbox Wave supports, or None."""
    return next((i for i in reversed(installed(root, system)) if i.supported), None)


def summary(root=None, system=sys.platform):
    """One line for Settings: what is installed, and whether Wave can use it."""
    found = installed(root, system)
    if not found:
        return f'Rekordbox is not installed. Wave needs Rekordbox {SUPPORTED_TEXT}.'
    if any(i.supported for i in found):
        others = [i.text for i in found if not i.supported]
        return f'Rekordbox {SUPPORTED_TEXT} is installed' + (f' (also {", ".join(others)}; Wave uses {SUPPORTED_TEXT})' if others else '') + '.'
    return (f'Rekordbox {", ".join(i.text for i in found)} is installed, but Wave needs {SUPPORTED_TEXT}. '
            f'Install {SUPPORTED_TEXT} beside it.' if system == 'win32' else
            f'Rekordbox {found[-1].text} is installed, but Wave needs {SUPPORTED_TEXT}.')


def _fetch(url, timeout=20):
    return urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=timeout)


def find_download(system=sys.platform, fetch=_fetch):
    """AlphaTheta's download of the supported Rekordbox: (url, sha256 or ''), or (None, '') when not found."""
    name = WINDOWS_ZIP if system == 'win32' else MAC_ZIP
    key = 'windows' if system == 'win32' else 'mac'
    try:
        with fetch(SOURCES) as answer:
            entry = json.load(answer).get(key) or {}
        if entry.get('version') == SUPPORTED_TEXT and str(entry.get('url', '')).startswith(OFFICIAL_HOSTS):
            return entry['url'], str(entry.get('sha256') or '').lower()
    except Exception as exc:
        log.info('No maintained Rekordbox download: %s', exc)
    try:
        with fetch(DOWNLOAD_PAGE) as answer:
            page = answer.read().decode('utf-8', 'replace')
        stem = re.escape(re.sub(r'(\.pkg)?\.zip$', '', name))
        match = re.search(r'https://(?:cdn\.)?rekordbox\.com/[^"\'\s<>]*' + stem + r'[^"\'\s<>]*\.zip', page)
        if match:
            return match.group(0).replace('\\/', '/'), ''
    except Exception as exc:
        log.info('rekordbox.com could not be read: %s', exc)
    return None, ''


def download(url, folder, progress=None, sha256='', fetch=_fetch):
    """AlphaTheta's archive, into ``folder``. Only from rekordbox.com, and checked against ``sha256`` when known."""
    import hashlib
    if not url.startswith(OFFICIAL_HOSTS):
        raise OSError('Wave only downloads Rekordbox from rekordbox.com')
    target = Path(folder) / url.rsplit('/', 1)[-1].split('?')[0]
    part = target.with_name(target.name + '.part')
    digest, done = hashlib.sha256(), 0
    with fetch(url, timeout=60) as answer, open(part, 'wb') as out:
        if not answer.geturl().startswith(OFFICIAL_HOSTS):
            part.unlink(missing_ok=True)
            raise OSError('The download was redirected away from rekordbox.com')
        total = int(answer.headers.get('Content-Length') or 0)
        while chunk := answer.read(1 << 20):
            out.write(chunk)
            digest.update(chunk)
            done += len(chunk)
            if progress:
                progress(done, total)
    if sha256 and digest.hexdigest() != sha256:
        part.unlink(missing_ok=True)
        raise OSError('The Rekordbox download did not match its checksum')
    part.replace(target)
    return target


def installer_in(archive, folder):
    """The installer inside AlphaTheta's zip (or the file itself, when it is one)."""
    archive = Path(archive)
    if archive.suffix.lower() in ('.exe', '.pkg'):
        return archive
    with zipfile.ZipFile(archive) as z:
        names = [n for n in z.namelist() if n.lower().endswith(('.exe', '.pkg')) and not n.startswith('__MACOSX')]
        if not names:
            raise OSError('No Rekordbox installer in that file')
        name = min(names, key=len)
        target = Path(folder) / Path(name).name
        with z.open(name) as src, open(target, 'wb') as out:
            shutil.copyfileobj(src, out)
    return target


def signer(path, system=sys.platform):
    """Who signed ``path``, if the signature is valid; '' otherwise."""
    if system == 'win32':
        script = ("$s = Get-AuthenticodeSignature -LiteralPath $env:WAVE_FILE; "
                  "if ($s.Status -eq 'Valid') { $s.SignerCertificate.Subject }")
        result = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', script],
                                env={**os.environ, 'WAVE_FILE': str(path)}, capture_output=True, text=True, timeout=60,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        return result.stdout.strip()
    if system == 'darwin':
        result = subprocess.run(['/usr/sbin/pkgutil', '--check-signature', str(path)], capture_output=True, text=True, timeout=60)
        return result.stdout if result.returncode == 0 and 'Status: signed' in result.stdout else ''
    return ''


def trusted(subject):
    return any(name in subject.lower() for name in SIGNERS)


def run_installer(installer, system=sys.platform):
    """Run AlphaTheta's installer, after checking its signature. Blocks until it finishes (Windows)."""
    subject = signer(installer, system)
    if not trusted(subject):
        raise OSError('That installer is not signed by AlphaTheta or Pioneer DJ: Wave will not run it')
    log.info('Running the Rekordbox installer signed by %s', subject.splitlines()[0] if subject else '?')
    if system == 'win32':
        import ctypes
        from ctypes import wintypes

        class SHELLEXECUTEINFOW(ctypes.Structure):
            _fields_ = [('cbSize', wintypes.DWORD), ('fMask', ctypes.c_ulong), ('hwnd', wintypes.HWND),
                        ('lpVerb', wintypes.LPCWSTR), ('lpFile', wintypes.LPCWSTR), ('lpParameters', wintypes.LPCWSTR),
                        ('lpDirectory', wintypes.LPCWSTR), ('nShow', ctypes.c_int), ('hInstApp', wintypes.HINSTANCE),
                        ('lpIDList', ctypes.c_void_p), ('lpClass', wintypes.LPCWSTR), ('hkeyClass', wintypes.HKEY),
                        ('dwHotKey', wintypes.DWORD), ('hIcon', wintypes.HANDLE), ('hProcess', wintypes.HANDLE)]
        info = SHELLEXECUTEINFOW()
        info.cbSize = ctypes.sizeof(info)
        info.fMask = 0x40  # SEE_MASK_NOCLOSEPROCESS
        info.lpVerb = 'runas'  # Rekordbox installs for every user: Windows asks for permission
        info.lpFile = str(installer)
        info.lpDirectory = str(Path(installer).parent)
        info.nShow = 1
        if not ctypes.windll.shell32.ShellExecuteExW(ctypes.byref(info)):
            raise OSError('The Rekordbox installer did not start (permission refused?)')
        ctypes.windll.kernel32.WaitForSingleObject(info.hProcess, 0xFFFFFFFF)
        ctypes.windll.kernel32.CloseHandle(info.hProcess)
    elif system == 'darwin':
        subprocess.run(['/usr/bin/open', '-W', str(installer)], check=True)
    else:
        raise OSError('Rekordbox installs on Windows and macOS')


def install_supported(progress=None, archive=None, status=lambda text: None):
    """Get and install the supported Rekordbox: from ``archive`` (a file the DJ chose) or rekordbox.com.
    Returns the install, or raises OSError saying what to do."""
    folder = Path(tempfile.mkdtemp(prefix='wave-rekordbox-'))
    try:
        if archive is None:
            status('Looking for Rekordbox ' + SUPPORTED_TEXT + ' on rekordbox.com…')
            url, sha = find_download()
            if not url:
                raise OSError(f'rekordbox.com no longer offers {SUPPORTED_TEXT} directly. Download it from the '
                              f'rekordbox.com archive, then choose the file with USE A DOWNLOADED INSTALLER.')
            status('Downloading Rekordbox ' + SUPPORTED_TEXT + '…')
            archive = download(url, folder, progress, sha)
        status('Checking the installer…')
        installer = installer_in(archive, folder)
        status('Installing Rekordbox ' + SUPPORTED_TEXT + ': answer its installer…')
        run_installer(installer)
        found = supported_install()
        if not found and sys.platform == 'win32':
            raise OSError(f'Rekordbox {SUPPORTED_TEXT} was not installed')
        return found
    finally:
        shutil.rmtree(folder, ignore_errors=True)
