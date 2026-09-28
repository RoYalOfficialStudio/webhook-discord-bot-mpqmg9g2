"""Load/export audio in all common formats.

WAV, FLAC, OGG and AIFF are handled natively via soundfile (no extra system
dependency). Everything else people actually throw at a music program — MP3,
M4A/AAC, WMA, and whatever else your system's ffmpeg build decodes — goes
through pydub, which shells out to ffmpeg.
"""
from __future__ import annotations

import os

import numpy as np
import soundfile as sf

NATIVE_EXTS = {".wav", ".flac", ".ogg", ".oga", ".aiff", ".aif", ".aifc"}

LOAD_FILTER = (
    "Audio files (*.wav *.mp3 *.flac *.ogg *.aiff *.aif *.m4a *.aac *.wma);;"
    "WAV (*.wav);;MP3 (*.mp3);;FLAC (*.flac);;OGG (*.ogg);;AIFF (*.aiff *.aif);;"
    "M4A/AAC (*.m4a *.aac);;WMA (*.wma);;All files (*)"
)

EXPORT_FILTER = (
    "WAV (*.wav);;MP3 (*.mp3);;FLAC (*.flac);;OGG (*.ogg);;AIFF (*.aiff);;"
    "M4A/AAC (*.m4a);;WMA (*.wma)"
)

_PYDUB_FORMAT_OVERRIDES = {"m4a": "ipod", "aac": "adts"}


def _pydub_or_raise():
    try:
        from pydub import AudioSegment
    except ImportError as exc:
        raise RuntimeError(
            "This file format needs the 'pydub' package and a system ffmpeg "
            "installation (see README: Installation)."
        ) from exc
    return AudioSegment


def load_audio(path: str) -> tuple[np.ndarray, int]:
    """Read any supported audio file. Returns (audio float32 [-1,1], samplerate)."""
    ext = os.path.splitext(path)[1].lower()
    if ext in NATIVE_EXTS:
        audio, sr = sf.read(path, dtype="float32")
        return audio, sr
    return _load_via_pydub(path)


def _load_via_pydub(path: str) -> tuple[np.ndarray, int]:
    AudioSegment = _pydub_or_raise()
    segment = AudioSegment.from_file(path)
    channels = segment.channels
    samples = np.array(segment.get_array_of_samples())
    if channels > 1:
        samples = samples.reshape((-1, channels))
    max_val = float(1 << (8 * segment.sample_width - 1))
    audio = samples.astype(np.float32) / max_val
    return audio, segment.frame_rate


def export_audio(path: str, audio: np.ndarray, sr: int) -> None:
    """Write audio to any supported format, inferred from the file extension."""
    ext = os.path.splitext(path)[1].lower()
    if ext in NATIVE_EXTS:
        sf.write(path, audio, sr)
        return
    _export_via_pydub(path, audio, sr, ext.lstrip("."))


def _export_via_pydub(path: str, audio: np.ndarray, sr: int, fmt: str) -> None:
    AudioSegment = _pydub_or_raise()
    pcm16 = (np.clip(audio, -1.0, 1.0) * 32767).astype(np.int16)
    channels = 1 if pcm16.ndim == 1 else pcm16.shape[1]
    segment = AudioSegment(
        pcm16.tobytes(),
        frame_rate=sr,
        sample_width=2,
        channels=channels,
    )
    segment.export(path, format=_PYDUB_FORMAT_OVERRIDES.get(fmt, fmt))
