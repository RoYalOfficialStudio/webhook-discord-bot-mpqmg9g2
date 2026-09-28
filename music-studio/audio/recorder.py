"""Offline microphone recorder. No network/cloud involved — everything stays local."""
from __future__ import annotations

import threading
from dataclasses import dataclass, field

import numpy as np
import sounddevice as sd
import soundfile as sf


def list_input_devices() -> list[dict]:
    devices = sd.query_devices()
    return [
        {"index": i, "name": d["name"], "channels": d["max_input_channels"]}
        for i, d in enumerate(devices)
        if d["max_input_channels"] > 0
    ]


@dataclass
class Recorder:
    samplerate: int = 44100
    channels: int = 1
    device: int | None = None

    _stream: sd.InputStream | None = field(default=None, init=False, repr=False)
    _chunks: list[np.ndarray] = field(default_factory=list, init=False, repr=False)
    _lock: threading.Lock = field(default_factory=threading.Lock, init=False, repr=False)
    _recording: bool = field(default=False, init=False, repr=False)

    @property
    def is_recording(self) -> bool:
        return self._recording

    def _callback(self, indata, frames, time_info, status):
        with self._lock:
            self._chunks.append(indata.copy())

    def start(self) -> None:
        if self._recording:
            return
        self._chunks = []
        self._stream = sd.InputStream(
            samplerate=self.samplerate,
            channels=self.channels,
            device=self.device,
            callback=self._callback,
        )
        self._stream.start()
        self._recording = True

    def stop(self) -> np.ndarray:
        if not self._recording:
            return np.zeros((0, self.channels), dtype=np.float32)
        self._stream.stop()
        self._stream.close()
        self._stream = None
        self._recording = False
        with self._lock:
            if self._chunks:
                audio = np.concatenate(self._chunks, axis=0).astype(np.float32)
            else:
                audio = np.zeros((0, self.channels), dtype=np.float32)
            self._chunks = []
        return audio

    def save(self, path: str, audio: np.ndarray) -> None:
        sf.write(path, audio, self.samplerate)
