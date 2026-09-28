# Offline Music Studio

Ein eigenständiges, komplett offline laufendes Musik-Aufnahme-Programm mit
Mehrspur-Mixer, Autotune und Studio-Effekten. Es ist bewusst als fokussierter
Kern gebaut (Aufnahme, Autotune, EQ/Delay/Reverb/Compressor, Mixdown/Export) —
nicht als 1:1-Klon von FL Studio (das wäre ein mehrjähriges Projekt mit
hunderten Plugins, VST-Host, Piano-Roll-Sequencer etc.), sondern als solide,
erweiterbare Basis dafür.

## Features

- **Offline-Aufnahme**: Mikrofon direkt in einen Track aufnehmen, keine
  Internetverbindung nötig.
- **Autotune / Pitch-Correction**: Tonhöhe automatisch auf eine Tonart/Skala
  (chromatisch, Dur, Moll) einrasten, mit einstellbarer Stärke ("strength")
  und Korrekturgeschwindigkeit ("speed" — niedrig = natürliches Gleiten,
  hoch = robotischer "T-Pain"-Sound).
- **Studio-Effekte** pro Track:
  - Parametrischer 3-Band-EQ (Low-Shelf, Peak, High-Shelf)
  - Delay/Echo mit Feedback und Mix
  - Reverb (Schroeder-Algorithmus: Kammfilter + Allpassfilter)
  - Kompressor (Attack/Release/Ratio/Threshold/Makeup-Gain)
- **Mehrspur-Mixer**: beliebig viele Tracks, je mit Lautstärke, Panorama,
  Mute/Solo, eigener Effektkette.
- **Export**: Mixdown als WAV (immer) oder MP3 (mit `pydub` + installiertem
  `ffmpeg`).
- **Projekte speichern/laden**: Tracks + Einstellungen als Ordner mit
  `project.json` + WAV-Dateien.

## Installation

```bash
cd music-studio
python -m venv .venv
source .venv/bin/activate  # Windows: .venv\Scripts\activate
pip install -r requirements.txt
```

Für MP3-Export zusätzlich:

```bash
pip install pydub
# und ffmpeg systemweit installieren (z.B. `apt install ffmpeg` / `brew install ffmpeg`)
```

## Starten

```bash
python main.py
```

## Bedienung

1. **+ Add Track** – neuen leeren Track anlegen.
2. **Record** – Mikrofonaufnahme starten/stoppen (verwendet dein
   Standard-Eingabegerät).
3. **Load File** – vorhandene WAV/FLAC/OGG-Datei in den Track laden.
4. **Effects...** – EQ, Delay, Reverb, Compressor und Autotune für diesen
   Track einstellen.
5. **Vol/Pan-Regler** und **Mute/Solo** wie in jedem DAW-Mixer.
6. **Play Mix** – aktuellen Mixdown anhören, **Stop** zum Abbrechen.
7. **Export Mixdown...** – fertigen Track als WAV/MP3 exportieren.
8. **Save/Open Project...** – Session in einen Ordner speichern bzw. laden.

## Architektur

```
music-studio/
  audio/
    recorder.py    Mikrofonaufnahme (sounddevice)
    effects.py     EQ, Delay, Reverb, Compressor, Limiter (numpy/scipy)
    autotune.py    Pitch-Detection (librosa pYIN) + Pitch-Correction
    mixer.py       Track/Mixer-Klassen, Rendering, Export
    project.py     Speichern/Laden von Projekten
  gui/
    main_window.py     Hauptfenster, Transport, Track-Liste
    track_widget.py    Eine Track-Zeile (Record/Load/Vol/Pan/Mute/Solo)
    effects_dialog.py  Effekt-Editor pro Track
  tests/
    test_effects.py    DSP-Effekte-Tests
    test_autotune.py   Pitch-Correction-Test
  main.py          Einstiegspunkt
```

Alle Effekte sind von Grund auf mit numpy/scipy implementiert (RBJ-Biquad-EQ,
Schroeder-Reverb, Hüllkurven-Kompressor) — keine externen Audio-Plugin-SDKs
oder Cloud-Dienste nötig, alles läuft lokal und offline.

## Grenzen (bewusst nicht enthalten)

Kein Piano-Roll-Sequencer, kein MIDI/VST-Hosting, keine hunderten
Instrumenten-Presets — das sind eigene, sehr große Feature-Bereiche. Die
Architektur (klare `Track`/`Mixer`/Effekt-Klassen) ist aber so angelegt,
dass sich weitere Effekte oder ein Sequencer später ergänzen lassen.
