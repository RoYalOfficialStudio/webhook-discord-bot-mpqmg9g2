import os
import sys

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import audio.recorder as recorder_module
from audio.recorder import Recorder, _MonitorRingBuffer


class _FakeStream:
    """Stand-in for sd.InputStream/sd.OutputStream that records its lifecycle."""

    log: list = []

    def __init__(self, kind, *a, **k):
        self.kind = kind
        self.stopped = False
        _FakeStream.log.append(("open", kind))

    def start(self):
        pass

    def stop(self):
        self.stopped = True

    def close(self):
        _FakeStream.log.append(("close", self.kind))


@pytest.fixture
def fake_streams(monkeypatch):
    _FakeStream.log = []
    monkeypatch.setattr(recorder_module.sd, "InputStream", lambda *a, **k: _FakeStream("input"))
    monkeypatch.setattr(recorder_module.sd, "OutputStream", lambda *a, **k: _FakeStream("output"))
    return _FakeStream.log


def test_monitor_defaults_off():
    rec = Recorder(samplerate=22050, channels=1)
    assert rec.monitor is False
    assert rec.output_device is None
    assert not rec.is_monitoring


def test_start_wraps_device_errors_in_clear_runtime_error(monkeypatch):
    class _BrokenInputStream:
        def __init__(self, *a, **k):
            raise OSError("Error querying device -1")

    monkeypatch.setattr(recorder_module.sd, "InputStream", _BrokenInputStream)
    rec = Recorder(samplerate=22050, channels=1)

    with pytest.raises(RuntimeError, match="Could not open the microphone"):
        rec.start()
    assert not rec.is_recording


def test_start_with_monitor_wraps_output_stream_errors_too(monkeypatch):
    monkeypatch.setattr(recorder_module.sd, "InputStream", lambda *a, **k: _FakeStream("input"))

    class _BrokenOutputStream:
        def __init__(self, *a, **k):
            raise OSError("Illegal combination of I/O devices [PaErrorCode -9993]")

    monkeypatch.setattr(recorder_module.sd, "OutputStream", _BrokenOutputStream)
    rec = Recorder(samplerate=22050, channels=1, monitor=True)

    with pytest.raises(RuntimeError, match="headphones"):
        rec.start()
    assert not rec.is_recording


def test_monitor_uses_two_independent_streams_not_one_duplex_stream(fake_streams):
    # Regression: a single sd.Stream(channels=(in,out), device=(in_dev,out_dev))
    # frequently fails on Windows with "Illegal combination of I/O devices" when
    # the input and output devices differ. Must use one InputStream and one
    # separate OutputStream instead, which PortAudio always allows.
    rec = Recorder(samplerate=22050, channels=1, monitor=True, device=1, output_device=2)
    rec.start()
    assert fake_streams == [("open", "input"), ("open", "output")]
    assert rec.is_recording


def test_monitor_works_without_recording(fake_streams):
    # You should be able to hear yourself (to dial in your mix) without
    # having to start a recording.
    rec = Recorder(samplerate=22050, channels=1)
    rec.set_monitor(True)

    assert rec.is_monitoring
    assert not rec.is_recording
    assert fake_streams == [("open", "input"), ("open", "output")]

    # live input reaches the output, but isn't captured into a take
    rec._input_callback(np.full((64, 1), 0.4, dtype=np.float32), 64, None, None)
    outdata = np.zeros((64, 2), dtype=np.float32)
    rec._output_callback(outdata, 64, None, None)
    assert np.allclose(outdata, 0.4 * rec.monitor_gain, atol=1e-6)
    assert rec._chunks == []
    assert rec.level > 0.0


def test_turning_monitor_off_without_recording_releases_both_streams(fake_streams):
    rec = Recorder(samplerate=22050, channels=1)
    rec.set_monitor(True)
    rec.set_monitor(False)
    assert not rec.is_monitoring
    assert ("close", "input") in fake_streams
    assert ("close", "output") in fake_streams


def test_recording_while_monitoring_reuses_the_same_input_stream(fake_streams):
    rec = Recorder(samplerate=22050, channels=1)
    rec.set_monitor(True)
    rec.start()
    # no second input stream should be opened for the recording
    assert fake_streams.count(("open", "input")) == 1

    rec._input_callback(np.full((32, 1), 0.2, dtype=np.float32), 32, None, None)
    audio = rec.stop()
    assert audio.shape[0] == 32

    # monitoring keeps running after the take is stopped
    assert rec.is_monitoring
    assert ("close", "input") not in fake_streams


def test_turning_monitor_off_mid_recording_keeps_recording(fake_streams):
    rec = Recorder(samplerate=22050, channels=1)
    rec.start()
    rec.set_monitor(True)
    rec.set_monitor(False)

    assert rec.is_recording
    assert ("close", "output") in fake_streams
    assert ("close", "input") not in fake_streams


def test_failed_monitor_rolls_back_and_releases_mic(monkeypatch):
    log = []

    class _Input:
        def __init__(self, *a, **k):
            log.append("open input")

        def start(self):
            pass

        def stop(self):
            pass

        def close(self):
            log.append("close input")

    class _BrokenOutput:
        def __init__(self, *a, **k):
            raise OSError("device busy")

    monkeypatch.setattr(recorder_module.sd, "InputStream", _Input)
    monkeypatch.setattr(recorder_module.sd, "OutputStream", _BrokenOutput)

    rec = Recorder(samplerate=22050, channels=1)
    with pytest.raises(RuntimeError, match="headphones"):
        rec.set_monitor(True)

    assert rec.monitor is False
    assert not rec.is_monitoring
    assert log == ["open input", "close input"]


def test_close_releases_everything(fake_streams):
    rec = Recorder(samplerate=22050, channels=1)
    rec.set_monitor(True)
    rec.start()
    rec.close()
    assert not rec.is_recording
    assert not rec.is_monitoring
    assert ("close", "input") in fake_streams
    assert ("close", "output") in fake_streams


def test_output_callback_outputs_silence_when_buffer_underruns():
    rec = Recorder(samplerate=22050, channels=1)
    rec._monitor_buffer = _MonitorRingBuffer(max_samples=1000)
    outdata = np.full((64, 2), 1.0, dtype=np.float32)
    rec._output_callback(outdata, 64, None, None)
    assert np.allclose(outdata, 0.0)


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
    assert not np.any(out == 1.0)
    assert np.any(out == 2.0)
