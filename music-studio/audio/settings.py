"""Persisted app-wide settings: which audio input/output device to use."""
from __future__ import annotations

import json
from dataclasses import dataclass, asdict
from pathlib import Path

SETTINGS_PATH = Path.home() / ".music_studio" / "settings.json"


@dataclass
class AppSettings:
    input_device: int | None = None
    output_device: int | None = None


def load_settings() -> AppSettings:
    try:
        with open(SETTINGS_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        return AppSettings(
            input_device=data.get("input_device"),
            output_device=data.get("output_device"),
        )
    except (FileNotFoundError, json.JSONDecodeError, OSError, ValueError):
        return AppSettings()


def save_settings(settings: AppSettings) -> None:
    SETTINGS_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(SETTINGS_PATH, "w", encoding="utf-8") as f:
        json.dump(asdict(settings), f, indent=2)
