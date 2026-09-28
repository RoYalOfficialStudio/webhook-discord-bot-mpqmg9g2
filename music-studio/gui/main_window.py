from __future__ import annotations

import os
import time

import numpy as np
import sounddevice as sd
from PySide6.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QScrollArea,
    QFileDialog, QLabel, QMessageBox, QProgressDialog, QFrame, QSlider,
)
from PySide6.QtCore import Qt, QTimer

from audio.mixer import Mixer, Track, EffectSettings
from audio.project import save_project, load_project
from audio.io_formats import EXPORT_FILTER
from audio.harmony import generate_harmony_voice
from audio.settings import load_settings, save_settings
from .track_widget import TrackWidget, _format_time
from .harmony_dialog import HarmonyDialog
from .devices_dialog import DevicesDialog

DEFAULT_SR = 44100


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Offline Music Studio")
        self.resize(900, 680)

        self.mixer = Mixer(sr=DEFAULT_SR)
        self.track_widgets: list[TrackWidget] = []
        self.settings = load_settings()

        self._playback_master: np.ndarray | None = None
        self._playback_sr = DEFAULT_SR
        self._playback_start = 0.0
        self._seeking = False
        self._playback_timer = QTimer(self)
        self._playback_timer.setInterval(100)
        self._playback_timer.timeout.connect(self._update_playback_progress)

        central = QWidget()
        self.setCentralWidget(central)
        outer = QVBoxLayout(central)
        outer.setContentsMargins(12, 12, 12, 12)
        outer.setSpacing(10)

        title = QLabel("OFFLINE MUSIC STUDIO")
        title.setStyleSheet("font-size: 16px; font-weight: 700; letter-spacing: 2px; color: #5ee27a;")
        outer.addWidget(title)

        transport_frame = QFrame()
        transport_frame.setObjectName("transportBar")
        transport = QHBoxLayout(transport_frame)
        outer.addWidget(transport_frame)

        add_btn = QPushButton("+ Add Track")
        add_btn.setObjectName("primary")
        add_btn.clicked.connect(self.add_track)
        transport.addWidget(add_btn)

        self.play_btn = QPushButton("▶ Play Mix")
        self.play_btn.setObjectName("primary")
        self.play_btn.clicked.connect(self.play_mix)
        transport.addWidget(self.play_btn)

        stop_btn = QPushButton("⏹ Stop")
        stop_btn.clicked.connect(self.stop_playback)
        transport.addWidget(stop_btn)

        transport.addSpacing(16)

        export_btn = QPushButton("Export Mixdown...")
        export_btn.clicked.connect(self.export_mix)
        transport.addWidget(export_btn)

        save_btn = QPushButton("Save Project...")
        save_btn.clicked.connect(self.save_project)
        transport.addWidget(save_btn)

        open_btn = QPushButton("Open Project...")
        open_btn.clicked.connect(self.open_project)
        transport.addWidget(open_btn)

        transport.addSpacing(16)

        harmony_btn = QPushButton("🎤 Add Harmony...")
        harmony_btn.clicked.connect(self.generate_harmony)
        transport.addWidget(harmony_btn)

        transport.addStretch()

        devices_btn = QPushButton("⚙ Devices...")
        devices_btn.setToolTip("Choose your microphone and headphones/speakers")
        devices_btn.clicked.connect(self.open_devices_dialog)
        transport.addWidget(devices_btn)

        # --- Playback bar: position, seek, preview volume -----------------------
        playback_frame = QFrame()
        playback_frame.setObjectName("transportBar")
        playback_row = QHBoxLayout(playback_frame)
        outer.addWidget(playback_frame)

        self.playback_time_label = QLabel("00:00.0 / 00:00.0")
        self.playback_time_label.setObjectName("dim")
        self.playback_time_label.setFixedWidth(120)
        playback_row.addWidget(self.playback_time_label)

        self.playback_slider = QSlider(Qt.Horizontal)
        self.playback_slider.setRange(0, 1000)
        self.playback_slider.sliderPressed.connect(self._on_seek_pressed)
        self.playback_slider.sliderReleased.connect(self._seek_playback)
        playback_row.addWidget(self.playback_slider, 1)

        playback_row.addWidget(QLabel("Preview Vol"))
        self.preview_volume_slider = QSlider(Qt.Horizontal)
        self.preview_volume_slider.setRange(0, 150)
        self.preview_volume_slider.setValue(100)
        self.preview_volume_slider.setFixedWidth(100)
        self.preview_volume_slider.setToolTip("Only affects how loud playback sounds here — not the exported file.")
        playback_row.addWidget(self.preview_volume_slider)

        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        outer.addWidget(self.scroll)

        self.track_container = QWidget()
        self.track_layout = QVBoxLayout(self.track_container)
        self.track_layout.addStretch()
        self.scroll.setWidget(self.track_container)

        if not self.mixer.tracks:
            self.add_track()

    def add_track(self) -> None:
        index = len(self.mixer.tracks) + 1
        track = Track(name=f"Track {index}", audio=np.zeros(0, dtype=np.float32), sr=DEFAULT_SR)
        self.mixer.add_track(track)
        self._add_track_widget(track)

    def _remove_track(self, widget: TrackWidget) -> None:
        widget.stop_recording_if_active()
        self.mixer.remove_track(widget.track)
        self.track_widgets.remove(widget)
        widget.setParent(None)
        widget.deleteLater()

    def open_devices_dialog(self) -> None:
        dialog = DevicesDialog(self.settings, self)
        if dialog.exec():
            self.settings = dialog.selected_settings()
            save_settings(self.settings)
            self._apply_settings_to_tracks()

    def _apply_settings_to_tracks(self) -> None:
        for widget in self.track_widgets:
            widget.recorder.device = self.settings.input_device
            widget.recorder.output_device = self.settings.output_device
            widget.output_device = self.settings.output_device

    def play_mix(self) -> None:
        master = self.mixer.render()
        if len(master) == 0:
            QMessageBox.information(self, "Nothing to play", "Add some audio to a track first.")
            return
        self._start_playback(master, self.mixer.sr)

    def stop_playback(self) -> None:
        sd.stop()
        self._playback_timer.stop()
        self._playback_master = None
        self.playback_slider.setValue(0)
        self.playback_time_label.setText("00:00.0 / 00:00.0")

    def _start_playback(self, master: np.ndarray, sr: int, start_sample: int = 0) -> None:
        volume = self.preview_volume_slider.value() / 100.0
        segment = master[start_sample:] * volume
        try:
            sd.play(segment, sr, device=self.settings.output_device)
        except Exception as exc:
            QMessageBox.warning(self, "Playback failed", str(exc))
            return
        self._playback_master = master
        self._playback_sr = sr
        self._playback_start = time.monotonic() - start_sample / sr
        self._playback_timer.start()

    def _on_seek_pressed(self) -> None:
        self._seeking = True

    def _seek_playback(self) -> None:
        self._seeking = False
        if self._playback_master is None:
            return
        frac = self.playback_slider.value() / 1000
        start_sample = int(frac * len(self._playback_master))
        sd.stop()
        self._start_playback(self._playback_master, self._playback_sr, start_sample=start_sample)

    def _update_playback_progress(self) -> None:
        if self._playback_master is None:
            self._playback_timer.stop()
            return
        duration = len(self._playback_master) / self._playback_sr
        elapsed = time.monotonic() - self._playback_start
        try:
            stream = sd.get_stream()
            still_playing = stream is not None and stream.active
        except RuntimeError:
            still_playing = False
        if elapsed >= duration or not still_playing:
            self._playback_timer.stop()
            self._playback_master = None
            self.playback_slider.setValue(0)
            self.playback_time_label.setText(f"00:00.0 / {_format_time(duration)}")
            return
        if not self._seeking:
            self.playback_slider.setValue(int(elapsed / duration * 1000))
        self.playback_time_label.setText(f"{_format_time(elapsed)} / {_format_time(duration)}")

    def export_mix(self) -> None:
        path, selected_filter = QFileDialog.getSaveFileName(
            self, "Export mixdown", "mixdown.wav", EXPORT_FILTER
        )
        if not path:
            return
        if "." not in os.path.basename(path):
            ext = selected_filter.split("*")[1].rstrip(")").lower() if "*" in selected_filter else ".wav"
            path += ext
        progress = QProgressDialog("Rendering mixdown...", None, 0, len(self.mixer.tracks), self)
        progress.setWindowModality(Qt.WindowModal)
        progress.setMinimumDuration(0)

        try:
            self.mixer.export(path, progress_cb=lambda i, n: progress.setValue(i))
        except RuntimeError as exc:
            QMessageBox.warning(self, "Export failed", str(exc))
            return
        finally:
            progress.setValue(len(self.mixer.tracks))

        QMessageBox.information(self, "Export complete", f"Saved to {path}")

    def save_project(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "Choose project folder")
        if not folder:
            return
        save_project(self.mixer, folder)
        QMessageBox.information(self, "Saved", f"Project saved to {folder}")

    def open_project(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "Choose project folder")
        if not folder:
            return
        try:
            self.mixer = load_project(folder)
        except FileNotFoundError:
            QMessageBox.warning(self, "Open failed", "No project.json found in that folder.")
            return

        for widget in self.track_widgets:
            widget.stop_recording_if_active()
            widget.setParent(None)
            widget.deleteLater()
        self.track_widgets = []

        for track in self.mixer.tracks:
            self._add_track_widget(track)

    def _add_track_widget(self, track: Track) -> None:
        widget = TrackWidget(track, color_index=len(self.track_widgets))
        widget.recorder.device = self.settings.input_device
        widget.recorder.output_device = self.settings.output_device
        widget.output_device = self.settings.output_device
        widget.removed.connect(self._remove_track)
        self.track_widgets.append(widget)
        self.track_layout.insertWidget(self.track_layout.count() - 1, widget)

    def generate_harmony(self) -> None:
        dialog = HarmonyDialog(self.mixer.tracks, self)
        if not dialog.exec() or not dialog.tracks:
            return

        source = dialog.selected_track()
        key, scale = dialog.selected_key_scale()
        steps = dialog.selected_steps()
        if not steps:
            return

        progress = QProgressDialog("Generating harmony voices...", None, 0, len(steps), self)
        progress.setWindowModality(Qt.WindowModal)
        progress.setMinimumDuration(0)

        for i, (name, step_count) in enumerate(steps):
            audio = generate_harmony_voice(source.audio, source.sr, key=key, scale=scale, steps=step_count)
            pan = 0.35 if step_count > 0 else -0.35
            track = Track(name=f"{source.name} ({name})", audio=audio, sr=source.sr, pan=pan, volume=0.85)
            self.mixer.add_track(track)
            self._add_track_widget(track)
            progress.setValue(i + 1)

        progress.setValue(len(steps))
