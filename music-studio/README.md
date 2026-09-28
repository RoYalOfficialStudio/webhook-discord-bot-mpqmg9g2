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
- **Autotune / Pitch-Correction** – mit allen gängigen Reglern:
  - Tonart + 12 Skalen (Dur, natürlich/harmonisch/melodisch Moll, Dorisch,
    Phrygisch, Lydisch, Mixolydisch, Lokrisch, Pentatonik, Blues, Chromatisch)
    oder eine **eigene Skala** (einzelne Noten frei an/abwählen)
  - **Strength**: wie stark korrigiert wird (0 = trocken, 1 = volles Einrasten)
  - **Retune Speed**: wie schnell die Korrektur nachzieht (langsam = natürliches
    Gleiten, schnell = robotischer "T-Pain"-Sound)
  - **Humanize**: lässt natürliches Vibrato/Bewegung eher in Ruhe, statt es
    platt zu bügeln
  - **Formant-Erhalt**: verhindert den "Chipmunk"-Klang bei größeren Korrekturen
  - **Referenz-Stimmung**: A4 frei einstellbar (z.B. 442 Hz statt 440 Hz)
  - **Vibrato**: synthetisches Vibrato (Tiefe/Rate) nach der Korrektur wieder
    draufsetzen
  - **Presets**: eigene Einstellungen benennen und speichern, jederzeit wieder
    laden oder löschen; ein paar Werks-Presets sind vorinstalliert
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
4. **Effects / Autotune...** – EQ, Delay, Reverb, Compressor und Autotune für
   diesen Track einstellen. Im Autotune-Tab oben das Preset-Dropdown nutzen,
   um eigene oder mitgelieferte Einstellungen zu laden/speichern/löschen.
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
    presets.py     Autotune-Presets speichern/laden/löschen (~/.music_studio/presets)
    mixer.py       Track/Mixer-Klassen, Rendering, Export
    project.py     Speichern/Laden von Projekten
  gui/
    theme.py           Dark-Theme (Farben, Stylesheet)
    main_window.py     Hauptfenster, Transport, Track-Liste
    track_widget.py    Eine Track-Zeile (Record/Load/Vol/Pan/Mute/Solo)
    effects_dialog.py  Effekt-Editor + Autotune-Presets pro Track
  tests/
    test_effects.py    DSP-Effekte-Tests
    test_autotune.py   Pitch-Correction-Tests (Skalen, Humanize, Formant, Referenz)
    test_presets.py    Preset-Speichern/Laden-Tests
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
