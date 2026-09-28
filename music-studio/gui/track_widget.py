from __future__ import annotations

import numpy as np
import sounddevice as sd
from PySide6.QtWidgets import (
    QWidget, QHBoxLayout, QVBoxLayout, QLabel, QPushButton, QSlider,
    QCheckBox, QLineEdit, QFileDialog, QFrame,
)
from PySide6.QtCore import Qt, Signal

from audio.recorder import Recorder
from audio.mixer import Track
from .effects_dialog import EffectsDialog

try:
    import soundfile as sf
except ImportError:  # pragma: no cover
    sf = None


class TrackWidget(QFrame):
    removed = Signal(object)
    changed = Signal()

    def __init__(self, track: Track, parent=None):
        super().__init__(parent)
        self.track = track
        self.recorder = Recorder(samplerate=track.sr, channels=1)
        self.setFrameShape(QFrame.StyledPanel)

        root = QVBoxLayout(self)
        top = QHBoxLayout()
        root.addLayout(top)

        self.name_edit = QLineEdit(track.name)
        self.name_edit.setMaximumWidth(140)
        self.name_edit.textChanged.connect(self._on_name_changed)
        top.addWidget(self.name_edit)

        self.record_btn = QPushButton("Record")
        self.record_btn.clicked.connect(self._toggle_record)
        top.addWidget(self.record_btn)

        self.load_btn = QPushButton("Load File")
        self.load_btn.clicked.connect(self._load_file)
        top.addWidget(self.load_btn)

        self.play_btn = QPushButton("Play")
        self.play_btn.clicked.connect(self._play)
        top.addWidget(self.play_btn)

        self.effects_btn = QPushButton("Effects...")
        self.effects_btn.clicked.connect(self._open_effects)
        top.addWidget(self.effects_btn)

        self.mute_box = QCheckBox("Mute")
        self.mute_box.setChecked(track.mute)
        self.mute_box.toggled.connect(self._on_mute)
        top.addWidget(self.mute_box)

        self.solo_box = QCheckBox("Solo")
        self.solo_box.setChecked(track.solo)
        self.solo_box.toggled.connect(self._on_solo)
        top.addWidget(self.solo_box)

        self.remove_btn = QPushButton("Remove")
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

        bottom.addWidget(QLabel("Pan"))
        self.pan_slider = QSlider(Qt.Horizontal)
        self.pan_slider.setRange(-100, 100)
        self.pan_slider.setValue(int(track.pan * 100))
        self.pan_slider.valueChanged.connect(self._on_pan)
        bottom.addWidget(self.pan_slider)

        self.status_label = QLabel(self._status_text())
        root.addWidget(self.status_label)

    def _status_text(self) -> str:
        n = len(self.track.audio)
        secs = n / self.track.sr if self.track.sr else 0
        return f"{secs:.1f}s recorded" if n else "empty — record or load audio"

    def _on_name_changed(self, text: str) -> None:
        self.track.name = text

    def _on_volume(self, value: int) -> None:
        self.track.volume = value / 100.0

    def _on_pan(self, value: int) -> None:
        self.track.pan = value / 100.0

    def _on_mute(self, checked: bool) -> None:
        self.track.mute = checked

    def _on_solo(self, checked: bool) -> None:
        self.track.solo = checked

    def _toggle_record(self) -> None:
        if not self.recorder.is_recording:
            self.recorder.start()
            self.record_btn.setText("Stop")
        else:
            audio = self.recorder.stop()
            self.track.audio = audio.flatten() if audio.ndim > 1 and audio.shape[1] == 1 else audio
            self.record_btn.setText("Record")
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
            self.changed.emit()
