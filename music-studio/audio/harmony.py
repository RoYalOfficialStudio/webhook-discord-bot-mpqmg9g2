"""Automatic vocal harmony generator.

Takes a lead vocal and creates one or more harmony voices by shifting each
detected note a fixed number of *scale degrees* (diatonic transposition,
e.g. "a third above") rather than a fixed number of semitones — so the
harmony always stays in key even though the exact semitone interval varies
note to note, just like a real backing singer would follow. This is not
something a plain pitch-correction ("autotune") effect provides; it produces
brand new backing-vocal tracks from the lead.
"""
from __future__ import annotations

import numpy as np
import librosa

from .autotune import _key_to_pitch_class, SCALES, resynthesize_with_pitch_curve

# A few common named intervals, expressed in scale *degrees* (steps within
# the diatonic scale table), not semitones. Positive = above, negative = below.
NAMED_INTERVALS = {
    "third above": 2,
    "fifth above": 4,
    "sixth above": 5,
    "octave above": 7,
    "third below": -2,
    "fifth below": -4,
    "octave below": -7,
}


def _build_scale_table(root_pc: int, scale_intervals: list[int], low: int = 24, high: int = 108) -> list[int]:
    return [m for m in range(low, high) if (m - root_pc) % 12 in scale_intervals]


def _diatonic_shift(midi_float: float, table: list[int], steps: int) -> float:
    if not table:
        return midi_float
    nearest = min(table, key=lambda m: abs(m - midi_float))
    idx = table.index(nearest)
    idx2 = min(max(idx + steps, 0), len(table) - 1)
    return float(table[idx2])


def generate_harmony_voice(
    y: np.ndarray,
    sr: int,
    key: str = "C",
    scale: str = "major",
    steps: int = 2,
    formant_preserve: bool = True,
    hop_length: int = 2048,
    fmin: float = 65.0,
    fmax: float = 1000.0,
) -> np.ndarray:
    """Render one harmony voice, `steps` scale degrees above/below the lead.

    `steps` follows NAMED_INTERVALS (e.g. 2 = "a third above" in a 7-note
    scale). Silence/unvoiced parts of the lead stay silent in the harmony.
    """
    if y.ndim > 1:
        y = np.mean(y, axis=1)
    y = y.astype(np.float32)
    if len(y) == 0:
        return y

    root_pc = _key_to_pitch_class(key)
    scale_intervals = SCALES.get(scale, SCALES["major"])
    table = _build_scale_table(root_pc, scale_intervals)

    f0, voiced_flag, _ = librosa.pyin(
        y, sr=sr, fmin=fmin, fmax=fmax, frame_length=hop_length * 2, hop_length=hop_length
    )
    n_frames = len(f0)
    n_steps = np.zeros(n_frames)
    for i in range(n_frames):
        if voiced_flag[i] and f0[i] and f0[i] > 0:
            midi = librosa.hz_to_midi(f0[i])
            target = _diatonic_shift(midi, table, steps)
            n_steps[i] = target - midi

    return resynthesize_with_pitch_curve(y, sr, n_steps, hop_length, formant_preserve=formant_preserve)


def generate_harmony_voices(
    y: np.ndarray,
    sr: int,
    key: str = "C",
    scale: str = "major",
    intervals: list[int] = (2, -2),
    **kwargs,
) -> list[np.ndarray]:
    """Convenience wrapper: render several harmony voices at once."""
    return [
        generate_harmony_voice(y, sr, key=key, scale=scale, steps=s, **kwargs)
        for s in intervals
    ]
