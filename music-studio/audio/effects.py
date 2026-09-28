"""Studio effects implemented from scratch with numpy/scipy: EQ, delay, reverb, compressor."""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from scipy import signal


def _ensure_2d(y: np.ndarray) -> tuple[np.ndarray, bool]:
    if y.ndim == 1:
        return y[:, None], True
    return y, False


# ---------------------------------------------------------------------------
# Parametric EQ (RBJ biquad cookbook filters)
# ---------------------------------------------------------------------------

def _peaking_coeffs(sr: float, freq: float, gain_db: float, q: float):
    a = 10 ** (gain_db / 40)
    w0 = 2 * np.pi * freq / sr
    alpha = np.sin(w0) / (2 * q)
    cos_w0 = np.cos(w0)

    b0 = 1 + alpha * a
    b1 = -2 * cos_w0
    b2 = 1 - alpha * a
    a0 = 1 + alpha / a
    a1 = -2 * cos_w0
    a2 = 1 - alpha / a
    return np.array([b0, b1, b2]) / a0, np.array([a0, a1, a2]) / a0


def _shelf_coeffs(sr: float, freq: float, gain_db: float, kind: str):
    a = 10 ** (gain_db / 40)
    w0 = 2 * np.pi * freq / sr
    cos_w0 = np.cos(w0)
    sin_w0 = np.sin(w0)
    s = 1.0  # shelf slope
    alpha = sin_w0 / 2 * np.sqrt((a + 1 / a) * (1 / s - 1) + 2)
    two_sqrt_a_alpha = 2 * np.sqrt(a) * alpha

    if kind == "low":
        b0 = a * ((a + 1) - (a - 1) * cos_w0 + two_sqrt_a_alpha)
        b1 = 2 * a * ((a - 1) - (a + 1) * cos_w0)
        b2 = a * ((a + 1) - (a - 1) * cos_w0 - two_sqrt_a_alpha)
        a0 = (a + 1) + (a - 1) * cos_w0 + two_sqrt_a_alpha
        a1 = -2 * ((a - 1) + (a + 1) * cos_w0)
        a2 = (a + 1) + (a - 1) * cos_w0 - two_sqrt_a_alpha
    else:  # high shelf
        b0 = a * ((a + 1) + (a - 1) * cos_w0 + two_sqrt_a_alpha)
        b1 = -2 * a * ((a - 1) + (a + 1) * cos_w0)
        b2 = a * ((a + 1) + (a - 1) * cos_w0 - two_sqrt_a_alpha)
        a0 = (a + 1) - (a - 1) * cos_w0 + two_sqrt_a_alpha
        a1 = 2 * ((a - 1) - (a + 1) * cos_w0)
        a2 = (a + 1) - (a - 1) * cos_w0 - two_sqrt_a_alpha

    return np.array([b0, b1, b2]) / a0, np.array([a0, a1, a2]) / a0


@dataclass
class EQBand:
    freq: float = 1000.0
    gain_db: float = 0.0
    q: float = 1.0
    kind: str = "peak"  # "peak", "low_shelf", "high_shelf"
    enabled: bool = True


@dataclass
class ParametricEQ:
    """Multi-band parametric EQ. Bands are applied in series (cascaded biquads)."""

    bands: list[EQBand] = field(default_factory=lambda: [
        EQBand(freq=100.0, gain_db=0.0, kind="low_shelf"),
        EQBand(freq=1000.0, gain_db=0.0, kind="peak", q=1.0),
        EQBand(freq=8000.0, gain_db=0.0, kind="high_shelf"),
    ])

    def process(self, y: np.ndarray, sr: int) -> np.ndarray:
        y2, was_1d = _ensure_2d(y)
        out = y2.astype(np.float64).copy()
        for band in self.bands:
            if not band.enabled or band.gain_db == 0.0:
                continue
            if band.kind == "peak":
                b, a = _peaking_coeffs(sr, band.freq, band.gain_db, band.q)
            elif band.kind == "low_shelf":
                b, a = _shelf_coeffs(sr, band.freq, band.gain_db, "low")
            else:
                b, a = _shelf_coeffs(sr, band.freq, band.gain_db, "high")
            for ch in range(out.shape[1]):
                out[:, ch] = signal.lfilter(b, a, out[:, ch])
        result = out.astype(np.float32)
        return result[:, 0] if was_1d else result


# ---------------------------------------------------------------------------
# Delay / echo
# ---------------------------------------------------------------------------

def apply_delay(y: np.ndarray, sr: int, delay_ms: float = 300.0,
                 feedback: float = 0.35, mix: float = 0.3, repeats: int = 6) -> np.ndarray:
    y2, was_1d = _ensure_2d(y)
    delay_samples = max(1, int(sr * delay_ms / 1000))
    out = y2.astype(np.float64).copy()
    tap = y2.astype(np.float64).copy()
    gain = 1.0
    for _ in range(repeats):
        gain *= feedback
        if gain < 1e-4:
            break
        shifted = np.zeros_like(tap)
        shifted[delay_samples:] = tap[:-delay_samples] if delay_samples < len(tap) else 0
        out += shifted * gain
        tap = shifted
    result = ((1 - mix) * y2 + mix * out).astype(np.float32)
    return result[:, 0] if was_1d else result


