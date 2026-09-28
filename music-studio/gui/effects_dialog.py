"""Dialog for editing a track's effect chain: EQ, delay, reverb, compressor, autotune."""
from __future__ import annotations

from PySide6.QtWidgets import (
    QDialog, QVBoxLayout, QFormLayout, QTabWidget, QWidget, QCheckBox,
    QDoubleSpinBox, QComboBox, QDialogButtonBox, QLabel,
)

from audio.effects import EQBand, ParametricEQ
from audio.autotune import NOTE_NAMES, SCALES
from audio.mixer import EffectSettings


def _spin(minimum, maximum, value, step=0.1, decimals=2) -> QDoubleSpinBox:
    box = QDoubleSpinBox()
    box.setRange(minimum, maximum)
    box.setSingleStep(step)
    box.setDecimals(decimals)
    box.setValue(value)
    return box


class EffectsDialog(QDialog):
    def __init__(self, effects: EffectSettings, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Track Effects")
        self.effects = effects
        if self.effects.eq is None:
            self.effects.eq = ParametricEQ()

        layout = QVBoxLayout(self)
        tabs = QTabWidget()
        layout.addWidget(tabs)

        tabs.addTab(self._build_eq_tab(), "EQ")
        tabs.addTab(self._build_delay_tab(), "Delay")
        tabs.addTab(self._build_reverb_tab(), "Reverb")
        tabs.addTab(self._build_compressor_tab(), "Compressor")
        tabs.addTab(self._build_autotune_tab(), "Autotune")

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def _build_eq_tab(self) -> QWidget:
        w = QWidget()
        form = QFormLayout(w)
        self.eq_enabled = QCheckBox("Enable EQ")
        self.eq_enabled.setChecked(self.effects.eq_enabled)
        form.addRow(self.eq_enabled)

        self.eq_gain_boxes = []
        labels = ["Low shelf (100 Hz) gain dB", "Mid peak (1 kHz) gain dB", "High shelf (8 kHz) gain dB"]
        for band, label in zip(self.effects.eq.bands, labels):
            box = _spin(-24.0, 24.0, band.gain_db, step=0.5)
            form.addRow(label, box)
            self.eq_gain_boxes.append(box)
        return w

    def _build_delay_tab(self) -> QWidget:
        w = QWidget()
        form = QFormLayout(w)
        self.delay_enabled = QCheckBox("Enable Delay")
        self.delay_enabled.setChecked(self.effects.delay_enabled)
        form.addRow(self.delay_enabled)

        self.delay_ms = _spin(10, 2000, self.effects.delay_ms, step=10, decimals=0)
        self.delay_feedback = _spin(0.0, 0.95, self.effects.delay_feedback, step=0.05)
        self.delay_mix = _spin(0.0, 1.0, self.effects.delay_mix, step=0.05)
        form.addRow("Delay time (ms)", self.delay_ms)
        form.addRow("Feedback", self.delay_feedback)
        form.addRow("Mix (wet)", self.delay_mix)
        return w

    def _build_reverb_tab(self) -> QWidget:
        w = QWidget()
        form = QFormLayout(w)
        self.reverb_enabled = QCheckBox("Enable Reverb")
        self.reverb_enabled.setChecked(self.effects.reverb_enabled)
        form.addRow(self.reverb_enabled)

        self.reverb_room = _spin(0.0, 1.0, self.effects.reverb_room_size, step=0.05)
        self.reverb_damping = _spin(0.0, 1.0, self.effects.reverb_damping, step=0.05)
        self.reverb_wet = _spin(0.0, 1.0, self.effects.reverb_wet, step=0.05)
        form.addRow("Room size", self.reverb_room)
        form.addRow("Damping", self.reverb_damping)
        form.addRow("Mix (wet)", self.reverb_wet)
        return w

    def _build_compressor_tab(self) -> QWidget:
        w = QWidget()
        form = QFormLayout(w)
        self.comp_enabled = QCheckBox("Enable Compressor")
        self.comp_enabled.setChecked(self.effects.compressor_enabled)
        form.addRow(self.comp_enabled)

        self.comp_threshold = _spin(-60.0, 0.0, self.effects.comp_threshold_db, step=1)
        self.comp_ratio = _spin(1.0, 20.0, self.effects.comp_ratio, step=0.5)
        self.comp_attack = _spin(0.1, 200.0, self.effects.comp_attack_ms, step=1)
        self.comp_release = _spin(1.0, 1000.0, self.effects.comp_release_ms, step=5)
        self.comp_makeup = _spin(0.0, 24.0, self.effects.comp_makeup_db, step=0.5)
        form.addRow("Threshold (dB)", self.comp_threshold)
        form.addRow("Ratio", self.comp_ratio)
        form.addRow("Attack (ms)", self.comp_attack)
        form.addRow("Release (ms)", self.comp_release)
        form.addRow("Makeup gain (dB)", self.comp_makeup)
        return w

    def _build_autotune_tab(self) -> QWidget:
        w = QWidget()
        form = QFormLayout(w)
        self.autotune_enabled = QCheckBox("Enable Autotune")
        self.autotune_enabled.setChecked(self.effects.autotune_enabled)
        form.addRow(self.autotune_enabled)

        self.autotune_key = QComboBox()
        self.autotune_key.addItems(NOTE_NAMES)
        self.autotune_key.setCurrentText(self.effects.autotune_key)
        form.addRow("Key", self.autotune_key)

        self.autotune_scale = QComboBox()
        self.autotune_scale.addItems(list(SCALES.keys()))
        self.autotune_scale.setCurrentText(self.effects.autotune_scale)
        form.addRow("Scale", self.autotune_scale)

        self.autotune_strength = _spin(0.0, 1.0, self.effects.autotune_strength, step=0.05)
        self.autotune_speed = _spin(0.01, 1.0, self.effects.autotune_speed, step=0.05)
        form.addRow("Strength", self.autotune_strength)
        form.addRow("Retune speed", self.autotune_speed)

        form.addRow(QLabel("Tip: speed near 1.0 = classic robotic/T-Pain snap."))
        return w

    def apply_to(self, effects: EffectSettings) -> None:
        effects.eq_enabled = self.eq_enabled.isChecked()
        for band, box in zip(effects.eq.bands, self.eq_gain_boxes):
            band.gain_db = box.value()

        effects.delay_enabled = self.delay_enabled.isChecked()
        effects.delay_ms = self.delay_ms.value()
        effects.delay_feedback = self.delay_feedback.value()
        effects.delay_mix = self.delay_mix.value()

        effects.reverb_enabled = self.reverb_enabled.isChecked()
        effects.reverb_room_size = self.reverb_room.value()
        effects.reverb_damping = self.reverb_damping.value()
        effects.reverb_wet = self.reverb_wet.value()

        effects.compressor_enabled = self.comp_enabled.isChecked()
        effects.comp_threshold_db = self.comp_threshold.value()
        effects.comp_ratio = self.comp_ratio.value()
        effects.comp_attack_ms = self.comp_attack.value()
        effects.comp_release_ms = self.comp_release.value()
        effects.comp_makeup_db = self.comp_makeup.value()

        effects.autotune_enabled = self.autotune_enabled.isChecked()
        effects.autotune_key = self.autotune_key.currentText()
        effects.autotune_scale = self.autotune_scale.currentText()
        effects.autotune_strength = self.autotune_strength.value()
        effects.autotune_speed = self.autotune_speed.value()
