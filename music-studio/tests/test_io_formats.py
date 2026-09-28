import os
import shutil
import sys
import tempfile

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from audio.io_formats import load_audio, export_audio

SR = 22050


def _sine(freq=440.0, duration=0.5, sr=SR, amp=0.5):
    t = np.linspace(0, duration, int(sr * duration), endpoint=False)
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def test_wav_roundtrip_needs_no_ffmpeg(tmp_path):
    y = _sine()
    path = str(tmp_path / "out.wav")
    export_audio(path, y, SR)
    loaded, sr = load_audio(path)
    assert sr == SR
    assert loaded.shape[0] == y.shape[0]
    assert np.max(np.abs(loaded - y)) < 1e-3


def test_flac_roundtrip_needs_no_ffmpeg(tmp_path):
    y = _sine()
    path = str(tmp_path / "out.flac")
    export_audio(path, y, SR)
    loaded, sr = load_audio(path)
    assert sr == SR
    assert loaded.shape[0] == y.shape[0]


@pytest.mark.skipif(shutil.which("ffmpeg") is None, reason="ffmpeg not installed")
def test_mp3_roundtrip_via_pydub(tmp_path):
    y = _sine()
    path = str(tmp_path / "out.mp3")
    export_audio(path, y, SR)
    assert os.path.getsize(path) > 0
    loaded, sr = load_audio(path)
    assert sr == SR
    # lossy codec: just check it's roughly the same length/energy, not sample-exact
    assert abs(loaded.shape[0] - y.shape[0]) < SR  # within ~1s (codec padding/delay)
    assert np.abs(loaded).mean() > 0.01
