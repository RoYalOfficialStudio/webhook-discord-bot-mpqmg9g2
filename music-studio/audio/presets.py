"""Save/load/list/delete named Autotune presets as JSON files.

Stored under a fixed per-user folder (not the project folder) so presets
carry over between projects and app restarts, like preset browsers in
commercial plugins.
"""
from __future__ import annotations

import json
import re
from pathlib import Path
from dataclasses import asdict, fields

PRESETS_DIR = Path.home() / ".music_studio" / "presets" / "autotune"

_AUTOTUNE_FIELDS = [
    "autotune_key",
    "autotune_scale",
    "autotune_custom_notes",
    "autotune_strength",
    "autotune_speed",
    "autotune_humanize",
    "autotune_formant_preserve",
    "autotune_reference_hz",
    "autotune_vibrato_depth",
    "autotune_vibrato_rate",
]

# A few ready-made starting points, in the same spirit as a plugin's factory presets.
FACTORY_PRESETS: dict[str, dict] = {
    "Classic Snap (T-Pain)": {
        "autotune_key": "C", "autotune_scale": "major", "autotune_custom_notes": [],
        "autotune_strength": 1.0, "autotune_speed": 1.0, "autotune_humanize": 0.0,
        "autotune_formant_preserve": False, "autotune_reference_hz": 440.0,
        "autotune_vibrato_depth": 0.0, "autotune_vibrato_rate": 5.0,
    },
    "Natural Vocal Tightening": {
        "autotune_key": "C", "autotune_scale": "major", "autotune_custom_notes": [],
        "autotune_strength": 0.6, "autotune_speed": 0.15, "autotune_humanize": 0.6,
        "autotune_formant_preserve": True, "autotune_reference_hz": 440.0,
        "autotune_vibrato_depth": 0.0, "autotune_vibrato_rate": 5.0,
    },
    "Subtle Pitch Safety Net": {
        "autotune_key": "C", "autotune_scale": "chromatic", "autotune_custom_notes": [],
        "autotune_strength": 0.35, "autotune_speed": 0.08, "autotune_humanize": 0.8,
        "autotune_formant_preserve": True, "autotune_reference_hz": 440.0,
        "autotune_vibrato_depth": 0.0, "autotune_vibrato_rate": 5.0,
    },
    "Robotic + Vibrato": {
        "autotune_key": "A", "autotune_scale": "minor", "autotune_custom_notes": [],
        "autotune_strength": 1.0, "autotune_speed": 0.9, "autotune_humanize": 0.0,
        "autotune_formant_preserve": False, "autotune_reference_hz": 440.0,
        "autotune_vibrato_depth": 0.3, "autotune_vibrato_rate": 6.0,
    },
}


def _safe_filename(name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9 _-]", "", name).strip()
    return cleaned or "preset"


def list_presets() -> list[str]:
    PRESETS_DIR.mkdir(parents=True, exist_ok=True)
    user_presets = sorted(p.stem for p in PRESETS_DIR.glob("*.json"))
    return list(FACTORY_PRESETS.keys()) + user_presets


def save_preset(name: str, effects) -> None:
    PRESETS_DIR.mkdir(parents=True, exist_ok=True)
    data = {f: getattr(effects, f) for f in _AUTOTUNE_FIELDS}
    path = PRESETS_DIR / f"{_safe_filename(name)}.json"
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)


def load_preset(name: str) -> dict:
    if name in FACTORY_PRESETS:
        return dict(FACTORY_PRESETS[name])
    path = PRESETS_DIR / f"{_safe_filename(name)}.json"
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def delete_preset(name: str) -> bool:
    if name in FACTORY_PRESETS:
        return False
    path = PRESETS_DIR / f"{_safe_filename(name)}.json"
    if path.exists():
        path.unlink()
        return True
    return False


def apply_preset(effects, data: dict) -> None:
    for f in _AUTOTUNE_FIELDS:
        if f in data:
            setattr(effects, f, data[f])
