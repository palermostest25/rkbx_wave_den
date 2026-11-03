import os
import struct
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from rb_waveform_core import mac_bridge as mb


def state(seq=2, version=mb.VERSION, status=mb.LIVE, master=1, decks=()):
    """A shared-state image as the bridge writes it."""
    buffer = bytearray(mb.SIZE)
    mb.HEADER.pack_into(buffer, 0, mb.MAGIC, version, seq, 4242, status, len(decks), master, 7, 123, b'note')
    for index, (flags, position, loop, path) in enumerate(decks):
        mb.DECK.pack_into(buffer, mb.DECKS_AT + index * mb.DECK.size, flags, 300_000, position, 12800.0, 12500.0,
                          2.4, 1, *(loop or (-1, -1)), 2, 0, path.encode())  # key shift 2, tempo range 0
    return buffer


class BridgeLayoutTests(unittest.TestCase):
    def test_reads_decks_in_seconds_with_loop_and_status(self):
        snap = mb.parse(state(decks=[(mb.LOADED | mb.PLAYING | mb.LOOPING | mb.SYNC, 4721.0, (4430, 6234), '/a.m4a'),
                                     (mb.LOADED | mb.MASTER_TEMPO, 1000.0, None, '/b.m4a')]))
        first, second = snap.decks
        self.assertEqual((snap.pid, snap.status, snap.ticks), (4242, mb.LIVE, 7))
        self.assertAlmostEqual(first.position, 4.721)
        self.assertEqual((first.bpm, first.original_bpm, first.duration), (128.0, 125.0, 300.0))
        self.assertEqual(first.loop, (4.43, 6.234))
        self.assertTrue(first.playing and first.sync and not first.master)
        self.assertTrue(second.master and second.master_tempo and second.loop is None)
        self.assertEqual((first.key_shift, first.path), (2, '/a.m4a'))

    def test_mounted_sticks_and_tempo_range_are_read(self):
        buffer = state(decks=[(mb.LOADED, 0.0, None, '/a.m4a')])
        mb.DEVICE.pack_into(buffer, mb.DEVICES_AT + mb.DEVICE.size, 77, b'/Volumes/GIG')
        snap = mb.parse(buffer)
        self.assertEqual(snap.devices, ((77, '/Volumes/GIG'),))
        self.assertEqual(snap.decks[0].tempo_range, 0)  # state() packs 0, which names no range
        self.assertNotIn(0, mb.TEMPO_RANGES)
        self.assertEqual((mb.TEMPO_RANGES[10], mb.TEMPO_RANGES[100]), ('±10', 'WIDE'))

    def test_torn_read_is_retried_not_misread(self):
        self.assertIsNone(mb.parse(state(seq=3)))

    def test_older_bridge_is_reported_not_parsed(self):
        snap = mb.parse(state(version=mb.VERSION - 1))
        self.assertEqual((snap.status, snap.decks), (mb.UNSUPPORTED, ()))
        self.assertIn('older', snap.note)

    def test_liveness_needs_this_rekordbox_and_a_fresh_heartbeat(self):
        snap = mb.parse(state())
        self.assertTrue(mb.live(snap, 4242, now=snap.uptime_ns + 10))
        self.assertFalse(mb.live(snap, 99, now=snap.uptime_ns + 10))
        self.assertFalse(mb.live(snap, 4242, now=snap.uptime_ns + 2_000_000_000))


