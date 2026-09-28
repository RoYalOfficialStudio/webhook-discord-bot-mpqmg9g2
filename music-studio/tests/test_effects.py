import numpy as np
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from audio.effects import ParametricEQ, EQBand, apply_delay, apply_reverb, apply_compressor, apply_limiter


SR = 44100


def _sine(freq=440.0, duration=1.0, sr=SR, amp=0.5):
    t = np.linspace(0, duration, int(sr * duration), endpoint=False)
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def test_eq_boost_increases_energy_near_target_band():
    y = _sine(1000.0)
    eq = ParametricEQ(bands=[EQBand(freq=1000.0, gain_db=12.0, q=1.0, kind="peak")])
    out = eq.process(y, SR)
    assert out.shape == y.shape
    assert np.sqrt(np.mean(out ** 2)) > np.sqrt(np.mean(y ** 2))


def test_eq_no_gain_is_near_passthrough():
    y = _sine(1000.0)
    eq = ParametricEQ(bands=[EQBand(freq=1000.0, gain_db=0.0, kind="peak")])
    out = eq.process(y, SR)
    assert np.allclose(out, y, atol=1e-5)


def test_delay_adds_energy_after_original_signal_ends():
    y = np.zeros(SR, dtype=np.float32)
    y[:100] = 1.0  # a short impulse-like burst at the very start
    out = apply_delay(y, SR, delay_ms=200.0, feedback=0.5, mix=0.8)
    delay_samples = int(SR * 0.2)
    assert np.abs(out[delay_samples:delay_samples + 100]).sum() > 0


def test_reverb_output_has_same_length_and_is_bounded():
    y = _sine(440.0, duration=0.5)
    out = apply_reverb(y, SR, room_size=0.6, damping=0.4, wet=0.4)
    assert out.shape == y.shape
    assert np.max(np.abs(out)) < 10.0  # no blow-up / instability


def test_compressor_reduces_peak_level_above_threshold():
    y = _sine(440.0, amp=0.9)
    out = apply_compressor(y, SR, threshold_db=-20.0, ratio=8.0, attack_ms=1.0, release_ms=50.0)
    assert np.max(np.abs(out)) <= np.max(np.abs(y)) + 1e-6


def test_limiter_never_exceeds_ceiling():
    y = _sine(440.0, amp=3.0)  # intentionally clipping-hot signal
    out = apply_limiter(y, ceiling_db=-1.0)
    ceiling = 10 ** (-1.0 / 20)
    assert np.max(np.abs(out)) <= ceiling + 1e-6