# ---------------------------------------------------------------------------
# Reverb (Schroeder: parallel comb filters -> series allpass filters)
# ---------------------------------------------------------------------------

def _comb_filter(x: np.ndarray, delay: int, feedback: float, damping: float) -> np.ndarray:
    # Damped feedback comb filter, expressed as a direct-form IIR so scipy can run it
    # in C instead of a per-sample Python loop (derivation: out = x + fb*lowpass(z^-D*out)).
    b = np.array([1.0, -damping])
    a = np.zeros(delay + 1)
    a[0] = 1.0
    a[1] += -damping
    a[delay] += -feedback * (1 - damping)
    return signal.lfilter(b, a, x)


def _allpass_filter(x: np.ndarray, delay: int, gain: float = 0.5) -> np.ndarray:
    # Schroeder allpass filter as a direct-form IIR (see module notes above _comb_filter).
    b = np.zeros(delay + 1)
    b[0] = gain
    b[delay] += 1.0
    a = np.zeros(delay + 1)
    a[0] = 1.0
    a[delay] += gain
    return signal.lfilter(b, a, x)


_COMB_DELAYS_MS = [29.7, 37.1, 41.1, 43.7]
_ALLPASS_DELAYS_MS = [5.0, 1.7]


def apply_reverb(y: np.ndarray, sr: int, room_size: float = 0.5,
                  damping: float = 0.5, wet: float = 0.3) -> np.ndarray:
    """room_size in [0, 1] controls decay/feedback, damping softens high frequencies."""
    y2, was_1d = _ensure_2d(y)
    feedback = 0.28 + 0.7 * np.clip(room_size, 0.0, 1.0)
    out = np.zeros_like(y2, dtype=np.float64)
    for ch in range(y2.shape[1]):
        x = y2[:, ch].astype(np.float64)
        combined = np.zeros_like(x)
        for ms in _COMB_DELAYS_MS:
            d = max(1, int(sr * ms / 1000))
            combined += _comb_filter(x, d, feedback, damping)
        combined /= len(_COMB_DELAYS_MS)
        for ms in _ALLPASS_DELAYS_MS:
            d = max(1, int(sr * ms / 1000))
            combined = _allpass_filter(combined, d)
        out[:, ch] = combined
    result = ((1 - wet) * y2 + wet * out).astype(np.float32)
    return result[:, 0] if was_1d else result


# ---------------------------------------------------------------------------
# Compressor (RMS envelope follower, soft-knee gain reduction)
# ---------------------------------------------------------------------------

def apply_compressor(y: np.ndarray, sr: int, threshold_db: float = -18.0,
                      ratio: float = 4.0, attack_ms: float = 10.0,
                      release_ms: float = 100.0, makeup_gain_db: float = 0.0) -> np.ndarray:
    y2, was_1d = _ensure_2d(y)
    x = y2.astype(np.float64)
    attack_coeff = np.exp(-1.0 / (sr * attack_ms / 1000)) if attack_ms > 0 else 0.0
    release_coeff = np.exp(-1.0 / (sr * release_ms / 1000)) if release_ms > 0 else 0.0

    # Sequential envelope follower (attack/release depend on their own running state,
    # so this can't be reduced to a single vectorized IIR call). Working on plain
    # Python floats/lists instead of numpy scalars keeps this fast enough offline.
    mono = np.max(np.abs(x), axis=1).tolist()
    envelope = [0.0] * len(mono)
    level = 0.0
    for i, sample in enumerate(mono):
        coeff = attack_coeff if sample > level else release_coeff
        level = coeff * level + (1 - coeff) * sample
        envelope[i] = level
    envelope = np.array(envelope)

    envelope_db = 20 * np.log10(np.maximum(envelope, 1e-9))
    over_db = envelope_db - threshold_db
    gain_reduction_db = np.where(over_db > 0, over_db * (1 / ratio - 1), 0.0)
    makeup = 10 ** (makeup_gain_db / 20)
    gain_lin = (10 ** (gain_reduction_db / 20)) * makeup

    out = x * gain_lin[:, None]
    result = out.astype(np.float32)
    return result[:, 0] if was_1d else result


def apply_limiter(y: np.ndarray, ceiling_db: float = -0.3) -> np.ndarray:
    """Simple brick-wall safety limiter for the master bus."""
    ceiling = 10 ** (ceiling_db / 20)
    peak = np.max(np.abs(y)) if y.size else 0.0
    if peak > ceiling:
        y = y * (ceiling / peak)
    return np.clip(y, -1.0, 1.0)
