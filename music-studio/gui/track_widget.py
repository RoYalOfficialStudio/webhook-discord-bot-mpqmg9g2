from __future__ import annotations

import numpy as np
import sounddevice as sd
from PySide6.QtWidgets import (
    QWidget, QHBoxLayout, QVBoxLayout, QLabel, QPushButton, QSlider,
    QLineEdit, QFileDialog, QFrame, QMessageBox, QProgressBar, QCheckBox,
)
from PySide6.QtCore import Qt, Signal, QTimer

from audio.recorder import Recorder
from audio.mixer import Track
from audio.io_formats import load_audio, LOAD_FILTER
from .effects_dialog import EffectsDialog
from .waveform_widget import WaveformWidget
from .theme import TRACK_COLORS


def _format_time(seconds: float) -> str:
    minutes = int(seconds // 60)
    secs = seconds - minutes * 60
    return f"{minutes:02d}:{secs:04.1f}"


class TrackWidget(QFrame):
    removed = Signal(object)
    changed = Signal()

    def __init__(self, track: Track, color_index: int = 0, parent=None):
        super().__init__(parent)
        self.track = track
        self.recorder = Recorder(samplerate=track.sr, channels=1)
        self.setObjectName("trackCard")
        self._accent = TRACK_COLORS[color_index % len(TRACK_COLORS)]
        self.setStyleSheet(f"QFrame#trackCard {{ border-left: 4px solid {self._accent}; }}")

        self._meter_timer = QTimer(self)
        self._meter_timer.setInterval(80)
        self._meter_timer.timeout.connect(self._update_meter)

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

        # --- Level meter + elapsed/duration time -----------------------------
        meter_row = QHBoxLayout()
        root.addLayout(meter_row)

        self.level_meter = QProgressBar()
        self.level_meter.setRange(0, 100)
        self.level_meter.setValue(0)
        self.level_meter.setTextVisible(False)
        self.level_meter.setFixedHeight(10)
        meter_row.addWidget(self.level_meter, 1)

        self.time_label = QLabel(self._duration_text())
        self.time_label.setObjectName("dim")
        self.time_label.setFixedWidth(70)
        meter_row.addWidget(self.time_label)

        # --- Waveform preview --------------------------------------------------
        self.waveform = WaveformWidget()
        root.addWidget(self.waveform)
        self.waveform.set_audio(track.audio, color=self._accent)

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

        # --- Quick Autotune controls (full set lives in Effects dialog) --------
        autotune_row = QHBoxLayout()
        root.addLayout(autotune_row)

        self.autotune_quick_enabled = QCheckBox("Autotune")
        self.autotune_quick_enabled.setChecked(track.effects.autotune_enabled)
        self.autotune_quick_enabled.toggled.connect(self._on_quick_autotune_toggle)
        autotune_row.addWidget(self.autotune_quick_enabled)

        autotune_row.addWidget(QLabel("Strength"))
        self.autotune_quick_strength = QSlider(Qt.Horizontal)
        self.autotune_quick_strength.setRange(0, 100)
        self.autotune_quick_strength.setValue(int(track.effects.autotune_strength * 100))
        self.autotune_quick_strength.valueChanged.connect(self._on_quick_strength)
        autotune_row.addWidget(self.autotune_quick_strength)
        self.autotune_strength_label = QLabel(f"{track.effects.autotune_strength:.2f}")
        self.autotune_strength_label.setObjectName("dim")
        self.autotune_strength_label.setFixedWidth(32)
        autotune_row.addWidget(self.autotune_strength_label)

        autotune_row.addWidget(QLabel("Speed"))
        self.autotune_quick_speed = QSlider(Qt.Horizontal)
        self.autotune_quick_speed.setRange(1, 100)
        self.autotune_quick_speed.setValue(int(track.effects.autotune_speed * 100))
        self.autotune_quick_speed.valueChanged.connect(self._on_quick_speed)
        autotune_row.addWidget(self.autotune_quick_speed)
        self.autotune_speed_label = QLabel(f"{track.effects.autotune_speed:.2f}")
        self.autotune_speed_label.setObjectName("dim")
        self.autotune_speed_label.setFixedWidth(32)
        autotune_row.addWidget(self.autotune_speed_label)

        self.status_label = QLabel(self._status_text())
        self.status_label.setObjectName("dim")
        root.addWidget(self.status_label)

    def _status_text(self) -> str:
        n = len(self.track.audio)
        base = "empty — record or load audio" if not n else "recorded"
        if self.track.effects.autotune_enabled:
            preset = self.track.effects.autotune_preset_name
            base += f"  ·  Autotune: {preset or self.track.effects.autotune_key + ' ' + self.track.effects.autotune_scale}"
        return base

    def _duration_text(self) -> str:
        n = len(self.track.audio)
        secs = n / self.track.sr if (self.track.sr and n) else 0.0
        return _format_time(secs)

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

    def _on_quick_autotune_toggle(self, checked: bool) -> None:
        self.track.effects.autotune_enabled = checked
        self.status_label.setText(self._status_text())
        self.changed.emit()

    def _on_quick_strength(self, value: int) -> None:
        self.track.effects.autotune_strength = value / 100.0
        self.autotune_strength_label.setText(f"{self.track.effects.autotune_strength:.2f}")

    def _on_quick_speed(self, value: int) -> None:
        self.track.effects.autotune_speed = value / 100.0
        self.autotune_speed_label.setText(f"{self.track.effects.autotune_speed:.2f}")

    def _refresh_quick_autotune_controls(self) -> None:
        fx = self.track.effects
        self.autotune_quick_enabled.blockSignals(True)
        self.autotune_quick_enabled.setChecked(fx.autotune_enabled)
        self.autotune_quick_enabled.blockSignals(False)
        self.autotune_quick_strength.blockSignals(True)
        self.autotune_quick_strength.setValue(int(fx.autotune_strength * 100))
        self.autotune_quick_strength.blockSignals(False)
        self.autotune_quick_speed.blockSignals(True)
        self.autotune_quick_speed.setValue(int(fx.autotune_speed * 100))
        self.autotune_quick_speed.blockSignals(False)
        self.autotune_strength_label.setText(f"{fx.autotune_strength:.2f}")
        self.autotune_speed_label.setText(f"{fx.autotune_speed:.2f}")

    def _update_meter(self) -> None:
        if not self.recorder.is_recording:
            self._meter_timer.stop()
            self.level_meter.setValue(0)
            return
        self.level_meter.setValue(int(min(1.0, self.recorder.level) * 100))
        self.time_label.setText(_format_time(self.recorder.elapsed_seconds))

    def _toggle_record(self) -> None:
        if not self.recorder.is_recording:
            self.recorder.start()
            self.record_btn.setText("⏹ Stop")
            self.waveform.set_audio(None)
            self._meter_timer.start()
        else:
            audio = self.recorder.stop()
            self._meter_timer.stop()
            self.level_meter.setValue(0)
            self.track.audio = audio.flatten() if audio.ndim > 1 and audio.shape[1] == 1 else audio
            self.record_btn.setText("⏺ Rec")
            self.record_btn.setChecked(False)
            self.time_label.setText(self._duration_text())
            self.waveform.set_audio(self.track.audio, color=self._accent)
            self.status_label.setText(self._status_text())
            self.changed.emit()

    def _load_file(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Load audio", "", LOAD_FILTER)
        if not path:
            return
        try:
            audio, sr = load_audio(path)
        except RuntimeError as exc:
            QMessageBox.warning(self, "Load failed", str(exc))
            return
        self.track.audio = audio
        self.track.sr = sr
        self.recorder.samplerate = sr
        self.time_label.setText(self._duration_text())
        self.waveform.set_audio(audio, color=self._accent)
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
            self._refresh_quick_autotune_controls()
            self.status_label.setText(self._status_text())
            self.changed.emit()
