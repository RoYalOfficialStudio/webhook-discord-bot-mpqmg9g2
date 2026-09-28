from __future__ import annotations

import numpy as np
import sounddevice as sd
from PySide6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QFormLayout, QComboBox, QPushButton,
    QDialogButtonBox, QLabel,
)

from audio.recorder import list_input_devices, list_output_devices
from audio.settings import AppSettings


class DevicesDialog(QDialog):
    """Pick which microphone to record from and which headphones/speakers to
    hear yourself and play mixes through — the app used the system default
    for both before, which silently doesn't match everyone's setup.
    """

    def __init__(self, settings: AppSettings, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Audio Devices")

        layout = QVBoxLayout(self)
        layout.addWidget(QLabel(
            "Choose your microphone (input) and your headphones/speakers (output).\n"
            "\"System default\" uses whatever Windows/macOS/Linux is currently set to."
        ))

        form = QFormLayout()
        layout.addLayout(form)

        self.input_combo = QComboBox()
        self.input_combo.addItem("System default", None)
        try:
            for d in list_input_devices():
                self.input_combo.addItem(d["name"], d["index"])
        except Exception:
            pass
        self._select_current(self.input_combo, settings.input_device)
        form.addRow("Microphone (input)", self.input_combo)

        self.output_combo = QComboBox()
        self.output_combo.addItem("System default", None)
        try:
            for d in list_output_devices():
                self.output_combo.addItem(d["name"], d["index"])
        except Exception:
            pass
        self._select_current(self.output_combo, settings.output_device)
        form.addRow("Headphones / Speakers (output)", self.output_combo)

        test_row = QHBoxLayout()
        layout.addLayout(test_row)
        test_btn = QPushButton("🔊 Test Output")
        test_btn.clicked.connect(self._test_output)
        test_row.addWidget(test_btn)
        self.test_label = QLabel("")
        self.test_label.setObjectName("dim")
        test_row.addWidget(self.test_label, 1)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    @staticmethod
    def _select_current(combo: QComboBox, value) -> None:
        idx = combo.findData(value)
        combo.setCurrentIndex(idx if idx >= 0 else 0)

    def _test_output(self) -> None:
        sr = 44100
        t = np.linspace(0, 0.4, int(sr * 0.4), endpoint=False)
        tone = (0.3 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
        try:
            sd.play(tone, sr, device=self.output_combo.currentData())
            self.test_label.setText("Playing a 440 Hz test tone — hear it?")
        except Exception as exc:
            self.test_label.setText(f"Failed: {exc}")

    def selected_settings(self) -> AppSettings:
        return AppSettings(
            input_device=self.input_combo.currentData(),
            output_device=self.output_combo.currentData(),
        )
