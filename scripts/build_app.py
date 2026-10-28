"""Build a native viewer bundle on the target OS; no cross-compilation."""
import subprocess
import sys
from pathlib import Path
root = Path(__file__).resolve().parents[1]
sep = ';' if sys.platform == 'win32' else ':'
manifest = ['--manifest', str(root / 'rb_waveform_core/data/windows.manifest')] if sys.platform == 'win32' else []
subprocess.run([sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean',
                '--onefile' if sys.platform == 'win32' else '--onedir',
                '--windowed', '--name', 'Rekordbox Wave',
                '--collect-all', 'pyrekordbox',
                '--exclude-module', 'tkinter', '--exclude-module', 'pygame',
                '--add-data', f'{root / "rb_waveform_core/data"}{sep}rb_waveform_core/data',
                '--hidden-import', 'PySide6.QtSvg',
                *manifest, str(root / 'rkbx_wave.py')], cwd=root, check=True)
