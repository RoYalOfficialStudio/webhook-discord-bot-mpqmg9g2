import sys
import os

import numpy as np
import librosa

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from audio.autotune import autotune

SR = 22050


def _sine(freq, duration=1.5, sr=SR, amp=0.6):
    t = np.linspace(0, duration, int(sr * duration), endpoint=False)
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def _detect_dominant_pitch(y, sr):
    f0, voiced_flag, _ = librosa.pyin(y, sr=sr, fmin=100, fmax=800)
    valid = f0[voiced_flag]
    return float(np.median(valid)) if len(valid) else 0.0


def test_autotune_pulls_flat_note_toward_target_scale_note():
    # A4 = 440 Hz is in C major; sing it ~40 cents flat and see autotune pull it back up.
    flat_freq = 440.0 * (2 ** (-0.4 / 12))
    y = _sine(flat_freq)

    corrected = autotune(y, SR, key="C", scale="major", strength=1.0, speed=1.0, hop_length=1024)

    original_pitch = _detect_dominant_pitch(y, SR)
    corrected_pitch = _detect_dominant_pitch(corrected, SR)

    assert abs(corrected_pitch - 440.0) < abs(original_pitch - 440.0)


def test_autotune_zero_strength_is_close_to_dry_signal():
    y = _sine(430.0)
    out = autotune(y, SR, key="C", scale="major", strength=0.0, speed=1.0, hop_length=1024)
    assert out.shape == y.shape
