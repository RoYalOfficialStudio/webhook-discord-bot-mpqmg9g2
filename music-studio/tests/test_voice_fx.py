import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from audio.effects import apply_doubler, apply_deesser
from audio.harmony import generate_harmony_voice, generate_harmony_voices
from audio.recorder import Recorder

SR = 22050


def _sine(freq=440.0, duration=1.0, sr=SR, amp=0.5):
    t = np.linspace(0, duration, int(sr * duration), endpoint=False)
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def _detect_dominant_pitch(y, sr):
    import librosa
    f0, voiced_flag, _ = librosa.pyin(y, sr=sr, fmin=100, fmax=1200)
    valid = f0[voiced_flag]
    return float(np.median(valid)) if len(valid) else 0.0


def test_doubler_produces_stereo_output_same_length():
    y = _sine(300.0, duration=0.5)
    out = apply_doubler(y, SR, voices=2, detune_cents=15.0, delay_ms=18.0, mix=0.5)
    assert out.shape == (len(y), 2)
    assert np.max(np.abs(out)) > 0


def test_doubler_dry_mix_zero_is_original_mono_doubled_to_stereo():
    y = _sine(300.0, duration=0.3)
    out = apply_doubler(y, SR, mix=0.0)
    assert np.allclose(out[:, 0], y, atol=1e-4)
    assert np.allclose(out[:, 1], y, atol=1e-4)


def test_deesser_reduces_energy_in_sibilant_band_when_hot():
    # A loud tone right in the sibilant band should get ducked.
    y = _sine(7000.0, duration=0.3, amp=0.9)
    out = apply_deesser(y, SR, freq=7000.0, bandwidth=2000.0, threshold_db=-30.0, ratio=8.0)
    assert out.shape == y.shape
    assert np.sqrt(np.mean(out ** 2)) < np.sqrt(np.mean(y ** 2))


def test_deesser_leaves_low_frequency_content_alone():
    y = _sine(200.0, duration=0.3, amp=0.8)
    out = apply_deesser(y, SR, freq=7000.0, bandwidth=2000.0, threshold_db=-30.0, ratio=8.0)
    assert np.max(np.abs(out - y)) < 0.05


def test_harmony_third_above_is_higher_than_lead():
    y = _sine(300.0, duration=1.5)  # ~D4, safely inside C major
    harmony = generate_harmony_voice(y, SR, key="C", scale="major", steps=2, hop_length=1024)
    lead_pitch = _detect_dominant_pitch(y, SR)
    harmony_pitch = _detect_dominant_pitch(harmony, SR)
    assert harmony.shape == y.shape
    assert harmony_pitch > lead_pitch


def test_harmony_third_below_is_lower_than_lead():
    y = _sine(300.0, duration=1.5)
    harmony = generate_harmony_voice(y, SR, key="C", scale="major", steps=-2, hop_length=1024)
    lead_pitch = _detect_dominant_pitch(y, SR)
    harmony_pitch = _detect_dominant_pitch(harmony, SR)
    assert harmony_pitch < lead_pitch


def test_generate_harmony_voices_returns_one_per_interval():
    y = _sine(300.0, duration=1.0)
    voices = generate_harmony_voices(y, SR, key="C", scale="major", intervals=[2, -2], hop_length=1024)
    assert len(voices) == 2
    for v in voices:
        assert v.shape == y.shape


def test_recorder_exposes_level_and_elapsed_time_after_stop():
    rec = Recorder(samplerate=SR, channels=1)
    assert rec.level == 0.0
    assert not rec.is_recording
    # Simulate a completed recording without touching real hardware.
    rec._frames_recorded = SR  # 1 second worth
    assert abs(rec.elapsed_seconds - 1.0) < 1e-6
