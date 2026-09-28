"""Autotune / pitch-correction effect.

Pipeline: detect pitch per analysis frame (librosa's pYIN), snap each detected
pitch to the nearest note of the chosen key/scale, smooth the correction curve
over time ("retune speed"), then resynthesize by pitch-shifting overlapping
frames and reassembling them with a Hann-windowed overlap-add.

This is a from-scratch approximation of what commercial pitch-correction
plugins do (they typically use PSOLA); it trades a bit of audio quality for
staying dependency-light (numpy/scipy/librosa only, fully offline).
"""
from __future__ import annotations

import numpy as np
import librosa

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

SCALES = {
    "chromatic": list(range(12)),
    "major": [0, 2, 4, 5, 7, 9, 11],
    "minor": [0, 2, 3, 5, 7, 8, 10],
}


def _key_to_pitch_class(key: str) -> int:
    key = key.strip().upper().replace("♯", "#").replace("B", "B")
    for i, name in enumerate(NOTE_NAMES):
        if name == key:
            return i
    raise ValueError(f"Unknown key: {key!r}. Use one of {NOTE_NAMES}")


def _nearest_scale_midi(midi_float: float, root_pc: int, scale_intervals: list[int]) -> float:
    base = int(round(midi_float))
    candidates = [
        m for m in range(base - 12, base + 13)
        if (m - root_pc) % 12 in scale_intervals
    ]
    if not candidates:
        return midi_float
    return float(min(candidates, key=lambda m: abs(m - midi_float)))


def autotune(
    y: np.ndarray,
    sr: int,
    key: str = "C",
    scale: str = "major",
    strength: float = 1.0,
    speed: float = 0.35,
    hop_length: int = 2048,
    fmin: float = 65.0,
    fmax: float = 1000.0,
) -> np.ndarray:
    """Correct the pitch of a monophonic vocal/instrument signal.

    strength: 0..1, how much of the correction to apply (0 = dry, 1 = full snap).
    speed: 0..1, how fast the correction follows the target pitch
           (low = natural glide, high = robotic/"T-Pain" snap).
    """
    if y.ndim > 1:
        y = np.mean(y, axis=1)
    y = y.astype(np.float32)
    if len(y) == 0:
        return y

    root_pc = _key_to_pitch_class(key)
    scale_intervals = SCALES.get(scale, SCALES["major"])

    f0, voiced_flag, _ = librosa.pyin(
        y, sr=sr, fmin=fmin, fmax=fmax, frame_length=hop_length * 2, hop_length=hop_length
    )

    n_frames = len(f0)
    target_midi = np.full(n_frames, np.nan)
    original_midi = np.full(n_frames, np.nan)
    for i in range(n_frames):
        if voiced_flag[i] and f0[i] and f0[i] > 0:
            midi = librosa.hz_to_midi(f0[i])
            original_midi[i] = midi
            target_midi[i] = _nearest_scale_midi(midi, root_pc, scale_intervals)

    # Exponential smoothing across time to emulate a "retune speed" control.
    smoothed = np.copy(target_midi)
    last_valid = None
    speed = float(np.clip(speed, 0.01, 1.0))
    for i in range(n_frames):
        if np.isnan(target_midi[i]):
            last_valid = None
            continue
        if last_valid is None:
            smoothed[i] = target_midi[i]
        else:
            smoothed[i] = last_valid + speed * (target_midi[i] - last_valid)
        last_valid = smoothed[i]

    n_steps = np.zeros(n_frames)
    for i in range(n_frames):
        if np.isnan(target_midi[i]):
            continue
        corrected_midi = original_midi[i] + strength * (smoothed[i] - original_midi[i])
        n_steps[i] = corrected_midi - original_midi[i]

    frame_length = hop_length * 2
    window = np.hanning(frame_length).astype(np.float32)
    padded = np.pad(y, (frame_length // 2, frame_length))
    out = np.zeros(len(padded), dtype=np.float64)
    norm = np.zeros(len(padded), dtype=np.float64)

    for i in range(n_frames):
        start = i * hop_length
        end = start + frame_length
        if end > len(padded):
            break
        chunk = padded[start:end]
        if abs(n_steps[i]) > 1e-3:
            chunk = librosa.effects.pitch_shift(chunk, sr=sr, n_steps=n_steps[i])
        windowed = chunk * window
        out[start:end] += windowed
        norm[start:end] += window

    norm[norm < 1e-8] = 1.0
    out = out / norm
    out = out[frame_length // 2: frame_length // 2 + len(y)]
    return out.astype(np.float32)
