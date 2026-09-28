from __future__ import annotations

from PySide6.QtWidgets import (
    QDialog, QVBoxLayout, QFormLayout, QComboBox, QCheckBox, QDialogButtonBox, QLabel,
)

from audio.autotune import NOTE_NAMES, SCALES
from audio.harmony import NAMED_INTERVALS
from audio.mixer import Track


class HarmonyDialog(QDialog):
    """Pick a source track + key/scale + which harmony voices to generate."""

    def __init__(self, tracks: list[Track], parent=None):
        super().__init__(parent)
        self.setWindowTitle("Generate Harmony")
        self.tracks = [t for t in tracks if len(t.audio) > 0]

        layout = QVBoxLayout(self)
        if not self.tracks:
            layout.addWidget(QLabel("No track has audio yet — record or load one first."))
            buttons = QDialogButtonBox(QDialogButtonBox.Cancel)
            buttons.rejected.connect(self.reject)
            layout.addWidget(buttons)
            return

        layout.addWidget(QLabel(
            "Creates new backing-vocal tracks from the selected lead, following\n"
            "the key/scale so every harmony note stays in tune automatically."
        ))

        form = QFormLayout()
        layout.addLayout(form)

        self.track_combo = QComboBox()
        self.track_combo.addItems([t.name for t in self.tracks])
        form.addRow("Source track", self.track_combo)

        self.key_combo = QComboBox()
        self.key_combo.addItems(NOTE_NAMES)
        default_track = self.tracks[0]
        if default_track.effects.autotune_enabled:
            self.key_combo.setCurrentText(default_track.effects.autotune_key)
        form.addRow("Key", self.key_combo)

        self.scale_combo = QComboBox()
        self.scale_combo.addItems([s for s in SCALES.keys()])
        if default_track.effects.autotune_enabled and default_track.effects.autotune_scale in SCALES:
            self.scale_combo.setCurrentText(default_track.effects.autotune_scale)
        form.addRow("Scale", self.scale_combo)

        layout.addWidget(QLabel("Harmony voices to add:"))
        self.interval_boxes: dict[str, QCheckBox] = {}
        for name in NAMED_INTERVALS:
            cb = QCheckBox(name)
            if name in ("third above", "third below"):
                cb.setChecked(True)
            self.interval_boxes[name] = cb
            layout.addWidget(cb)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def selected_track(self) -> Track:
        return self.tracks[self.track_combo.currentIndex()]

    def selected_key_scale(self) -> tuple[str, str]:
        return self.key_combo.currentText(), self.scale_combo.currentText()

    def selected_steps(self) -> list[tuple[str, int]]:
        return [(name, NAMED_INTERVALS[name]) for name, cb in self.interval_boxes.items() if cb.isChecked()]
