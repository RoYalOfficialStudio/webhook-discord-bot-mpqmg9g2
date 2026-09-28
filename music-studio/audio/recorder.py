"""Offline microphone recorder. No network/cloud involved — everything stays local."""
from __future__ import annotations

import threading
import time
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
    monitor: bool = False  # hear yourself live through the output device while recording
    monitor_gain: float = 0.8

    _stream: sd.InputStream | sd.Stream | None = field(default=None, init=False, repr=False)
    _chunks: list[np.ndarray] = field(default_factory=list, init=False, repr=False)
    _lock: threading.Lock = field(default_factory=threading.Lock, init=False, repr=False)
    _recording: bool = field(default=False, init=False, repr=False)
    _start_time: float = field(default=0.0, init=False, repr=False)
    _level: float = field(default=0.0, init=False, repr=False)
    _frames_recorded: int = field(default=0, init=False, repr=False)

    @property
    def is_recording(self) -> bool:
        return self._recording

    @property
    def level(self) -> float:
        """Current input peak level in [0, 1], updated live while recording."""
        return self._level

    @property
    def level_db(self) -> float:
        return 20 * np.log10(max(self._level, 1e-6))

    @property
    def elapsed_seconds(self) -> float:
        if not self._recording:
            return self._frames_recorded / self.samplerate if self.samplerate else 0.0
        return time.monotonic() - self._start_time

    def _callback(self, indata, frames, time_info, status):
        with self._lock:
            self._chunks.append(indata.copy())
            self._frames_recorded += frames
        self._level = float(np.max(np.abs(indata))) if frames else 0.0

    def _monitor_callback(self, indata, outdata, frames, time_info, status):
        with self._lock:
            self._chunks.append(indata.copy())
            self._frames_recorded += frames
        self._level = float(np.max(np.abs(indata))) if frames else 0.0
        out_channels = outdata.shape[1]
        if indata.shape[1] == out_channels:
            outdata[:] = indata * self.monitor_gain
        else:
            outdata[:] = np.repeat(indata[:, :1], out_channels, axis=1) * self.monitor_gain

    def start(self) -> None:
        if self._recording:
            return
        self._chunks = []
        self._level = 0.0
        self._frames_recorded = 0
        if self.monitor:
            self._stream = sd.Stream(
                samplerate=self.samplerate,
                channels=(self.channels, 2),
                device=(self.device, None),
                callback=self._monitor_callback,
            )
        else:
            self._stream = sd.InputStream(
                samplerate=self.samplerate,
                channels=self.channels,
                device=self.device,
                callback=self._callback,
            )
        self._stream.start()
        self._start_time = time.monotonic()
        self._recording = True

    def stop(self) -> np.ndarray:
        if not self._recording:
            return np.zeros((0, self.channels), dtype=np.float32)
        self._stream.stop()
        self._stream.close()
        self._stream = None
        self._recording = False
        self._level = 0.0
        with self._lock:
            if self._chunks:
                audio = np.concatenate(self._chunks, axis=0).astype(np.float32)
            else:
                audio = np.zeros((0, self.channels), dtype=np.float32)
            self._chunks = []
        return audio

    def save(self, path: str, audio: np.ndarray) -> None:
        sf.write(path, audio, self.samplerate)
