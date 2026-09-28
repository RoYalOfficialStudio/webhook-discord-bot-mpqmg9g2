import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from audio import presets as preset_store
from audio.mixer import EffectSettings


def test_preset_roundtrip(tmp_path, monkeypatch):
    monkeypatch.setattr(preset_store, "PRESETS_DIR", tmp_path / "autotune")

    effects = EffectSettings()
    effects.autotune_key = "F#"
    effects.autotune_scale = "dorian"
    effects.autotune_strength = 0.7
    effects.autotune_speed = 0.2
    effects.autotune_humanize = 0.5
    effects.autotune_formant_preserve = True
    effects.autotune_reference_hz = 442.0
    effects.autotune_vibrato_depth = 0.2
    effects.autotune_vibrato_rate = 6.0

    preset_store.save_preset("My Test Preset", effects)
    assert "My Test Preset" in preset_store.list_presets()

    data = preset_store.load_preset("My Test Preset")
    assert data["autotune_key"] == "F#"
    assert data["autotune_scale"] == "dorian"
    assert data["autotune_reference_hz"] == 442.0

    fresh = EffectSettings()
    preset_store.apply_preset(fresh, data)
    assert fresh.autotune_key == "F#"
    assert fresh.autotune_humanize == 0.5

    assert preset_store.delete_preset("My Test Preset") is True
    assert "My Test Preset" not in preset_store.list_presets()


def test_factory_presets_are_listed_and_cannot_be_deleted(tmp_path, monkeypatch):
    monkeypatch.setattr(preset_store, "PRESETS_DIR", tmp_path / "autotune")

    names = preset_store.list_presets()
    assert "Classic Snap (T-Pain)" in names

    data = preset_store.load_preset("Classic Snap (T-Pain)")
    assert data["autotune_strength"] == 1.0

    assert preset_store.delete_preset("Classic Snap (T-Pain)") is False
