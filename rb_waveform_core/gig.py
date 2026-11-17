"""What keeps Wave dependable through a set: a log of what happened, one Wave at a time, and a
screen that never sleeps while Wave is up."""
from __future__ import annotations

import faulthandler
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import subprocess
import sys
import threading

log = logging.getLogger('rkbx_wave')
_fault_file = None


def setup_logging(folder):
    """Log to ``folder``/wave.log (kept small, five files), and record every crash there too."""
    global _fault_file
    folder = Path(folder)
    root = logging.getLogger()
    if any(getattr(h, 'wave_log', False) for h in root.handlers):
        return folder / 'wave.log'
    try:
        folder.mkdir(parents=True, exist_ok=True)
        handler = RotatingFileHandler(folder / 'wave.log', maxBytes=1_000_000, backupCount=4, encoding='utf-8')
    except OSError:
        return None
    handler.wave_log = True
    handler.setFormatter(logging.Formatter('%(asctime)s %(levelname)s %(name)s: %(message)s'))
    root.addHandler(handler)
    root.setLevel(logging.INFO)
    for noisy in ('pyrekordbox', 'PIL', 'urllib3'):
        logging.getLogger(noisy).setLevel(logging.WARNING)

    def unhandled(kind, value, trace):
        if issubclass(kind, KeyboardInterrupt):
            return sys.__excepthook__(kind, value, trace)
        log.critical('Unhandled error', exc_info=(kind, value, trace))
    sys.excepthook = unhandled
    threading.excepthook = lambda args: log.critical(
        'Unhandled error in thread %s', getattr(args.thread, 'name', '?'), exc_info=(args.exc_type, args.exc_value, args.exc_traceback))
    try:
        _fault_file = open(folder / 'crash.log', 'a', encoding='utf-8')
        faulthandler.enable(_fault_file)
    except OSError:
        pass
    try:
        from PySide6.QtCore import qInstallMessageHandler, QtMsgType
        levels = {QtMsgType.QtWarningMsg: logging.WARNING, QtMsgType.QtCriticalMsg: logging.ERROR,
                  QtMsgType.QtFatalMsg: logging.CRITICAL}
        qInstallMessageHandler(lambda kind, _, text: log.log(levels.get(kind, logging.DEBUG), 'Qt: %s', text))
    except Exception:
        pass
    return folder / 'wave.log'


class SingleInstance:
    """One Wave at a time: a second start brings the running Wave forward and quits."""
    def __init__(self, name='rekordbox-wave'):
        from PySide6.QtNetwork import QLocalServer, QLocalSocket
        self.name = f'{name}-{os.environ.get("USERNAME") or os.environ.get("USER") or "user"}'
        self.server = None
        self.QLocalServer = QLocalServer
        socket = QLocalSocket()
        socket.connectToServer(self.name)
        if socket.waitForConnected(400):
            socket.write(b'show\n')
            socket.flush()
            socket.waitForBytesWritten(400)
            socket.disconnectFromServer()
            self.primary = False
            return
        self.primary = True

    def listen(self, on_show):
        QLocalServer = self.QLocalServer
        QLocalServer.removeServer(self.name)  # left by a Wave that crashed
        self.server = QLocalServer()
        self.server.setSocketOptions(QLocalServer.SocketOption.UserAccessOption)
        if not self.server.listen(self.name):
            log.warning('Single-instance server: %s', self.server.errorString())
            return

        def connected():
            while self.server.hasPendingConnections():
                client = self.server.nextPendingConnection()
                client.readyRead.connect(lambda c=client: (c.readAll(), on_show()))
                client.disconnected.connect(client.deleteLater)
        self.server.newConnection.connect(connected)


class KeepAwake:
    """No screen saver, display sleep or system sleep while Wave runs."""
    def __init__(self):
        self.process = None
        self.on = False

    def set(self, on):
        if on == self.on:
            return
        self.on = on
        try:
            if sys.platform == 'win32':
                import ctypes
                # ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED, held by the UI thread until cleared.
                ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | (0x1 | 0x2 if on else 0))
            elif sys.platform == 'darwin':
                if on and self.process is None:
                    self.process = subprocess.Popen(['/usr/bin/caffeinate', '-d', '-i', '-w', str(os.getpid())],
                                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                elif not on and self.process is not None:
                    self.process.terminate()
                    self.process = None
        except Exception as exc:
            log.warning('Could not keep the screen awake: %s', exc)
