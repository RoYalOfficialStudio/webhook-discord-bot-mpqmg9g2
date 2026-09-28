from __future__ import annotations

import numpy as np
import sounddevice as sd
from PySide6.QtWidgets import (
    QWidget, QHBoxLayout, QVBoxLayout, QLabel, QPushButton, QSlider,
    QLineEdit, QFileDialog, QFrame,
)
from PySide6.QtCore import Qt, Signal

from audio.recorder import Recorder
from audio.mixer import Track
from .effects_dialog import EffectsDialog
from .theme import TRACK_COLORS

try:
    import soundfile as sf
except ImportError:  # pragma: no cover
    sf = None


class TrackWidget(QFrame):
    removed = Signal(object)
    changed = Signal()

    def __init__(self, track: Track, color_index: int = 0, parent=None):
        super().__init__(parent)
        self.track = track
        self.recorder = Recorder(samplerate=track.sr, channels=1)
        self.setObjectName("trackCard")
        accent = TRACK_COLORS[color_index % len(TRACK_COLORS)]
        self.setStyleSheet(f"QFrame#trackCard {{ border-left: 4px solid {accent}; }}")

        root = QVBoxLayout(self)
        root.setContentsMargins(10, 8, 10, 8)
        top = QHBoxLayout()
        root.addLayout(top)

        self.name_edit = QLineEdit(track.name)
        self.name_edit.setObjectName("trackName")
        self.name_edit.setMaximumWidth(140)
        self.name_edit.textChanged.connect(self._on_name_changed)
        top.addWidget(self.name_edit)

        self.record_btn = QPushButton("⏺ Rec")
        self.record_btn.setObjectName("recordBtn")
        self.record_btn.setCheckable(True)
        self.record_btn.clicked.connect(self._toggle_record)
        top.addWidget(self.record_btn)

        self.load_btn = QPushButton("📁 Load")
        self.load_btn.clicked.connect(self._load_file)
        top.addWidget(self.load_btn)

        self.play_btn = QPushButton("▶ Play")
        self.play_btn.clicked.connect(self._play)
        top.addWidget(self.play_btn)

        self.effects_btn = QPushButton("Effects / Autotune...")
        self.effects_btn.clicked.connect(self._open_effects)
        top.addWidget(self.effects_btn)

        top.addStretch()

        self.mute_btn = QPushButton("M")
        self.mute_btn.setObjectName("toggleMute")
        self.mute_btn.setCheckable(True)
        self.mute_btn.setChecked(track.mute)
        self.mute_btn.setToolTip("Mute")
        self.mute_btn.toggled.connect(self._on_mute)
        top.addWidget(self.mute_btn)

        self.solo_btn = QPushButton("S")
        self.solo_btn.setObjectName("toggleSolo")
        self.solo_btn.setCheckable(True)
        self.solo_btn.setChecked(track.solo)
        self.solo_btn.setToolTip("Solo")
        self.solo_btn.toggled.connect(self._on_solo)
        top.addWidget(self.solo_btn)

        self.remove_btn = QPushButton("✕")
        self.remove_btn.setObjectName("removeBtn")
        self.remove_btn.setToolTip("Remove track")
        self.remove_btn.clicked.connect(lambda: self.removed.emit(self))
        top.addWidget(self.remove_btn)

        bottom = QHBoxLayout()
        root.addLayout(bottom)

        bottom.addWidget(QLabel("Vol"))
        self.volume_slider = QSlider(Qt.Horizontal)
        self.volume_slider.setRange(0, 150)
        self.volume_slider.setValue(int(track.volume * 100))
        self.volume_slider.valueChanged.connect(self._on_volume)
        bottom.addWidget(self.volume_slider)
        self.volume_label = QLabel(f"{track.volume:.2f}")
        self.volume_label.setObjectName("dim")
        self.volume_label.setFixedWidth(36)
        bottom.addWidget(self.volume_label)

        bottom.addWidget(QLabel("Pan"))
        self.pan_slider = QSlider(Qt.Horizontal)
        self.pan_slider.setRange(-100, 100)
        self.pan_slider.setValue(int(track.pan * 100))
        self.pan_slider.valueChanged.connect(self._on_pan)
        bottom.addWidget(self.pan_slider)
        self.pan_label = QLabel(self._pan_text(track.pan))
        self.pan_label.setObjectName("dim")
        self.pan_label.setFixedWidth(36)
        bottom.addWidget(self.pan_label)

        self.status_label = QLabel(self._status_text())
        self.status_label.setObjectName("dim")
        root.addWidget(self.status_label)

    def _status_text(self) -> str:
        n = len(self.track.audio)
        secs = n / self.track.sr if self.track.sr else 0
        base = f"{secs:.1f}s recorded" if n else "empty — record or load audio"
        if self.track.effects.autotune_enabled:
            preset = self.track.effects.autotune_preset_name
            base += f"  ·  Autotune: {preset or self.track.effects.autotune_key + ' ' + self.track.effects.autotune_scale}"
        return base

    @staticmethod
    def _pan_text(pan: float) -> str:
        if abs(pan) < 0.01:
            return "C"
        return f"{abs(int(pan * 100))}{'R' if pan > 0 else 'L'}"

    def _on_name_changed(self, text: str) -> None:
        self.track.name = text

    def _on_volume(self, value: int) -> None:
        self.track.volume = value / 100.0
        self.volume_label.setText(f"{self.track.volume:.2f}")

    def _on_pan(self, value: int) -> None:
        self.track.pan = value / 100.0
        self.pan_label.setText(self._pan_text(self.track.pan))

    def _on_mute(self, checked: bool) -> None:
        self.track.mute = checked

    def _on_solo(self, checked: bool) -> None:
        self.track.solo = checked

    def _toggle_record(self) -> None:
        if not self.recorder.is_recording:
            self.recorder.start()
            self.record_btn.setText("⏹ Stop")
        else:
            audio = self.recorder.stop()
            self.track.audio = audio.flatten() if audio.ndim > 1 and audio.shape[1] == 1 else audio
            self.record_btn.setText("⏺ Rec")
            self.record_btn.setChecked(False)
            self.status_label.setText(self._status_text())
            self.changed.emit()

    def _load_file(self) -> None:
        if sf is None:
            return
        path, _ = QFileDialog.getOpenFileName(self, "Load audio", "", "Audio files (*.wav *.flac *.ogg)")
        if not path:
            return
        audio, sr = sf.read(path, dtype="float32")
        self.track.audio = audio
        self.track.sr = sr
        self.recorder.samplerate = sr
        self.status_label.setText(self._status_text())
        self.changed.emit()

    def _play(self) -> None:
        if len(self.track.audio) == 0:
            return
        sd.play(self.track.rendered(), self.track.sr)

    def _open_effects(self) -> None:
        dialog = EffectsDialog(self.track.effects, self)
        if dialog.exec():
            dialog.apply_to(self.track.effects)
            self.status_label.setText(self._status_text())
            self.changed.emit()
