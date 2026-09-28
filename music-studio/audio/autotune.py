"""Autotune / pitch-correction effect.

Pipeline: detect pitch per analysis frame (librosa's pYIN), snap each detected
pitch to the nearest note of the chosen key/scale (or a custom note set),
smooth the correction curve over time ("retune speed"), optionally back off
correction on natural vibrato ("humanize") and add synthesized vibrato back
in, then resynthesize by pitch-shifting overlapping frames and reassembling
them with a Hann-windowed overlap-add. An optional formant-correction pass
(cepstral envelope compensation) keeps the timbre from sounding "chipmunked"
on larger shifts.

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
    "harmonic_minor": [0, 2, 3, 5, 7, 8, 11],
    "melodic_minor": [0, 2, 3, 5, 7, 9, 11],
    "major_pentatonic": [0, 2, 4, 7, 9],
    "minor_pentatonic": [0, 3, 5, 7, 10],
    "blues": [0, 3, 5, 6, 7, 10],
    "dorian": [0, 2, 3, 5, 7, 9, 10],
    "phrygian": [0, 1, 3, 5, 7, 8, 10],
    "lydian": [0, 2, 4, 6, 7, 9, 11],
    "mixolydian": [0, 2, 4, 5, 7, 9, 10],
    "locrian": [0, 1, 3, 5, 6, 8, 10],
}


def _key_to_pitch_class(key: str) -> int:
    key = key.strip().upper().replace("♯", "#")
    for i, name in enumerate(NOTE_NAMES):
        if name == key:
            return i
    raise ValueError(f"Unknown key: {key!r}. Use one of {NOTE_NAMES}")


def _nearest_scale_midi(midi_float: float, root_pc: int, scale_intervals: list[int]) -> float:
    if not scale_intervals:
        return midi_float
    base = int(round(midi_float))
    candidates = [
        m for m in range(base - 12, base + 13)
        if (m - root_pc) % 12 in scale_intervals
    ]
    if not candidates:
        return midi_float
    return float(min(candidates, key=lambda m: abs(m - midi_float)))


def _cepstral_envelope(magnitude: np.ndarray, n_fft: int) -> np.ndarray:
    """Smooth log-magnitude spectral envelope via cepstral liftering (formant shape)."""
    cutoff = max(8, n_fft // 100)
    log_mag = np.log(np.maximum(magnitude, 1e-8))
    cepstrum = np.fft.irfft(log_mag, n=n_fft)
    lifter = np.zeros(n_fft)
    lifter[:cutoff] = 1.0
    lifter[-cutoff:] = 1.0
    smooth_log_mag = np.fft.rfft(cepstrum * lifter, n=n_fft).real
    return np.exp(smooth_log_mag)


def _correct_formants(shifted_chunk: np.ndarray, original_chunk: np.ndarray, n_fft: int) -> np.ndarray:
    """Re-impose the original chunk's spectral envelope onto the pitch-shifted one."""
    orig_spec = np.fft.rfft(original_chunk, n=n_fft)
    shift_spec = np.fft.rfft(shifted_chunk, n=n_fft)
    orig_env = _cepstral_envelope(np.abs(orig_spec), n_fft)
    shift_env = _cepstral_envelope(np.abs(shift_spec), n_fft)
    ratio = np.clip(orig_env / np.maximum(shift_env, 1e-8), 0.25, 4.0)
    corrected_spec = shift_spec * ratio
    return np.fft.irfft(corrected_spec, n=n_fft)[: len(shifted_chunk)]


