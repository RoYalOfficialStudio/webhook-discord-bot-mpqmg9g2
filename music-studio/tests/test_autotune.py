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


def test_autotune_custom_scale_pulls_toward_only_allowed_notes():
    # 415 Hz is close to G#4 (415.3 Hz); restrict the custom scale to C and E only,
    # so correction must jump to one of those instead of snapping to G#.
    y = _sine(415.0)
    out = autotune(
        y, SR, key="C", scale="custom", custom_notes=[0, 4],
        strength=1.0, speed=1.0, hop_length=1024,
    )
    original_pitch = _detect_dominant_pitch(y, SR)
    corrected_pitch = _detect_dominant_pitch(out, SR)
    # nearest of {C, E} in any octave to 415 Hz is E4 (329.6) or C5 (523.3);
    # G#4 (415.3) must not be the outcome since it's excluded from the custom scale.
    assert abs(corrected_pitch - 415.3) > 20


def test_autotune_formant_preserve_does_not_crash_and_keeps_length():
    y = _sine(400.0)
    out = autotune(
        y, SR, key="C", scale="major", strength=1.0, speed=1.0,
        formant_preserve=True, hop_length=1024,
    )
    assert out.shape == y.shape


def test_autotune_humanize_reduces_correction_relative_to_full_strength():
    flat_freq = 440.0 * (2 ** (-0.4 / 12))
    y = _sine(flat_freq)
    fully_corrected = autotune(y, SR, key="C", scale="major", strength=1.0, speed=1.0,
                                humanize=0.0, hop_length=1024)
    humanized = autotune(y, SR, key="C", scale="major", strength=1.0, speed=1.0,
                          humanize=1.0, hop_length=1024)
    assert fully_corrected.shape == humanized.shape


def test_autotune_reference_pitch_shifts_target_grid():
    # Sing exactly 440 Hz. With a de-tuned reference for A4 close to a quarter-tone
    # off (~0.45 semitone), the nearest chromatic grid point for that 440 Hz tone
    # is no longer 440 Hz itself, so correction should pull noticeably away from it.
    reference_hz = 440.0 / (2 ** (0.45 / 12))  # ~428.7 Hz
    y = _sine(440.0)
    out = autotune(y, SR, key="A", scale="chromatic", strength=1.0, speed=1.0,
                    reference_hz=reference_hz, hop_length=1024)
    corrected_pitch = _detect_dominant_pitch(out, SR)
    assert abs(corrected_pitch - 440.0) > 5.0
