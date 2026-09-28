"""Dialog for editing a track's effect chain: EQ, delay, reverb, compressor, autotune."""
from __future__ import annotations

from PySide6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QFormLayout, QTabWidget, QWidget, QCheckBox,
    QDoubleSpinBox, QComboBox, QDialogButtonBox, QLabel, QPushButton, QGridLayout,
    QInputDialog, QMessageBox,
)

from audio.effects import EQBand, ParametricEQ
from audio.autotune import NOTE_NAMES, SCALES
from audio.mixer import EffectSettings
from audio import presets as preset_store


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
        self._last_preset_name = effects.autotune_preset_name
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
        tabs.addTab(self._build_voice_fx_tab(), "Voice FX")

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

    def _build_voice_fx_tab(self) -> QWidget:
        w = QWidget()
        outer = QVBoxLayout(w)

        outer.addWidget(QLabel("De-Esser — tames harsh \"s\"/\"sh\" sibilance without dulling the voice"))
        deesser_form = QFormLayout()
        outer.addLayout(deesser_form)

        self.deesser_enabled = QCheckBox("Enable De-Esser")
        self.deesser_enabled.setChecked(self.effects.deesser_enabled)
        deesser_form.addRow(self.deesser_enabled)

        self.deesser_freq = _spin(2000.0, 12000.0, self.effects.deesser_freq, step=100, decimals=0)
        self.deesser_bandwidth = _spin(500.0, 8000.0, self.effects.deesser_bandwidth, step=100, decimals=0)
        self.deesser_threshold = _spin(-60.0, 0.0, self.effects.deesser_threshold_db, step=1)
        self.deesser_ratio = _spin(1.0, 20.0, self.effects.deesser_ratio, step=0.5)
        deesser_form.addRow("Center frequency (Hz)", self.deesser_freq)
        deesser_form.addRow("Bandwidth (Hz)", self.deesser_bandwidth)
        deesser_form.addRow("Threshold (dB)", self.deesser_threshold)
        deesser_form.addRow("Ratio", self.deesser_ratio)

        outer.addWidget(QLabel("Doubler — layers detuned/delayed copies for a thicker, wider \"double-tracked\" voice"))
        doubler_form = QFormLayout()
        outer.addLayout(doubler_form)

        self.doubler_enabled = QCheckBox("Enable Doubler")
        self.doubler_enabled.setChecked(self.effects.doubler_enabled)
        doubler_form.addRow(self.doubler_enabled)

        self.doubler_voices = _spin(1, 4, self.effects.doubler_voices, step=1, decimals=0)
        self.doubler_detune = _spin(1.0, 50.0, self.effects.doubler_detune_cents, step=1)
        self.doubler_delay = _spin(1.0, 60.0, self.effects.doubler_delay_ms, step=1)
        self.doubler_mix = _spin(0.0, 1.0, self.effects.doubler_mix, step=0.05)
        doubler_form.addRow("Voices", self.doubler_voices)
        doubler_form.addRow("Detune (cents)", self.doubler_detune)
        doubler_form.addRow("Delay (ms)", self.doubler_delay)
        doubler_form.addRow("Mix (wet)", self.doubler_mix)

        return w

    def _build_autotune_tab(self) -> QWidget:
        w = QWidget()
        outer = QVBoxLayout(w)

        outer.addLayout(self._build_preset_row())

        form = QFormLayout()
        outer.addLayout(form)

        self.autotune_enabled = QCheckBox("Enable Autotune")
        self.autotune_enabled.setChecked(self.effects.autotune_enabled)
        form.addRow(self.autotune_enabled)

        self.autotune_key = QComboBox()
        self.autotune_key.addItems(NOTE_NAMES)
        self.autotune_key.setCurrentText(self.effects.autotune_key)
        form.addRow("Key", self.autotune_key)

        self.autotune_scale = QComboBox()
        self.autotune_scale.addItems(list(SCALES.keys()) + ["custom"])
        self.autotune_scale.setCurrentText(self.effects.autotune_scale)
        self.autotune_scale.currentTextChanged.connect(self._on_scale_changed)
        form.addRow("Scale", self.autotune_scale)

        self.autotune_strength = _spin(0.0, 1.0, self.effects.autotune_strength, step=0.05)
        self.autotune_speed = _spin(0.01, 1.0, self.effects.autotune_speed, step=0.05)
        self.autotune_humanize = _spin(0.0, 1.0, self.effects.autotune_humanize, step=0.05)
        form.addRow("Strength", self.autotune_strength)
        form.addRow("Retune speed", self.autotune_speed)
        form.addRow("Humanize (keep natural vibrato)", self.autotune_humanize)

        self.autotune_formant = QCheckBox("Preserve formants (avoid \"chipmunk\" sound)")
        self.autotune_formant.setChecked(self.effects.autotune_formant_preserve)
        form.addRow(self.autotune_formant)

        self.autotune_reference = _spin(415.0, 466.0, self.effects.autotune_reference_hz, step=0.5, decimals=1)
        form.addRow("Reference pitch A4 (Hz)", self.autotune_reference)

        self.autotune_vibrato_depth = _spin(0.0, 2.0, self.effects.autotune_vibrato_depth, step=0.05)
        self.autotune_vibrato_rate = _spin(0.5, 12.0, self.effects.autotune_vibrato_rate, step=0.1)
        form.addRow("Vibrato depth (semitones)", self.autotune_vibrato_depth)
        form.addRow("Vibrato rate (Hz)", self.autotune_vibrato_rate)

        outer.addWidget(self._build_custom_scale_grid())
        self._on_scale_changed(self.autotune_scale.currentText())

        outer.addWidget(QLabel("Tip: speed near 1.0 = classic robotic/T-Pain snap."))
        return w

    def _build_preset_row(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.addWidget(QLabel("Preset:"))

        self.preset_combo = QComboBox()
        self._reload_preset_list()
        row.addWidget(self.preset_combo, 1)

        load_btn = QPushButton("Load")
        load_btn.clicked.connect(self._load_selected_preset)
        row.addWidget(load_btn)

        save_btn = QPushButton("Save As...")
        save_btn.clicked.connect(self._save_as_preset)
        row.addWidget(save_btn)

        delete_btn = QPushButton("Delete")
        delete_btn.clicked.connect(self._delete_selected_preset)
        row.addWidget(delete_btn)

        return row

    def _reload_preset_list(self) -> None:
        current = self.preset_combo.currentText() if hasattr(self, "preset_combo") else ""
        self.preset_combo.clear()
        self.preset_combo.addItems(preset_store.list_presets())
        if current:
            idx = self.preset_combo.findText(current)
            if idx >= 0:
                self.preset_combo.setCurrentIndex(idx)

    def _load_selected_preset(self) -> None:
        name = self.preset_combo.currentText()
        if not name:
            return
        data = preset_store.load_preset(name)
        self.autotune_key.setCurrentText(data.get("autotune_key", "C"))
        self.autotune_scale.setCurrentText(data.get("autotune_scale", "major"))
        self.autotune_strength.setValue(data.get("autotune_strength", 1.0))
        self.autotune_speed.setValue(data.get("autotune_speed", 0.35))
        self.autotune_humanize.setValue(data.get("autotune_humanize", 0.0))
        self.autotune_formant.setChecked(data.get("autotune_formant_preserve", False))
        self.autotune_reference.setValue(data.get("autotune_reference_hz", 440.0))
        self.autotune_vibrato_depth.setValue(data.get("autotune_vibrato_depth", 0.0))
        self.autotune_vibrato_rate.setValue(data.get("autotune_vibrato_rate", 5.0))
        custom_notes = data.get("autotune_custom_notes", [])
        for pc, box in self.custom_note_boxes.items():
            box.setChecked(pc in custom_notes)
        self._last_preset_name = name

    def _save_as_preset(self) -> None:
        name, ok = QInputDialog.getText(self, "Save Preset", "Preset name:")
        if not ok or not name.strip():
            return
        temp = EffectSettings(eq=ParametricEQ())
        self.apply_to(temp)
        preset_store.save_preset(name.strip(), temp)
        self._last_preset_name = name.strip()
        self._reload_preset_list()
        idx = self.preset_combo.findText(name.strip())
        if idx >= 0:
            self.preset_combo.setCurrentIndex(idx)

    def _delete_selected_preset(self) -> None:
        name = self.preset_combo.currentText()
        if not name:
            return
        if preset_store.delete_preset(name):
            self._reload_preset_list()
        else:
            QMessageBox.information(self, "Can't delete", "Factory presets can't be deleted.")

    def _build_custom_scale_grid(self) -> QWidget:
        box = QWidget()
        grid = QGridLayout(box)
        grid.addWidget(QLabel("Custom scale notes (used when Scale = custom):"), 0, 0, 1, 6)
        self.custom_note_boxes = {}
        selected = set(self.effects.autotune_custom_notes)
        for i, note in enumerate(NOTE_NAMES):
            cb = QCheckBox(note)
            cb.setChecked(i in selected)
            self.custom_note_boxes[i] = cb
            grid.addWidget(cb, 1 + i // 6, i % 6)
        self._custom_scale_box = box
        return box

    def _on_scale_changed(self, text: str) -> None:
        if hasattr(self, "_custom_scale_box"):
            self._custom_scale_box.setEnabled(text == "custom")

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
        effects.autotune_custom_notes = [pc for pc, box in self.custom_note_boxes.items() if box.isChecked()]
        effects.autotune_strength = self.autotune_strength.value()
        effects.autotune_speed = self.autotune_speed.value()
        effects.autotune_humanize = self.autotune_humanize.value()
        effects.autotune_formant_preserve = self.autotune_formant.isChecked()
        effects.autotune_reference_hz = self.autotune_reference.value()
        effects.autotune_vibrato_depth = self.autotune_vibrato_depth.value()
        effects.autotune_vibrato_rate = self.autotune_vibrato_rate.value()
        effects.autotune_preset_name = self._last_preset_name

        effects.deesser_enabled = self.deesser_enabled.isChecked()
        effects.deesser_freq = self.deesser_freq.value()
        effects.deesser_bandwidth = self.deesser_bandwidth.value()
        effects.deesser_threshold_db = self.deesser_threshold.value()
        effects.deesser_ratio = self.deesser_ratio.value()

        effects.doubler_enabled = self.doubler_enabled.isChecked()
        effects.doubler_voices = int(self.doubler_voices.value())
        effects.doubler_detune_cents = self.doubler_detune.value()
        effects.doubler_delay_ms = self.doubler_delay.value()
        effects.doubler_mix = self.doubler_mix.value()
