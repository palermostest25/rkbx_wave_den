"""Normal Rekordbox playback via read-only macOS Accessibility and library queries."""
from __future__ import annotations
from dataclasses import dataclass, replace
from pathlib import Path
from queue import Queue, Empty
import re
import threading
import time
from .rkbx_link_listener import DeckState

CLOCK = re.compile(r'^-?\d+:\d{2}(?::\d{2})?$')


def seconds(text):
    sign = -1 if text.startswith('-') else 1
    value = 0.0
    for part in text.lstrip('-').split(':'):
        value = value*60+float(part)
    return sign*value


def edge_stamp(previous, position, read_at, max_gap=.3):
    """The UI clock shows tenths; when it ticks between two reads, that tick is
    the exact moment the deck crossed ``position``. Stamp the midpoint of the
    reads rather than the late read time, which would add up to a poll of jitter."""
    if previous and position != previous[0] and 0 < read_at - previous[1] < max_gap:
        return (previous[1] + read_at) / 2
    return read_at


@dataclass
class DeckFields:
    deck: int
    title: object
    artist: object
    bpm: object
    remaining: object
    remaining_fraction: object
    elapsed: object
    elapsed_fraction: object


def deck_fields(nodes):
    result=[]
    for k,node in enumerate(nodes):
        if not node.help.startswith('Key:') or k<4:
            continue
        end=next((j for j in range(k+1,len(nodes)) if nodes[j].help.startswith('Sync Master:')),None)
        if end is None:
            continue
        clocks=[j for j in range(k+1,end) if CLOCK.fullmatch(nodes[j].value)]
        if len(clocks)!=2 or any(j+1>=end or not nodes[j+1].value.startswith('.') for j in clocks):
            continue
        before=nodes[k-4:k]
        if before[1].role!='AXImage':
            continue
        a,b=clocks
        result.append(DeckFields(len(result),before[0],before[2],before[3],nodes[a],nodes[a+1],nodes[b],nodes[b+1]))
    return result


class RekordboxAccessibilityReader:
    def __init__(self,deck_count=4,poll_hz=60):
        self.deck_count=deck_count
        self.interval=1/max(1,poll_hz)
        self.connected=False
        self.status='Waiting for Rekordbox'
        self._queue=Queue()
        self._stop_event=threading.Event()
        self._thread=None

    def start(self):
        if self._thread is None:
            self._stop_event.clear()
            self._thread=threading.Thread(target=self._run,name='rekordbox-accessibility',daemon=True)
            self._thread.start()

    def stop(self):
        self._stop_event.set()
        if self._thread:
            self._thread.join(timeout=2.5)
            self._thread=None
        self.connected=False

    def get_event(self,timeout=None):
        try:return self._queue.get(timeout=timeout)
        except Empty:return None

    @staticmethod
    def _find_process():
        import psutil
        for p in psutil.process_iter(['name','exe']):
            exe=p.info.get('exe') or ''
            if p.info['name']=='rekordbox' and exe.startswith('/Applications/'):
                return p.pid
        return None

    @staticmethod
    def _clock(ax,whole,fraction):
        for _ in range(2):
            text=ax.text(whole.element)
            decimal=ax.text(fraction.element)
            if text==ax.text(whole.element):
                if not CLOCK.fullmatch(text) or not re.fullmatch(r'\.\d+',decimal):
                    raise ValueError('Rekordbox time fields changed; rescanning')
                return seconds(text)+(float(decimal) if not text.startswith('-') else -float(decimal))
        raise ValueError('Rekordbox time changed while reading')

    def _run(self):
        from .mac_ax import Accessibility
        from .library import ReadOnlyLibrary
        ax=None; library=None
        states={i:DeckState(i) for i in range(self.deck_count)}
        names={}; bindings=[]; pid=None; next_scan=0; prompted=False
        next_meta=0; loaded={}; last_read={}
        try:
            ax=Accessibility()
            while not self._stop_event.is_set():
                started=time.monotonic()
                try:
                    if not ax.trusted():
                        if not prompted:
                            ax.request_permission();prompted=True
                        self.connected=False
                        self.status='Allow Rekordbox Wave in System Settings → Privacy & Security → Accessibility'
                        self._stop_event.wait(1)
                        continue
                    if started>=next_scan:
                        next_scan=started+3
                        target=self._find_process()
                        if target!=pid:
                            pid=target;bindings=[];names.clear()
                        if pid is None:
                            next_scan=0
                            self.connected=False;self.status='Waiting for normal Rekordbox in /Applications'
                            self._stop_event.wait(1);continue
                    # A full tree walk is thousands of AX calls and stalls playback reads, so
                    # it runs only when bindings are missing; stale bindings fail to read and land here.
                    if not bindings:
                        bindings=deck_fields(ax.snapshot(pid))
                        loaded.clear();last_read.clear();next_meta=0
                    if not bindings:
                        self.connected=False
                        self.status='Show Rekordbox PERFORMANCE decks with elapsed-time fields'
                        self._stop_event.wait(.5);continue
                    self.connected=True
                    self.status='REKORDBOX · Accessibility connected'
                    # Each AX read is a round trip into Rekordbox: only the elapsed clock
                    # is read every tick; titles, BPM and duration twice a second.
                    meta=started>=next_meta
                    if meta:next_meta=started+.5
                    for f in bindings[:self.deck_count]:
                        state=states[f.deck]
                        if not meta and not loaded.get(f.deck):
                            continue
                        pos=self._clock(ax,f.elapsed,f.elapsed_fraction)
                        read_at=time.time()
                        stamp=edge_stamp(last_read.get(f.deck),pos,read_at)
                        last_read[f.deck]=(pos,read_at)
                        if not meta:
                            state.time_seconds=pos
                            state.last_update=read_at
                            self._queue.put(state.snapshot('accessibility',when=stamp))
                            continue
                        title=ax.text(f.title.element).strip()
                        artist=ax.text(f.artist.element).strip()
                        remaining=self._clock(ax,f.remaining,f.remaining_fraction)
                        loaded[f.deck]=bool(title and title!='Not Loaded.')
                        try:bpm=float(ax.text(f.bpm.element))
                        except ValueError:bpm=None
                        identity=(title,artist,round(pos+abs(remaining)))
                        if identity!=names.get(f.deck):
                            names[f.deck]=identity
                            state.anlz_path=None
                            missing=None
                            if title and title!='Not Loaded.':
                                if library is None:library=ReadOnlyLibrary()
                                track=library.resolve(title,artist,pos+abs(remaining))
                                if track:
                                    state.anlz_path=str(track.analysis_folder)
                                else:
                                    missing='No unique analysis match for '+title
                            state.time_seconds=None
                            self._queue.put(state.snapshot('anlz_path'))
                            if missing:
                                self._queue.put(replace(state.snapshot('status'),error=missing))
                        state.time_seconds=pos
                        state.current_bpm=bpm
                        state.last_update=read_at
                        self._queue.put(state.snapshot('accessibility',when=stamp))
                except Exception as exc:
                    self.connected=False;self.status=str(exc)
                    bindings=[];next_scan=0;names.clear();loaded.clear();last_read.clear()
                    self._stop_event.wait(.5)
                self._stop_event.wait(max(0,self.interval-(time.monotonic()-started)))
        except Exception as exc:
            self.connected=False;self.status='Mac connection failed: '+str(exc)
        finally:
            if ax:ax.close()
            if library:library.close()
            self.connected=False
