"""Offline microphone recorder. No network/cloud involved — everything stays local."""
from __future__ import annotations

import collections
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


def list_output_devices() -> list[dict]:
    devices = sd.query_devices()
    return [
        {"index": i, "name": d["name"], "channels": d["max_output_channels"]}
        for i, d in enumerate(devices)
        if d["max_output_channels"] > 0
    ]


class _MonitorRingBuffer:
    """Small thread-safe pipe of audio frames from the input callback to the
    output callback. Input and output devices run on independent audio
    threads with independently-sized blocks, so this can't just hand off a
    single fixed-size chunk each time — it streams arbitrary-sized reads and
    writes, and caps how much it holds so monitoring latency can't creep up.
    """

    def __init__(self, max_samples: int):
        self._chunks: collections.deque[np.ndarray] = collections.deque()
        self._buffered = 0
        self._max_samples = max_samples
        self._lock = threading.Lock()

    def write(self, data: np.ndarray) -> None:
        with self._lock:
            self._chunks.append(data)
            self._buffered += len(data)
            while self._buffered > self._max_samples and self._chunks:
                dropped = self._chunks.popleft()
                self._buffered -= len(dropped)

    def read(self, n: int, channels: int) -> np.ndarray:
        out = np.zeros((n, channels), dtype=np.float32)
        filled = 0
        with self._lock:
            while filled < n and self._chunks:
                chunk = self._chunks[0]
                take = min(n - filled, len(chunk))
                source = chunk[:take]
                if source.shape[1] == channels:
                    out[filled:filled + take] = source
                else:
                    out[filled:filled + take] = np.repeat(source[:, :1], channels, axis=1)
                filled += take
                self._buffered -= take
                if take == len(chunk):
                    self._chunks.popleft()
                else:
                    self._chunks[0] = chunk[take:]
        return out


@dataclass
class Recorder:
    samplerate: int = 44100
    channels: int = 1
    device: int | None = None  # microphone (input); None = system default
    output_device: int | None = None  # headphones/speakers for monitoring; None = system default
    monitor: bool = False  # hear yourself live through the output device while recording
    monitor_gain: float = 0.8

    _stream: sd.InputStream | None = field(default=None, init=False, repr=False)
    _monitor_stream: sd.OutputStream | None = field(default=None, init=False, repr=False)
    _monitor_buffer: _MonitorRingBuffer | None = field(default=None, init=False, repr=False)
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
        if self._monitor_buffer is not None:
            self._monitor_buffer.write(indata.copy())

    def _output_callback(self, outdata, frames, time_info, status):
        if self._monitor_buffer is None:
            outdata[:] = 0
            return
        data = self._monitor_buffer.read(frames, outdata.shape[1])
        outdata[:] = data * self.monitor_gain

    def start(self) -> None:
        if self._recording:
            return
        self._chunks = []
        self._level = 0.0
        self._frames_recorded = 0
        # Two independent streams (rather than one full-duplex sd.Stream spanning
        # both devices) — PortAudio often refuses to combine an arbitrary input
        # device with an arbitrary output device in a single duplex stream
        # ("Illegal combination of I/O devices"), especially across host APIs on
        # Windows. Two single-direction streams always work.
        self._monitor_buffer = _MonitorRingBuffer(int(self.samplerate * 0.5)) if self.monitor else None
        input_stream = None
        monitor_stream = None
        try:
            input_stream = sd.InputStream(
                samplerate=self.samplerate,
                channels=self.channels,
                device=self.device,
                callback=self._callback,
            )
            input_stream.start()
            if self.monitor:
                monitor_stream = sd.OutputStream(
                    samplerate=self.samplerate,
                    channels=2,
                    device=self.output_device,
                    callback=self._output_callback,
                )
                monitor_stream.start()
        except Exception as exc:
            if input_stream is not None:
                input_stream.stop()
                input_stream.close()
            raise RuntimeError(
                f"Could not open the audio device ({exc}). Check Devices... "
                f"and make sure the microphone{' and headphones' if self.monitor else ''} "
                f"are connected and not in use by another app."
            ) from exc
        self._stream = input_stream
        self._monitor_stream = monitor_stream
        self._start_time = time.monotonic()
        self._recording = True

    def stop(self) -> np.ndarray:
        if not self._recording:
            return np.zeros((0, self.channels), dtype=np.float32)
        self._stream.stop()
        self._stream.close()
        self._stream = None
        if self._monitor_stream is not None:
            self._monitor_stream.stop()
            self._monitor_stream.close()
            self._monitor_stream = None
        self._monitor_buffer = None
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
