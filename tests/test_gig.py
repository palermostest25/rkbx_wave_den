"""One Wave at a time, and a log of what happened."""
import logging
import tempfile
import unittest
from pathlib import Path

from PySide6.QtWidgets import QApplication
from PySide6.QtTest import QTest

from rb_waveform_core.gig import KeepAwake, SingleInstance, setup_logging


class GigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def test_a_second_wave_brings_the_first_forward_instead(self):
        first = SingleInstance('wave-test-instance')
        self.assertTrue(first.primary)
        shown = []
        first.listen(lambda: shown.append(1))
        second = SingleInstance('wave-test-instance')
        self.assertFalse(second.primary)
        for _ in range(20):
            QTest.qWait(25)
            if shown:
                break
        self.assertEqual(shown, [1])
        first.server.close()

    def test_errors_land_in_the_log(self):
        with tempfile.TemporaryDirectory() as folder:
            path = setup_logging(Path(folder))
            try:
                logging.getLogger('rb_waveform_core.test').warning('deck 2 hiccup')
                for handler in logging.getLogger().handlers:
                    handler.flush()
                self.assertIn('deck 2 hiccup', path.read_text())
            finally:
                import faulthandler, sys, threading
                from rb_waveform_core import gig
                faulthandler.disable()
                if gig._fault_file:
                    gig._fault_file.close()
                    gig._fault_file = None
                sys.excepthook, threading.excepthook = sys.__excepthook__, threading.__excepthook__
                for handler in list(logging.getLogger().handlers):
                    if getattr(handler, 'wave_log', False):
                        logging.getLogger().removeHandler(handler)
                        handler.close()

    def test_keep_awake_is_safe_everywhere(self):
        awake = KeepAwake()
        awake.set(True)
        awake.set(False)


if __name__ == '__main__':
    unittest.main()
