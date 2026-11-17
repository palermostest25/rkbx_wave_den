"""Rekordbox as Wave's engine: started, connected and kept running behind Wave, without being asked.

``HostPolicy`` decides; ``RekordboxHost`` carries the decisions out off the UI thread. The policy
never restarts a Rekordbox that might be playing: when Wave can't see the decks it waits for the
DJ, and Rekordbox itself still asks before quitting with a deck playing.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import logging
import sys
import time

log = logging.getLogger(__name__)

# What the bridge in the running Rekordbox is doing, as RekordboxBridgeReader.state reports it.
ABSENT, NONE, STARTING, LIVE, BUSY, OLDER, UNSUPPORTED, FAILED = (
    'absent', 'none', 'starting', 'live', 'busy', 'older', 'unsupported', 'failed')

LAUNCH, ATTACH, RESTART = 'launch', 'attach', 'restart'
# Asks shown to the DJ when Wave won't act alone.
ASK_START = 'Rekordbox is not running'
ASK_CONNECT = 'Rekordbox runs without Wave: CONNECT restarts it with Wave (stop the decks first)'
ASK_GAVE_UP = 'Rekordbox keeps closing: CONNECT starts it again'
ASK_UPDATE = "Rekordbox runs an earlier Wave's bridge: CONNECT restarts it to finish updating (stop the decks first)"


@dataclass(frozen=True)
class Seen:
    pid: int | None
    state: str = ABSENT
    stale: bool = False          # live, but with an earlier Wave's bridge build
    playing: bool = False        # any deck
    loaded: bool = False         # any deck has a track
    known: bool = False          # Wave can see the decks (bridge or a fallback reader)
    age: float | None = None     # seconds since this Rekordbox started
    bridge_loaded: bool = False  # Windows: some Wave bridge library is already inside this Rekordbox


class HostPolicy:
    START_GRACE = 75        # seconds a Rekordbox Wave started may take before its bridge reports
    FRESH = 90              # a Rekordbox younger than this has nothing worth keeping
    RELAUNCHES = 3          # crash relaunches allowed ...
    RELAUNCH_WINDOW = 600   # ... within this many seconds

    def __init__(self, windows=sys.platform == 'win32', auto_start=True, auto_connect=True, keep_running=True):
        self.windows = windows
        self.auto_start = auto_start
        self.auto_connect = auto_connect
        self.keep_running = keep_running
        self.first = True
        self.last_pid = None
        self.launched_at = None      # when Wave last started Rekordbox
        self.owned = set()           # pids Wave started or connected
        self.tried_attach = set()
        self.restarted = set()       # pids Wave restarted (once each)
        self.relaunches = []
        self.restarts = []
        self.stale_restarted = False # restarted once to finish an update
        self.quitting = False        # Wave asked Rekordbox to quit
        self.ask = None              # what the DJ is asked to do, when Wave won't act alone

    def starting(self, now):
        return self.launched_at is not None and now - self.launched_at < self.START_GRACE

    def decide(self, seen, now):
        """The next action - LAUNCH, ATTACH, RESTART - or None. Sets ``ask`` when the DJ must act."""
        first, self.first = self.first, False
        if seen.pid is None:
            went_away, self.last_pid = self.last_pid, None
            if self.quitting:
                return None
            if went_away and went_away in self.owned:
                self.relaunches = [t for t in self.relaunches if now - t < self.RELAUNCH_WINDOW]
                if not self.keep_running:
                    self.ask = ASK_START
                    return None
                if len(self.relaunches) >= self.RELAUNCHES:
                    self.ask = ASK_GAVE_UP
                    return None
                log.warning('Rekordbox (pid %s) closed while Wave was using it: starting it again', went_away)
                self.relaunches.append(now)
                return self._launch(now)
            if self.starting(now):
                return None
            if first and self.auto_start:
                return self._launch(now)
            self.ask = ASK_START
            return None
        if seen.pid != self.last_pid:
            self.last_pid = seen.pid
        if seen.state in (LIVE, BUSY, STARTING):
            self.ask = None
            self.owned.add(seen.pid)
            if (seen.state == LIVE and seen.stale and not self.stale_restarted and self._idle(seen) and self.auto_connect
                    and not self.starting(now) and self._may_restart(seen, now)):
                log.info('Rekordbox runs an earlier Wave bridge and is idle: restarting it to finish updating')
                self.stale_restarted = True  # once: if that didn't help, the earlier bridge still works
                return self._restart(seen, now)
            if seen.state == LIVE and seen.stale and not self.stale_restarted:
                self.ask = ASK_UPDATE  # everything works; the newest abilities wait for a restart
            return None
        if seen.state in (UNSUPPORTED, FAILED):
            self.ask = None  # nothing Wave can do; the status says why
            return None
        # NONE (no bridge) or OLDER (an earlier Wave's incompatible bridge)
        if seen.state == NONE and self.starting(now):
            # Wave just started it: the bridge arrives once Rekordbox reads OpenSSL's config. On Windows,
            # a bridge that hasn't come after half a minute is connected live instead.
            if not (self.windows and seen.age is not None and seen.age > 30):
                return None
        if (self.windows and seen.state == NONE and not seen.bridge_loaded and self.auto_connect
                and seen.pid not in self.tried_attach):
            self.tried_attach.add(seen.pid)
            return ATTACH
        if self.auto_connect and self._may_restart(seen, now) and (
                (seen.known and self._idle(seen)) or (seen.age is not None and seen.age < self.FRESH and not seen.loaded)):
            return self._restart(seen, now)
        self.ask = ASK_CONNECT
        return None

    @staticmethod
    def _idle(seen):
        return not seen.playing and not seen.loaded

    def _may_restart(self, seen, now):
        self.restarts = [t for t in self.restarts if now - t < self.RELAUNCH_WINDOW]
        return seen.pid not in self.restarted and len(self.restarts) < self.RELAUNCHES

    def _launch(self, now):
        self.ask = None
        self.launched_at = now
        return LAUNCH

    def _restart(self, seen, now):
        self.ask = None
        self.restarted.add(seen.pid)
        self.restarts.append(now)
        self.launched_at = now
        return RESTART

    def launched(self, pid):
        """Wave started (or connected) this Rekordbox: it keeps it running."""
        if pid:
            self.owned.add(pid)


class RekordboxHost:
    """Carries out HostPolicy's decisions, one at a time, off the UI thread."""

    def __init__(self, policy=None, *, launch=None, attach=None, restart=None, pid=None, hidden=lambda: True):
        from . import mac_bridge
        self.policy = policy or HostPolicy()
        self._launch = launch or (lambda: mac_bridge.launch(hidden=hidden()))
        self._restart = restart or (lambda: mac_bridge.restart(hidden=hidden()))
        self._attach = attach or attach_running
        self._pid = pid or mac_bridge.rekordbox_pid
        self.jobs = ThreadPoolExecutor(max_workers=1, thread_name_prefix='rekordbox-host')
        self.job = None
        self.doing = ''      # what Wave is doing for Rekordbox right now, for the status
        self.error = ''      # the last action's failure

    def busy(self):
        return self.job is not None and not self.job.done()

    def poll(self, seen, now=None):
        """Call a couple of times a second with what Wave sees; returns the action it started, if any."""
        now = time.monotonic() if now is None else now
        if self.job is not None and self.job.done():
            try:
                self.job.result()
                self.error = ''
            except Exception as exc:
                log.warning('%s failed: %s', self.doing, exc)
                self.error = str(exc)
            self.job, self.doing = None, ''
        if self.busy():
            return None
        action = self.policy.decide(seen, now)
        if action:
            self.run(action, seen.pid)
        return action

    def run(self, action, pid=None):
        self.doing = {LAUNCH: 'Starting Rekordbox', ATTACH: 'Connecting to Rekordbox',
                      RESTART: 'Restarting Rekordbox with Wave'}[action]
        log.info('%s (pid %s)', self.doing, pid)

        def work():
            if action == LAUNCH:
                self._launch()
            elif action == RESTART:
                self._restart()
            else:
                connected, message = self._attach(pid)
                log.info('Live connect to Rekordbox %s: %s', pid, message)
                if not connected:
                    raise OSError(message)
            self.policy.launched(self._pid())
        self.job = self.jobs.submit(work)

    def connect_now(self):
        """The DJ pressed CONNECT: start or restart Rekordbox with Wave now."""
        if self.busy():
            return
        self.policy.ask = None
        self.policy.launched_at = time.monotonic()
        self.run(RESTART if self._pid() else LAUNCH)

    def quit(self, playing=False):
        """Wave is closing: quit the Rekordbox it was using, unless a deck plays."""
        pid = self._pid()
        if not pid or playing or pid not in self.policy.owned:
            return False
        self.policy.quitting = True
        log.info('Quitting Rekordbox (pid %s) with Wave', pid)
        try:
            from .mac_bridge import ask_to_quit
            ask_to_quit()
            return True
        except Exception as exc:
            log.warning('Could not quit Rekordbox: %s', exc)
            return False

    def close(self):
        self.jobs.shutdown(wait=False, cancel_futures=True)


def attach_running(pid):
    """Windows: put this Wave's bridge into the running Rekordbox ``pid``."""
    from .mac_bridge import install, bundled_dylib, bridge_dir, bridge_name
    from .win_attach import attach
    install()
    return attach(pid, bridge_dir()/bridge_name(bundled_dylib()))