def resynthesize_with_pitch_curve(
    y: np.ndarray,
    sr: int,
    n_steps: np.ndarray,
    hop_length: int,
    formant_preserve: bool = False,
) -> np.ndarray:
    """Pitch-shift `y` frame-by-frame according to a per-frame semitone curve
    (one value of `n_steps` per hop_length-spaced analysis frame) and
    reassemble with a Hann-windowed overlap-add. Shared by autotune() and the
    harmony generator, which both need "shift this frame by X semitones".
    """
    frame_length = hop_length * 2
    window = np.hanning(frame_length).astype(np.float32)
    padded = np.pad(y, (frame_length // 2, frame_length))
    out = np.zeros(len(padded), dtype=np.float64)
    norm = np.zeros(len(padded), dtype=np.float64)

    for i in range(len(n_steps)):
        start = i * hop_length
        end = start + frame_length
        if end > len(padded):
            break
        chunk = padded[start:end]
        if abs(n_steps[i]) > 1e-3:
            shifted = librosa.effects.pitch_shift(chunk, sr=sr, n_steps=n_steps[i])
            if formant_preserve:
                shifted = _correct_formants(shifted, chunk, n_fft=frame_length)
            chunk = shifted
        windowed = chunk * window
        out[start:end] += windowed
        norm[start:end] += window

    norm[norm < 1e-8] = 1.0
    out = out / norm
    return out[frame_length // 2: frame_length // 2 + len(y)].astype(np.float32)


def autotune(
    y: np.ndarray,
    sr: int,
    key: str = "C",
    scale: str = "major",
    custom_notes: list[int] | None = None,
    strength: float = 1.0,
    speed: float = 0.35,
    humanize: float = 0.0,
    formant_preserve: bool = False,
    reference_hz: float = 440.0,
    vibrato_depth: float = 0.0,
    vibrato_rate: float = 5.0,
    hop_length: int = 2048,
    fmin: float = 65.0,
    fmax: float = 1000.0,
) -> np.ndarray:
    """Correct the pitch of a monophonic vocal/instrument signal.

    key/scale: target note grid; pass scale="custom" with custom_notes (a list
        of allowed pitch classes 0-11, 0=key's root) to build your own scale.
    strength: 0..1, how much of the correction to apply (0 = dry, 1 = full snap).
    speed: 0..1, how fast the correction follows the target pitch
           (low = natural glide, high = robotic/"T-Pain" snap).
    humanize: 0..1, backs off correction during natural vibrato/pitch movement
              instead of flattening it (0 = always fully correct, 1 = mostly
              leave existing vibrato alone).
    formant_preserve: keep vocal timbre natural on larger shifts instead of
              the classic "chipmunk" sound.
    reference_hz: tuning reference for A4 (default concert pitch 440 Hz).
    vibrato_depth/vibrato_rate: re-introduce a synthesized vibrato (in
              semitones / Hz) on top of the corrected pitch.
    """
    if y.ndim > 1:
        y = np.mean(y, axis=1)
    y = y.astype(np.float32)
    if len(y) == 0:
        return y

    root_pc = _key_to_pitch_class(key)
    if scale == "custom" and custom_notes:
        scale_intervals = sorted(set(int(n) % 12 for n in custom_notes))
    else:
        scale_intervals = SCALES.get(scale, SCALES["major"])

    ref_ratio = 440.0 / reference_hz

    f0, voiced_flag, _ = librosa.pyin(
        y, sr=sr, fmin=fmin, fmax=fmax, frame_length=hop_length * 2, hop_length=hop_length
    )

    n_frames = len(f0)
    target_midi = np.full(n_frames, np.nan)
    original_midi = np.full(n_frames, np.nan)
    for i in range(n_frames):
        if voiced_flag[i] and f0[i] and f0[i] > 0:
            midi = librosa.hz_to_midi(f0[i] * ref_ratio)
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

    # Humanize: measure local pitch movement (vibrato/slides) and pull the
    # effective strength down there, so natural expression survives instead
    # of being flattened onto the grid.
    humanize = float(np.clip(humanize, 0.0, 1.0))
    variability = np.zeros(n_frames)
    if humanize > 0:
        window_r = 2
        for i in range(n_frames):
            lo, hi = max(0, i - window_r), min(n_frames, i + window_r + 1)
            local = original_midi[lo:hi]
            local = local[~np.isnan(local)]
            variability[i] = np.std(local) if len(local) > 1 else 0.0
        max_var = np.nanmax(variability) if np.nanmax(variability) > 0 else 1.0
        variability = np.clip(variability / max_var, 0.0, 1.0)

    if vibrato_depth > 0:
        times = librosa.frames_to_time(np.arange(n_frames), sr=sr, hop_length=hop_length)
        vibrato = vibrato_depth * np.sin(2 * np.pi * vibrato_rate * times)
    else:
        vibrato = np.zeros(n_frames)

    n_steps = np.zeros(n_frames)
    for i in range(n_frames):
        if np.isnan(target_midi[i]):
            continue
        effective_strength = strength * (1.0 - humanize * variability[i])
        corrected_midi = original_midi[i] + effective_strength * (smoothed[i] - original_midi[i])
        corrected_midi += vibrato[i]
        target_freq = librosa.midi_to_hz(corrected_midi) / ref_ratio
        n_steps[i] = 12 * np.log2(target_freq / f0[i])

    return resynthesize_with_pitch_curve(y, sr, n_steps, hop_length, formant_preserve=formant_preserve)
