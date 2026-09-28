from __future__ import annotations

import numpy as np
import sounddevice as sd
from PySide6.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QScrollArea,
    QFileDialog, QLabel, QMessageBox, QProgressDialog, QFrame,
)
from PySide6.QtCore import Qt

from audio.mixer import Mixer, Track, EffectSettings
from audio.project import save_project, load_project
from .track_widget import TrackWidget

DEFAULT_SR = 44100


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Offline Music Studio")
        self.resize(900, 600)

        self.mixer = Mixer(sr=DEFAULT_SR)
        self.track_widgets: list[TrackWidget] = []

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

        play_btn = QPushButton("▶ Play Mix")
        play_btn.setObjectName("primary")
        play_btn.clicked.connect(self.play_mix)
        transport.addWidget(play_btn)

        stop_btn = QPushButton("⏹ Stop")
        stop_btn.clicked.connect(sd.stop)
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

        transport.addStretch()

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

        widget = TrackWidget(track, color_index=len(self.track_widgets))
        widget.removed.connect(self._remove_track)
        self.track_widgets.append(widget)
        self.track_layout.insertWidget(self.track_layout.count() - 1, widget)

    def _remove_track(self, widget: TrackWidget) -> None:
        self.mixer.remove_track(widget.track)
        self.track_widgets.remove(widget)
        widget.setParent(None)
        widget.deleteLater()

    def play_mix(self) -> None:
        master = self.mixer.render()
        if len(master) == 0:
            QMessageBox.information(self, "Nothing to play", "Add some audio to a track first.")
            return
        sd.play(master, self.mixer.sr)

    def export_mix(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "Export mixdown", "mixdown.wav", "WAV (*.wav);;MP3 (*.mp3)"
        )
        if not path:
            return
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
            widget.setParent(None)
            widget.deleteLater()
        self.track_widgets = []

        for track in self.mixer.tracks:
            widget = TrackWidget(track, color_index=len(self.track_widgets))
            widget.removed.connect(self._remove_track)
            self.track_widgets.append(widget)
            self.track_layout.insertWidget(self.track_layout.count() - 1, widget)
