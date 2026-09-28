# Offline Music Studio

Ein eigenständiges, komplett offline laufendes Musik-Aufnahme-Programm mit
Mehrspur-Mixer, Autotune und Studio-Effekten. Es ist bewusst als fokussierter
Kern gebaut (Aufnahme, Autotune, EQ/Delay/Reverb/Compressor, Mixdown/Export) —
nicht als 1:1-Klon von FL Studio (das wäre ein mehrjähriges Projekt mit
hunderten Plugins, VST-Host, Piano-Roll-Sequencer etc.), sondern als solide,
erweiterbare Basis dafür.

## Features

- **Offline-Aufnahme**: Mikrofon direkt in einen Track aufnehmen, keine
  Internetverbindung nötig — mit **Pegelanzeige**, **Aufnahme-Timer** und
  **Wellenform-Vorschau**, damit immer sichtbar ist, wie viel und wie laut
  gerade aufgenommen wurde.
  - **🎧 Monitor**: sich selbst live über Kopfhörer/Lautsprecher hören
    während der Aufnahme (vor dem Aufnehmen aktivieren; bei Lautsprechern statt
    Kopfhörer kann es zu Rückkopplung/Echo kommen).
- **Wellenform zum Schneiden**: Bereich per Drag auf der Wellenform markieren
  (wie in FL Studios Playlist) und mit **✂ Cut Selection** direkt heraus-
  schneiden — der Track wird sichtbar kürzer, der Rest rückt zusammen.
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
  - **Schnellregler direkt am Track**: Autotune an/aus, Strength und Speed,
    ohne den Effekt-Dialog öffnen zu müssen (voller Regelsatz bleibt im
    Effects-Dialog verfügbar)
- **Studio-Effekte** pro Track:
  - Parametrischer 3-Band-EQ (Low-Shelf, Peak, High-Shelf)
  - Delay/Echo mit Feedback und Mix
  - Reverb (Schroeder-Algorithmus: Kammfilter + Allpassfilter)
  - Kompressor (Attack/Release/Ratio/Threshold/Makeup-Gain)
- **Voice FX — Effekte, die es in klassischem Autotune nicht gibt**:
  - **De-Esser**: zähmt harte "S"/"Sch"-Laute, ohne die Stimme dumpf zu machen
  - **Doubler**: legt leicht verstimmte/verzögerte Kopien der Stimme übereinander
    für einen dickeren, breiteren "doppelt eingesungenen" Sound
  - **Harmonie-Generator** ("🎤 Add Harmony..."): erzeugt aus einer Lead-Stimme
    automatisch zusätzliche Backing-Vocal-Spuren (Terz/Quinte/Sexte/Oktave
    über oder unter der Lead), die sich an Tonart/Skala halten — komplett neue
    Spuren, kein reiner Effekt
- **Mehrspur-Mixer**: beliebig viele Tracks, je mit Lautstärke, Panorama,
  Mute/Solo, eigener Effektkette.
- **Alle gängigen Audioformate** zum Laden und Exportieren: WAV, MP3, FLAC,
  OGG, AIFF, M4A/AAC, WMA. WAV/FLAC/OGG/AIFF funktionieren direkt; MP3/M4A/AAC/
  WMA brauchen zusätzlich ein installiertes `ffmpeg` auf dem System (siehe
  Installation).
- **Projekte speichern/laden**: Tracks + Einstellungen als Ordner mit
  `project.json` + WAV-Dateien.

## Installation

### Windows-Installer (empfohlen, kein Terminal nötig)

Ein fertiges `OfflineMusicStudioSetup.exe` wird automatisch von GitHub Actions
gebaut (`.github/workflows/build-windows-installer.yml`, PyInstaller + Inno
Setup) — kein Python, kein PowerShell auf deinem Rechner nötig:

1. Im GitHub-Repo auf **Actions → Build Windows Installer → Run workflow**
   klicken (oder es läuft automatisch bei Änderungen an `music-studio/`).
2. Nach ein paar Minuten ist der Lauf grün → im Abschnitt **Artifacts** die
   Datei `OfflineMusicStudioSetup` herunterladen und entpacken.
3. `OfflineMusicStudioSetup.exe` doppelklicken → normaler Windows-Installer
   (Weiter/Weiter/Fertig), erstellt Startmenü- und optional Desktop-Icon.
4. Danach einfach "Offline Music Studio" im Startmenü öffnen.

