"""Capture the production Qt surface; no raster or browser stand-in."""
import argparse
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from PySide6.QtWidgets import QApplication
from PySide6.QtTest import QTest
from rb_waveform_core.qt_app import Session, MainWindow
from rb_waveform_core.cues import CueMarker

parser=argparse.ArgumentParser()
parser.add_argument('--out',type=Path,required=True)
args=parser.parse_args()
args.out.mkdir(parents=True,exist_ok=True)
app=QApplication([])
app.setStyle('Fusion')
app.setQuitOnLastWindowClosed(False)
with tempfile.TemporaryDirectory() as temp:
    session=Session(demo=True,config_path=Path(temp)/'settings.json')
    window=MainWindow(session)
    for width,height,count in [(1920,1080,4),(1280,720,4),(1920,1080,2)]:
        window.resize(width,height)
        window.set_count(count)
        window.show()
        QTest.qWait(50)
        window.grab().save(str(args.out/f'qt-{width}x{height}-{count}deck.png'))
    window.set_count(4)
    window.resize(1280,720)
    window.toggle_settings()
    QTest.qWait(40)
    window.grab().save(str(args.out/'qt-settings-1280.png'))
    window.toggle_settings()
    for deck in session.decks.values():
        deck.cues.extend([CueMarker(41.25,(255,190,54),True),CueMarker(41.25,(130,80,255),False,3)])
    QTest.qWait(40)
    window.grab().save(str(args.out/'qt-clustered-cues.png'))
    for deck in session.decks.values():
        deck.analysis=None
    QTest.qWait(40)
    window.grab().save(str(args.out/'qt-empty.png'))
    window.close()
