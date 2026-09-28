import os
import sys

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import audio.recorder as recorder_module
from audio.recorder import Recorder, _MonitorRingBuffer


def test_monitor_defaults_off():
    rec = Recorder(samplerate=22050, channels=1)
    assert rec.monitor is False
    assert rec.output_device is None


def test_start_wraps_device_errors_in_clear_runtime_error(monkeypatch):
    class _BrokenInputStream:
        def __init__(self, *a, **k):
            raise OSError("Error querying device -1")

    monkeypatch.setattr(recorder_module.sd, "InputStream", _BrokenInputStream)
    rec = Recorder(samplerate=22050, channels=1)

    with pytest.raises(RuntimeError, match="Could not open the audio device"):
        rec.start()
    assert not rec.is_recording


def test_start_with_monitor_wraps_output_stream_errors_too(monkeypatch):
    class _WorkingInputStream:
        def __init__(self, *a, **k):
            pass

        def start(self):
            pass

        def stop(self):
            pass

        def close(self):
            pass

    class _BrokenOutputStream:
        def __init__(self, *a, **k):
            raise OSError("Illegal combination of I/O devices [PaErrorCode -9993]")

    monkeypatch.setattr(recorder_module.sd, "InputStream", _WorkingInputStream)
    monkeypatch.setattr(recorder_module.sd, "OutputStream", _BrokenOutputStream)
    rec = Recorder(samplerate=22050, channels=1, monitor=True)

    with pytest.raises(RuntimeError, match="headphones"):
        rec.start()
    assert not rec.is_recording


def test_monitor_uses_two_independent_streams_not_one_duplex_stream(monkeypatch):
    # Regression test: a single sd.Stream(channels=(in,out), device=(in_dev,out_dev))
    # frequently fails on Windows with "Illegal combination of I/O devices" when the
    # input and output devices differ. Recording+monitoring must use one InputStream
    # and one separate OutputStream instead, which PortAudio always allows.
    created = []

    class _FakeStream:
        def __init__(self, name, *a, **k):
            created.append(name)

        def start(self):
            pass

        def stop(self):
            pass

        def close(self):
            pass

    monkeypatch.setattr(recorder_module.sd, "InputStream", lambda *a, **k: _FakeStream("input"))
    monkeypatch.setattr(recorder_module.sd, "OutputStream", lambda *a, **k: _FakeStream("output"))

    rec = Recorder(samplerate=22050, channels=1, monitor=True, device=1, output_device=2)
    rec.start()

    assert created == ["input", "output"]
    assert rec.is_recording


def test_callback_forwards_to_monitor_buffer_when_monitoring():
    rec = Recorder(samplerate=22050, channels=1, monitor=True, monitor_gain=0.5)
    rec._monitor_buffer = _MonitorRingBuffer(max_samples=1000)

    indata = np.full((128, 1), 0.4, dtype=np.float32)
    rec._callback(indata, 128, None, None)

    assert rec.level > 0.0
    assert rec._frames_recorded == 128

    outdata = np.zeros((128, 2), dtype=np.float32)
    rec._output_callback(outdata, 128, None, None)
    assert np.allclose(outdata[:, 0], 0.2, atol=1e-6)
    assert np.allclose(outdata[:, 1], 0.2, atol=1e-6)


def test_output_callback_outputs_silence_when_buffer_underruns():
    rec = Recorder(samplerate=22050, channels=1, monitor=True)
    rec._monitor_buffer = _MonitorRingBuffer(max_samples=1000)
    outdata = np.full((64, 2), 1.0, dtype=np.float32)
    rec._output_callback(outdata, 64, None, None)
    assert np.allclose(outdata, 0.0)


class _DummyStream:
    def stop(self):
        pass

    def close(self):
        pass


def test_accumulated_chunks_are_concatenated_on_stop():
    rec = Recorder(samplerate=22050, channels=1)
    rec._recording = True
    rec._stream = _DummyStream()
    for _ in range(3):
        indata = np.full((64, 1), 0.1, dtype=np.float32)
        rec._callback(indata, 64, None, None)
    audio = rec.stop()
    assert audio.shape[0] == 192


def test_monitor_ring_buffer_streams_across_mismatched_block_sizes():
    buf = _MonitorRingBuffer(max_samples=10000)
    buf.write(np.full((100, 1), 0.3, dtype=np.float32))
    buf.write(np.full((50, 1), 0.6, dtype=np.float32))

    first = buf.read(80, channels=2)
    assert np.allclose(first, 0.3)

    second = buf.read(80, channels=2)
    # 20 remaining samples at 0.3, then 50 samples at 0.6, then 10 samples of silence
    assert np.allclose(second[:20], 0.3)
    assert np.allclose(second[20:70], 0.6)
    assert np.allclose(second[70:], 0.0)


def test_monitor_ring_buffer_drops_oldest_when_over_capacity():
    buf = _MonitorRingBuffer(max_samples=100)
    buf.write(np.full((80, 1), 1.0, dtype=np.float32))
    buf.write(np.full((80, 1), 2.0, dtype=np.float32))  # pushes buffered total over 100

    out = buf.read(160, channels=1)
    # the old 1.0-valued samples should have been dropped to stay under capacity
    assert not np.any(out == 1.0)
    assert np.any(out == 2.0)