Hinweis: WAV/FLAC/OGG/AIFF funktionieren im Installer direkt. Für MP3/M4A/AAC/
WMA zusätzlich einmalig `ffmpeg` installieren (z.B. `winget install ffmpeg`),
da das nicht mitinstalliert wird.

### Manuell mit Python (Entwickler / Mac / Linux)

```bash
cd music-studio
python -m venv .venv
source .venv/bin/activate  # Windows: .venv\Scripts\activate
pip install -r requirements.txt
```

Für MP3/M4A/AAC/WMA (laden **und** exportieren) zusätzlich `ffmpeg` systemweit
installieren (WAV/FLAC/OGG/AIFF brauchen das nicht):

```bash
# Windows (PowerShell):  winget install ffmpeg
# macOS:                 brew install ffmpeg
# Linux:                 sudo apt install ffmpeg
```

## Starten

```bash
python main.py
```

## Bedienung

1. **+ Add Track** – neuen leeren Track anlegen.
2. **Record** – Mikrofonaufnahme starten/stoppen (verwendet dein
   Standard-Eingabegerät). **🎧** daneben aktiviert Live-Monitoring (dich
   selbst hören während der Aufnahme).
3. **Wellenform** – nach der Aufnahme/dem Laden per Drag einen Bereich
   markieren, dann **✂ Cut Selection** zum Herausschneiden oder
   **Clear Selection** zum Abbrechen der Auswahl.
4. **Load File** – vorhandene Audiodatei in den Track laden (WAV, MP3, FLAC,
   OGG, AIFF, M4A/AAC, WMA).
5. **Effects / Autotune...** – EQ, Delay, Reverb, Compressor, Autotune und
   Voice FX (De-Esser, Doubler) für diesen Track einstellen. Im Autotune-Tab
   oben das Preset-Dropdown nutzen, um eigene oder mitgelieferte Einstellungen
   zu laden/speichern/löschen.
6. **Autotune-Zeile** direkt am Track – schneller Ein/Aus-Schalter plus
   Strength/Speed, für schnelle Anpassungen ohne Dialog.
7. **Vol/Pan-Regler** und **Mute/Solo** wie in jedem DAW-Mixer.
8. **Play Mix** – aktuellen Mixdown anhören, **Stop** zum Abbrechen.
9. **🎤 Add Harmony...** – aus einer Lead-Stimme automatisch neue
   Harmonie-Spuren erzeugen (Terz/Quinte/... über oder unter der Lead).
10. **Export Mixdown...** – fertigen Track als WAV/MP3/FLAC/OGG/AIFF/M4A/WMA
   exportieren.
11. **Save/Open Project...** – Session in einen Ordner speichern bzw. laden.

## Architektur

```
music-studio/
  audio/
    recorder.py    Mikrofonaufnahme + Pegel/Timer + Live-Monitoring (sounddevice)
    effects.py     EQ, Delay, Reverb, Compressor, Limiter, Doubler, De-Esser
    autotune.py    Pitch-Detection (librosa pYIN) + Pitch-Correction
    harmony.py     Harmonie-Generator (diatonische Transposition der Lead-Stimme)
    presets.py     Autotune-Presets speichern/laden/löschen (~/.music_studio/presets)
    io_formats.py  Laden/Exportieren aller Audioformate (WAV/MP3/FLAC/OGG/...)
    mixer.py       Track/Mixer-Klassen, Rendering, Export
    project.py     Speichern/Laden von Projekten
  gui/
    theme.py           Dark-Theme (Farben, Stylesheet)
    main_window.py     Hauptfenster, Transport, Track-Liste, Harmonie-Aktion
    track_widget.py    Eine Track-Zeile (Record/Load/Vol/Pan/Mute/Solo, Meter,
                       Wellenform, Autotune-Schnellregler)
    waveform_widget.py Wellenform-Vorschau + Auswahl/Cut per Drag
    effects_dialog.py  Effekt-Editor (EQ/Delay/Reverb/Compressor/Autotune/Voice FX)
    harmony_dialog.py  Auswahl-Dialog für den Harmonie-Generator
  tests/
    test_effects.py    DSP-Effekte-Tests
    test_autotune.py   Pitch-Correction-Tests (Skalen, Humanize, Formant, Referenz)
    test_presets.py    Preset-Speichern/Laden-Tests
    test_io_formats.py Format-Roundtrip-Tests (WAV/FLAC immer, MP3 falls ffmpeg da ist)
    test_voice_fx.py   Doubler/De-Esser/Harmonie-Tests
    test_recorder.py   Monitor-Callback-Tests
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
