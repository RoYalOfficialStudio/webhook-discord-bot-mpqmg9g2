"""Multi-track mixer: per-track volume/pan/effects chain, mixdown, and export."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable

import numpy as np

from .effects import ParametricEQ, apply_delay, apply_reverb, apply_compressor, apply_limiter
from .autotune import autotune
from .io_formats import export_audio


def _to_stereo(y: np.ndarray) -> np.ndarray:
    if y.ndim == 1:
        return np.stack([y, y], axis=1)
    if y.shape[1] == 1:
        return np.repeat(y, 2, axis=1)
    return y[:, :2]


@dataclass
class EffectSettings:
    eq: ParametricEQ | None = None
    eq_enabled: bool = False

    delay_enabled: bool = False
    delay_ms: float = 300.0
    delay_feedback: float = 0.35
    delay_mix: float = 0.3

    reverb_enabled: bool = False
    reverb_room_size: float = 0.5
    reverb_damping: float = 0.5
    reverb_wet: float = 0.3

    compressor_enabled: bool = False
    comp_threshold_db: float = -18.0
    comp_ratio: float = 4.0
    comp_attack_ms: float = 10.0
    comp_release_ms: float = 100.0
    comp_makeup_db: float = 0.0

    autotune_enabled: bool = False
    autotune_key: str = "C"
    autotune_scale: str = "major"
    autotune_custom_notes: list[int] = field(default_factory=list)
    autotune_strength: float = 1.0
    autotune_speed: float = 0.35
    autotune_humanize: float = 0.0
    autotune_formant_preserve: bool = False
    autotune_reference_hz: float = 440.0
    autotune_vibrato_depth: float = 0.0
    autotune_vibrato_rate: float = 5.0
    autotune_preset_name: str = ""


@dataclass
class Track:
    name: str
    audio: np.ndarray  # float32, shape (n_samples,) or (n_samples, channels)
    sr: int
    volume: float = 1.0  # linear gain, 1.0 = unity
    pan: float = 0.0  # -1 (left) .. 1 (right)
    mute: bool = False
    solo: bool = False
    effects: EffectSettings = field(default_factory=EffectSettings)

    def rendered(self) -> np.ndarray:
        """Apply this track's effect chain and return processed stereo audio."""
        y = self.audio
        fx = self.effects

        if fx.autotune_enabled:
            y = autotune(
                y, self.sr,
                key=fx.autotune_key,
                scale=fx.autotune_scale,
                custom_notes=fx.autotune_custom_notes,
                strength=fx.autotune_strength,
                speed=fx.autotune_speed,
                humanize=fx.autotune_humanize,
                formant_preserve=fx.autotune_formant_preserve,
                reference_hz=fx.autotune_reference_hz,
                vibrato_depth=fx.autotune_vibrato_depth,
                vibrato_rate=fx.autotune_vibrato_rate,
            )
        if fx.eq_enabled and fx.eq is not None:
            y = fx.eq.process(y, self.sr)
        if fx.compressor_enabled:
            y = apply_compressor(
                y, self.sr,
                threshold_db=fx.comp_threshold_db,
                ratio=fx.comp_ratio,
                attack_ms=fx.comp_attack_ms,
                release_ms=fx.comp_release_ms,
                makeup_gain_db=fx.comp_makeup_db,
            )
        if fx.delay_enabled:
            y = apply_delay(
                y, self.sr,
                delay_ms=fx.delay_ms,
                feedback=fx.delay_feedback,
                mix=fx.delay_mix,
            )
        if fx.reverb_enabled:
            y = apply_reverb(
                y, self.sr,
                room_size=fx.reverb_room_size,
                damping=fx.reverb_damping,
                wet=fx.reverb_wet,
            )

        stereo = _to_stereo(y).astype(np.float64)
        left_gain = self.volume * min(1.0, 1.0 - self.pan)
        right_gain = self.volume * min(1.0, 1.0 + self.pan)
        stereo[:, 0] *= left_gain
        stereo[:, 1] *= right_gain
        return stereo


class Mixer:
    def __init__(self, sr: int = 44100):
        self.sr = sr
        self.tracks: list[Track] = []

    def add_track(self, track: Track) -> None:
        self.tracks.append(track)

    def remove_track(self, track: Track) -> None:
        self.tracks.remove(track)

    def render(self, progress_cb: Callable[[int, int], None] | None = None) -> np.ndarray:
        active = [t for t in self.tracks if not t.mute]
        if any(t.solo for t in self.tracks):
            active = [t for t in active if t.solo]

        if not active:
            return np.zeros((0, 2), dtype=np.float32)

        rendered = []
        for i, track in enumerate(active):
            rendered.append(track.rendered())
            if progress_cb:
                progress_cb(i + 1, len(active))

        max_len = max(r.shape[0] for r in rendered)
        master = np.zeros((max_len, 2), dtype=np.float64)
        for r in rendered:
            master[: r.shape[0]] += r

        master = apply_limiter(master)
        return master.astype(np.float32)

    def export(self, path: str,
               progress_cb: Callable[[int, int], None] | None = None) -> None:
        master = self.render(progress_cb=progress_cb)
        export_audio(path, master, self.sr)
