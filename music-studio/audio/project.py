"""Save/load a project: track audio as WAV files next to a JSON manifest."""
from __future__ import annotations

import json
import os
from dataclasses import asdict

import soundfile as sf

from .effects import EQBand, ParametricEQ
from .mixer import EffectSettings, Mixer, Track


def save_project(mixer: Mixer, folder: str) -> None:
    os.makedirs(folder, exist_ok=True)
    manifest = {"sr": mixer.sr, "tracks": []}

    for i, track in enumerate(mixer.tracks):
        wav_name = f"track_{i:02d}.wav"
        sf.write(os.path.join(folder, wav_name), track.audio, track.sr)

        fx = asdict(track.effects)
        fx["eq"] = {"bands": [asdict(b) for b in track.effects.eq.bands]} if track.effects.eq else None

        manifest["tracks"].append({
            "name": track.name,
            "file": wav_name,
            "sr": track.sr,
            "volume": track.volume,
            "pan": track.pan,
            "mute": track.mute,
            "solo": track.solo,
            "effects": fx,
        })

    with open(os.path.join(folder, "project.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)


def load_project(folder: str) -> Mixer:
    with open(os.path.join(folder, "project.json"), "r", encoding="utf-8") as f:
        manifest = json.load(f)

    mixer = Mixer(sr=manifest["sr"])
    for t in manifest["tracks"]:
        audio, sr = sf.read(os.path.join(folder, t["file"]), dtype="float32")

        fx_data = dict(t["effects"])
        eq_data = fx_data.pop("eq", None)
        eq = ParametricEQ(bands=[EQBand(**b) for b in eq_data["bands"]]) if eq_data else None
        effects = EffectSettings(eq=eq, **fx_data)

        track = Track(
            name=t["name"], audio=audio, sr=sr,
            volume=t["volume"], pan=t["pan"], mute=t["mute"], solo=t["solo"],
            effects=effects,
        )
        mixer.add_track(track)

    return mixer
