import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from audio.recorder import Recorder


def test_monitor_defaults_off():
    rec = Recorder(samplerate=22050, channels=1)
    assert rec.monitor is False


def test_monitor_callback_writes_stereo_passthrough_from_mono_input():
    rec = Recorder(samplerate=22050, channels=1, monitor=True, monitor_gain=0.5)
    indata = np.full((128, 1), 0.4, dtype=np.float32)
    outdata = np.zeros((128, 2), dtype=np.float32)

    rec._monitor_callback(indata, outdata, 128, None, None)

    assert np.allclose(outdata[:, 0], 0.2, atol=1e-6)
    assert np.allclose(outdata[:, 1], 0.2, atol=1e-6)
    assert rec.level > 0.0
    assert rec._frames_recorded == 128


class _DummyStream:
    def stop(self):
        pass

    def close(self):
        pass


def test_monitor_callback_accumulates_chunks_for_stop():
    rec = Recorder(samplerate=22050, channels=1, monitor=True)
    rec._recording = True
    rec._stream = _DummyStream()
    for _ in range(3):
        indata = np.full((64, 1), 0.1, dtype=np.float32)
        outdata = np.zeros((64, 2), dtype=np.float32)
        rec._monitor_callback(indata, outdata, 64, None, None)
    audio = rec.stop()
    assert audio.shape[0] == 192
