import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from audio import settings as settings_store
from audio.settings import AppSettings


def test_settings_roundtrip(tmp_path, monkeypatch):
    monkeypatch.setattr(settings_store, "SETTINGS_PATH", tmp_path / "settings.json")

    settings = AppSettings(input_device=2, output_device=5)
    settings_store.save_settings(settings)

    loaded = settings_store.load_settings()
    assert loaded.input_device == 2
    assert loaded.output_device == 5


def test_settings_defaults_when_missing(tmp_path, monkeypatch):
    monkeypatch.setattr(settings_store, "SETTINGS_PATH", tmp_path / "does_not_exist.json")
    loaded = settings_store.load_settings()
    assert loaded.input_device is None
    assert loaded.output_device is None


def test_settings_defaults_on_corrupt_file(tmp_path, monkeypatch):
    path = tmp_path / "settings.json"
    path.write_text("not valid json{{{")
    monkeypatch.setattr(settings_store, "SETTINGS_PATH", path)
    loaded = settings_store.load_settings()
    assert loaded.input_device is None
    assert loaded.output_device is None