class BridgeCommandTests(unittest.TestCase):
    def test_load_request_is_checksummed_and_answered(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'state.bin'
            path.write_bytes(bytes(state()))
            os.chmod(path, 0o600)
            bridge = mb.Bridge(path)

            def rekordbox():  # answer the way the bridge does, after checking the checksum
                for _ in range(200):
                    command = bytes(bridge.map[mb.COMMAND_AT:mb.COMMAND_AT + 40])
                    seq, kind, deck, content, issued, device = mb.COMMAND.unpack_from(command)
                    if seq and struct.unpack_from('<Q', command, 32)[0] == mb.fnv(command[:32]):
                        mb.REPLY.pack_into(bridge.map, mb.REPLY_AT, seq, mb.REQUESTED, f'deck {deck} {content}'.encode())
                        return
                    time.sleep(.005)
            answer = threading.Thread(target=rekordbox)
            answer.start()
            self.assertEqual(bridge.request_load(2, 174338329), (True, 'deck 2 174338329'))
            answer.join()
            self.assertEqual(bridge.request_load(0, 2**32)[0], False)  # not a collection id
            bridge.close()

    def test_short_file_from_an_older_bridge_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'state.bin'
            path.write_bytes(bytes(state(version=mb.VERSION - 1))[:4664])
            os.chmod(path, 0o600)
            with self.assertRaises(mb.OlderBridge):
                mb.Bridge(path)

    def test_unprivate_state_file_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'state.bin'
            path.write_bytes(bytes(state()))
            os.chmod(path, 0o644)
            with self.assertRaises(OSError):
                mb.Bridge(path)


class DeviceDiscoveryTests(unittest.TestCase):
    def test_network_shares_are_never_probed(self):
        from rb_waveform_core import library
        parts = [SimpleNamespace(mountpoint='/Volumes/Pond', fstype='smbfs', opts='rw'),
                 SimpleNamespace(mountpoint='/Volumes/Share', fstype='webdav', opts='rw'),
                 SimpleNamespace(mountpoint='/Volumes/Remote', fstype='apfs', opts='rw,remote')]
        probed = []
        with patch('psutil.disk_partitions', return_value=parts), \
                patch.object(Path, 'is_file', lambda self: probed.append(self) or False):
            self.assertEqual(library.device_library_paths(), [])
        self.assertEqual(probed, [])


class LoadFlowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def session(self, directory, answer=(True, 'Load requested on deck 2')):
        from rb_waveform_core.qt_app import Session
        session = Session(demo=True, config_path=Path(directory) / 's.json')
        session.reader = SimpleNamespace(bridged=True, RELAUNCH='relaunch', deck_paths={},
                                         load=lambda deck, ident, root=None: answer, stop=lambda: None)
        messages = []
        session.loadStatus.connect(messages.append)
        return session, messages

    def track(self):
        from rb_waveform_core.catalog import CatalogTrack
        return CatalogTrack('174338329', 'Still Riding', 'Barry', 133, '3A', 217, '/Music/Still Riding.m4a')

    def wait(self, session, messages, until):
        deadline = time.monotonic() + 3
        while not any(until in m for m in messages) and time.monotonic() < deadline:
            session.poll_loading(time.monotonic())
            time.sleep(.01)

    def test_load_is_confirmed_only_by_the_engine_file(self):
        with tempfile.TemporaryDirectory() as directory:
            session, messages = self.session(directory)
            self.assertTrue(session.load_track(1, self.track()))
            session.poll_loading(time.monotonic())
            self.assertFalse(any(m.startswith('Loaded') for m in messages))  # accepted is not loaded
            session.reader.deck_paths = {1: '/music/still riding.m4a'}  # the engine, case aside
            self.wait(session, messages, 'Loaded')
            self.assertIn('Loaded Still Riding on deck 2', messages)
            session.close()

    def test_usb_track_is_confirmed_by_its_mounted_path(self):
        from rb_waveform_core.catalog import CatalogTrack
        with tempfile.TemporaryDirectory() as directory:
            session, messages = self.session(directory)
            asked = []
            session.reader.load = lambda deck, ident, root=None: asked.append((deck, ident, root)) or (True, 'ok')
            track = CatalogTrack('7', 'Stick Song', 'A', 128, '8A', 200, '/Contents/a/b.mp3')
            self.assertTrue(session.load_track(2, track, '/Volumes/GIG/PIONEER/rekordbox/exportLibrary.db'))
            session.reader.deck_paths = {2: '/Volumes/GIG/Contents/a/b.mp3'}
            self.wait(session, messages, 'Loaded')
            self.assertEqual(asked, [(2, 7, '/Volumes/GIG')])
            self.assertIn('Loaded Stick Song on deck 3', messages)
            session.close()

    def test_playing_deck_and_refusals_never_reach_rekordbox(self):
        with tempfile.TemporaryDirectory() as directory:
            session, messages = self.session(directory, answer=(False, 'Deck 2 is playing'))
            session.timelines[0].playing = True
            self.assertFalse(session.load_track(0, self.track()))
            self.assertIn('playing', messages[-1])
            self.assertTrue(session.load_track(1, self.track()))
            self.wait(session, messages, 'Deck 2: ')
            self.assertEqual(messages[-1], 'Deck 2: Deck 2 is playing')
            session.reader.bridged = False
            self.assertFalse(session.load_track(1, self.track()))
            self.assertEqual(messages[-1], 'relaunch')
            session.close()


if __name__ == '__main__':
    unittest.main()
