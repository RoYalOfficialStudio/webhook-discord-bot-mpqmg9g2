# RoY STUDIO – Bestandsaufnahme des Entwicklungsstands

Erstellt: 2026-10-28. Untersuchter Stand: Git-Commit `c2a64d0` auf Branch `claude/bold-franklin-fybkj5`. Der Arbeitsbaum war sauber (`git status` ohne Änderungen).

**Wichtiger Hinweis zum Projektpfad.**
- Der angegebene Pfad `C:\Users\Anwender\Desktop\RoY Music\RoY_STUDIO` liegt auf dem Windows-PC des Nutzers. Von dieser Cloud-Umgebung aus ist er **nicht erreichbar**.
- Untersucht wurde das Git-Repository `webhook-discord-bot-mpqmg9g2`, Unterordner `RoYStudio/`. Daraus wird die portable Windows-ZIP gebaut, die der Nutzer auf diesem PC benutzt.
- Ob der Windows-Ordner abweichende oder zusätzliche Dateien enthält, ist **NICHT VERIFIZIERT**.

Statusbegriffe: BESTÄTIGT, TEILWEISE, NICHT INTEGRIERT, GEPLANT, NICHT VORHANDEN, NICHT VERIFIZIERT.

**Was „BESTÄTIGT“ hier bedeutet.**
- Der Code ist vorhanden.
- Die Funktion ist erreichbar: in der Oberfläche oder als Befehl.
- Es gibt automatisierte Tests, die in dieser Umgebung gelaufen sind.

Die Tests liefen unter Linux und unter Wine (Windows-Programm unter Linux), mit Null-Audiogerät und virtuellem Bildschirm (Xvfb). Echte Windows-Hardware, echte Audio-Interfaces und echte Mikrofone sind damit **nicht** geprüft.

---

## 1. Projektübersicht

**Technik**
- C++20, CMake (`RoYStudio/CMakeLists.txt`), Ninja.
- Linux: GCC 13.3.
- Windows: mingw-w64 13.2 als Cross-Build, statisch gelinkt.
- Rund 28 500 Zeilen in `src/*/*.cpp` und `apps/*/*.cpp`.

**Bibliotheken** (`RoYStudio/third_party/`, Lizenzen in `THIRD_PARTY_NOTICES.md`)

| Bibliothek | Verwendung |
|---|---|
| miniaudio | Audio-I/O, Datei-Decoding |
| Dear ImGui | Oberfläche |
| nlohmann/json | Projektformat, Presets |
| CLAP-Header | Plugin-Host |
| VST3 SDK 3.8.1 | Plugin-Host |
| LAME 3.100 | MP3-Export, wird zur Laufzeit geladen (LGPL) |

**Build-Targets** (`CMakeLists.txt`)

| Target | Zweck |
|---|---|
| `roy_core` | statische Kernbibliothek |
| `roy_studio` / `RoYStudio.exe` | Oberfläche (Win32/DX11 bzw. GLFW/GL) |
| `roy_cli` | Kommandozeile |
| `roy_bench` | Benchmarks |
| `roy_soak` | Langzeittest |
| `roy_plugin_host` / `RoYPluginHost.exe` | Sandbox-Prozess für Plugins |
| `roy_tests` | Testprogramm |
| `roy_test_stem_engine` | Test-Hilfsprogramm für Stem-Trennung |
| Test-Plugins | CLAP und VST3 |

**Module unter `src/`**

| Ordner | Inhalt |
|---|---|
| `audio/` | Engine, DeviceManager, Transport, TempoMap, Metronom, OfflineRender, RenderGraph, PreviewPlayer |
| `project/` | Projektmodell, Laden/Speichern (`ProjectIO`), Session mit Autosave und Recovery, Automation |
| `commands/` | Befehlssystem mit Undo (137 registrierte Befehle laut Zählung von `r.add({"…"`) |
| `record/` | Recorder, Takes und Comping |
| `arrange/` | Clip-Operationen, Waveform-Cache, Beat-Import |
| `midi/` | MIDI-Dateien, Live-MIDI-Eingang, MIDI-Learn, Skalen |
| `beat/` | Step-Sequencer |
| `instruments/` | 808, Drums, Sampler, Synth |
| `sampler/` | Sample-Werkzeuge |
| `effects/` | Dynamics, Effekte, Spezialeffekte |
| `dsp/` | FFT, Filter, Loudness, Oversampler, Resampler, TimeStretch |
| `mixer/` | ChannelPreset |
| `master/` | Mastering |
| `export/` | WAV, FLAC, MP3 |
| `vocal/` | Pitch-Detektor, Pitch-Analyse, Pitch Guardian, Vocal Doctor, Vocal Tools, Flow Analyzer |
| `intelligence/` | Mix-Analyse, Assistant, Arrangement, ProducerMemory, WhatIf |
| `stems/` | Stem-Trennung |
| `plugins/` | Scanner, Sandbox, Shared Memory, Kompatibilitätsprüfung |
| `support/` | Einstellungen, Diagnose, Systemcheck, Testsignale |
| `core/` | Log, Crash-Handler, Dateien, Zip, SPSC-Queue |

**Oberfläche** (`apps/roy_studio/`)

| Datei | Bereich |
|---|---|
| `UiMain.cpp` | Menü, Transport, Statusleiste, Projekt-Dialoge, Befehlspalette |
| `UiPlaylist.cpp` | Playlist |
| `UiMixer.cpp` | Mixer und Master |
| `UiMusic.cpp` | Beat Lab, 808, Piano-Roll |
| `UiTools.cpp` | Vocals, Browser, Projekt |
| `UiSession.cpp` | Ersteinrichtung, Export |

---

## 2. Implementierte Funktionen (mit Belegen)

### B. Aufnahme und Wiedergabe

| Funktion | Status | Beleg |
|---|---|---|
| Audioausgabe und -eingang | BESTÄTIGT (Code und Tests) | `src/audio/DeviceManager.cpp` (miniaudio); Tests `test_gmb01_audio.cpp`, `test_gmb04_record.cpp`; Mikrofon auf echter Hardware nur durch Nutzerberichte, nicht durch Tests |
| Unterstützte Backends | BESTÄTIGT (Code) | `DeviceManager.cpp:39-41`: WASAPI, DirectSound, WinMM, CoreAudio, ALSA, PulseAudio, JACK, Null; **kein ASIO** (bewusst: keine kostenpflichtigen oder dubiosen Treiber) |
| Kopfhörer-Monitoring (LIVE VOCAL mit Insert-Kette) | BESTÄTIGT | Befehl `SetupLiveVocal`, LIVE-Button in `UiPlaylist.cpp`/`UiMixer.cpp`, `tests/test_live_vocal.cpp` |
| Sample-Rate und Puffer einstellbar | BESTÄTIGT (Code und UI) | `UiSession.cpp:139-145`, `support/AppSettings.cpp` |
| Mehrspuraufnahme, Takes, Comping | BESTÄTIGT | `record/Recorder.cpp`, `record/Takes.cpp`, `tests/test_takes.cpp`, Take-Lanes in der Playlist |
| Loop-Recording (ein Take pro Durchlauf) | BESTÄTIGT | Test `test_gmb04_record.cpp:114` |
| Punch-in/out | **NICHT INTEGRIERT** | `Recorder::setPunch` und Test `test_gmb04_record.cpp:138` vorhanden; in `apps/` kein Aufruf, kein Befehl |
| Wellenform | BESTÄTIGT | `arrange/WaveformCache.cpp`, `UiPlaylist.cpp:69` (`drawWaveform`) |
| Schneiden, Trimmen, Fades, Clip-Lautstärke, Verschieben | BESTÄTIGT | `arrange/ClipOps.cpp`, `UiPlaylist.cpp` (Ctrl+E-Split, Kanten-Trim, Clip-Menü), `test_gmb03_arrange.cpp` |
| Speichern, Autosave, Absturz-Recovery, Backups | BESTÄTIGT | `project/Session.cpp`, `ProjectIO.cpp`, `test_gmb02_project.cpp`, `test_faults.cpp` |
| Ein Ordner pro Song (`Audio`, `Exports`, `BACKUPS`, `RECOVERY`, `Stems`) | BESTÄTIGT | `ProjectIO.cpp:605` (`createProjectFolder`) |
| Song-Liste und Datei-Dialog beim Öffnen | BESTÄTIGT (Code); auf Windows NICHT VERIFIZIERT | `UiMain.cpp` |
| Export WAV, FLAC, MP3 im Hintergrund, mit Fortschrittsanzeige | BESTÄTIGT | `export/`, `test_gmb11_master.cpp`, `test_mp3.cpp`; Export enthält Takes: `test_beat_import.cpp` |
| Beat-Import (MP3/WAV, BPM und Tonart aus dem Dateinamen) | BESTÄTIGT | `arrange/BeatImport.cpp`, `test_beat_import.cpp` |

### C. Auto-Tune und Pitch Correction

Ein Modul namens **„RoYAutoTune“ existiert im untersuchten Code NICHT**. `grep "RoYAutoTune"` über `src`, `apps` und `tests` findet nichts. Die vorhandene Echtzeit-Tonhöhenkorrektur heißt **`RoyVocalTune`**:
- Code: `src/effects/Special.cpp:227-330`.
- Registriert als `roy.vocaltune` in `src/dsp/Builtins.cpp:39`.

Dazu kommt die Offline-Korrektur **Pitch Guardian** in `src/vocal/PitchGuardian.*`.

| Funktion | Status | Beleg / Einschränkung |
|---|---|---|
| Grundfrequenz-Erkennung (Echtzeit) | BESTÄTIGT | YIN-ähnliche Differenzfunktion in `RoyVocalTune::analyse()`: Analyse alle 128 dezimierte Samples, Bereich 70–1000 Hz |
| Grundfrequenz-Erkennung (offline) | BESTÄTIGT | `vocal/PitchDetector.cpp`, `PitchAnalysis.cpp` (Einteilung in stabil, Vibrato, Slide, unsicher), `test_gmb06_pitch.cpp` |
| Tonhöhenkorrektur in Echtzeit | BESTÄTIGT | Zwei-Tap-Pitch-Shifter mit Verzögerungsleitung (`PitchShifterRt`, `Special.h:70`), Fenster 30 ms |
| Tonart und Skala | BESTÄTIGT | Parameter `keyRoot` und `keyScale`; LIVE übernimmt die Song-Tonart; Off-Key-Filter, Allow Chromatic, Threshold in Cent |
| Retune Speed und Strength | BESTÄTIGT | Parameter `speed` (0–400 ms) und `strength` |
| Formant-Erhaltung (Echtzeit) | **NICHT VORHANDEN** | Der Shifter verschiebt die Formanten mit der Tonhöhe; `dsp/TimeStretch.h:4` nennt diese Grenze ausdrücklich |
| Formant-Erhaltung (offline, Pitch Guardian) | BESTÄTIGT (Code); klangliche Qualität NICHT VERIFIZIERT | PSOLA mit `formantPreserve`, `PitchGuardian.cpp:220-378` |
| Humanize und Vibrato erhalten (offline) | BESTÄTIGT (Code und Test) | `PitchGuardian.h:30-32` (`humanize`, `vibratoPreserve`) |
| Humanize und Vibrato (Echtzeit-Tune) | NICHT VORHANDEN | Keine solchen Parameter in `RoyVocalTune` |
| Dry/Wet (Echtzeit-Tune) | NICHT VORHANDEN | Es gibt nur `strength` (Stärke der Korrektur), keinen Mix |
| Bypass | BESTÄTIGT | Bypass pro Insert-Slot (`PluginSlot::bypass`) |
| Manuelle Tonhöhenbearbeitung | TEILWEISE | Pitch-Editor-Daten (`PitchEditorData`) mit Kurve, Zielen, Cent und Status sowie A/B-Vergleich (`VocalAB`), Test `test_vocal_editor.cpp`; Ziehen einzelner Noten im UI nicht gefunden; `RegionPitch` nur über die Befehlspalette mit JSON-Argumenten |
| Algorithmische Latenz | NICHT VERIFIZIERT | `RoyVocalTune` meldet keine `latencySamples()`; keine Messung vorhanden |
| Anbindung an Aufnahme, Monitoring und Mixer | BESTÄTIGT | Insert im Kanal; LIVE nutzt dieselbe Kette; die Aufnahme bleibt trocken (Meldung im Code, `test_live_vocal.cpp`) |

### D. Mixer und Effekte

Alle Effekte sind über das Insert-Menü erreichbar. `UiMixer.cpp:256` listet sämtliche Einträge von `ProcessorFactory`.

Registrierte Effekte (`src/dsp/Builtins.cpp`):
- EQ, Compressor, De-Esser, Gate, Limiter, Clipper, Transient
- Saturation, Distortion
- Reverb, Delay, Chorus, Flanger, Phaser
- Pitch, VocalTune, NoiseCleaner, DynamicSpace, Stereo, Analyzer, External
- Instrumente: 808, Drums, Sampler, Synth

| Funktion | Status | Beleg |
|---|---|---|
| Gain, Fader, Pan, Pegelanzeige | BESTÄTIGT | `UiMixer.cpp`, `test_mixer.cpp` |
| EQ, Kompressor, De-Esser, Gate, Saturation, Distortion, Reverb, Delay, Chorus/Doubler | BESTÄTIGT | `effects/*.cpp`, `test_gmb09_effects.cpp`; Doubler über `roy.pitch` mit Cent-Verstimmung oder Chorus |
| Expander | NICHT VORHANDEN als eigener Effekt | Gate mit `range` (Absenkung statt Stille) |
| Sends, Busse, Master-Routing | BESTÄTIGT | `test_mixer.cpp`, Sends im Mixer-UI |
| Sidechain | **NICHT INTEGRIERT (UI)** | Befehl-Argument `sidechain` in `Commands.cpp:467`; im UI kein Zugang gefunden |
| Effektketten, Presets, Bypass | BESTÄTIGT | `mixer/ChannelPreset.cpp`, PRESETS-Button, 17 mitgelieferte `.roychain` unter `packaging/portable/Presets/Channel/` |
| Mastering-Ketten | BESTÄTIGT | `master/Mastering.cpp`: streaming, dynamic, club, voice, ultimate; Master-Assistent `assist()`, `test_gmb11_master.cpp` |
| Clipping-Erkennung und Ausgangsschutz | BESTÄTIGT | Look-ahead-Limiter am Ausgang (`AudioEngine.cpp`, nur live), Zähler `outputOvers`, Hinweis TOO LOUD; Test „output protection“ in `test_live_vocal.cpp` |

Die klangliche Qualität der Presets ist **NICHT VERIFIZIERT**. Die Tests prüfen nur: Preset lädt, Signal ist hörbar, kein Clipping.

### E. MIDI, Sampler, Playlist

| Funktion | Status | Beleg |
|---|---|---|
| MIDI-Clips, Piano-Roll, MIDI-Dateien | BESTÄTIGT | `midi/`, `test_gmb05_midi.cpp` |
| Live-MIDI-Eingang (Keyboard) | BESTÄTIGT | `test_live_midi.cpp` |
| MIDI-Learn | BESTÄTIGT | `test_midi_learn.cpp` |
| Step-Sequencer, Patterns, 808 | BESTÄTIGT | `test_beatlab.cpp`, `test_gmb07_beat.cpp`, `test_808.cpp` |
| Sampler, Audio-Import, Browser-Vorhören | BESTÄTIGT | `test_gmb08_sampler.cpp`, `test_browser.cpp` |
| Tempo-Map, Tempo-Sync von Loops | BESTÄTIGT | `audio/TempoMap.cpp`, `test_browser.cpp` (Tempo-Sync) |
| Automation | BESTÄTIGT | `project/Automation.cpp`, `test_automation.cpp` |
| Undo für alle Befehle | BESTÄTIGT | `commands/UndoManager.cpp`, `test_gmb06_commands.cpp` |

### Plugins

CLAP- und VST3-Host laufen im Sandbox-Prozess, mit Absturz- und Hänger-Isolation. Belegt durch `test_plugins.cpp` und `test_vst3.cpp`.

Plugins von **Drittherstellern sind NICHT VERIFIZIERT**. Laut `00_IMPORTANT/PLUGIN_REPORTS/` wurden nur die offiziellen Steinberg-SDK-Beispiele und eigene Test-Plugins geprüft.

---

## 3. Teilweise implementierte Funktionen

| Funktion | Was fehlt |
|---|---|
| Manuelle Noten-Tonhöhenbearbeitung | Kein grafisches Ziehen einzelner Noten |
| Echtzeit-Autotune | Formant-Erhaltung, Dry/Wet, Humanize/Vibrato-Regler und Latenzmeldung fehlen |
| Stem-Trennung | Eingebaut nur `roy.dsp-basic` mit starkem Übersprechen (laut `stems/StemSeparator.h`). `roy.external` braucht eine vom Nutzer installierte Engine. |
| Windows-Dateidialoge | Laufen auf eigenem STA-Thread; Funktion auf echtem Windows nur durch Nutzerberichte belegt |

## 4. Vorhanden, aber nicht integriert

| Modul | Integrationsbedarf |
|---|---|
| Punch-in/out (`Recorder::setPunch`) | Befehl, UI-Marker, Speicherung im Projekt |
| Sidechain-Routing | UI im Insert-Menü oder Kanal |
| `RegionGain`, `RegionPitch`, `VocalMicroscope` | Nur über die Befehlspalette mit JSON (`UiMain.cpp`, `drawPalette`); kein eigenes UI |
| NoiseCleaner (`roy.noisecleaner`) | Als Insert erreichbar und von Vocal Doctor vorgeschlagen; eigener Test NICHT VERIFIZIERT |

## F. KI-Funktionen

**Es ist keine KI- oder ML-Verarbeitung vorhanden.** Die Suche nach `onnx`, `tensorflow`, `torch` und `neural` in `src` und `apps` ergab nur die Fundstelle „editor“ in `plugins/Compat`.

| Funktion | Status | Tatsächliche Umsetzung |
|---|---|---|
| Vocal Enhancement / Rauschunterdrückung | NICHT VORHANDEN als KI | DSP: NoiseCleaner (spektral), Gate, `BreathReduce` |
| Automatische Vocal-Analyse | BESTÄTIGT als **regelbasierte DSP-Analyse**, nicht KI | Vocal Doctor, Flow Analyzer, Pitch-Analyse |
| Effektvorschläge / Auto-Mix | BESTÄTIGT als **regelbasierte Mix-Analyse**, nicht KI | `intelligence/MixIntelligence.h`: Messungen und Vorschläge als Befehle mit Undo |
| Preset-Wahl nach Stil oder Stimme | NICHT VORHANDEN | Presets werden manuell gewählt |
| ML-Stem-Trennung | NICHT VORHANDEN | Nur Adapter für eine externe, vom Nutzer installierte Engine |

## G. Audio-Engine, Performance, Stabilität

| Punkt | Status | Beleg |
|---|---|---|
| Keine Speicherallokation im Audio-Callback | BESTÄTIGT (Test) | `test_gmb01_audio.cpp:330`, `test_gmb04_record.cpp:268`, weitere Tests mit `AllocationCounter` |
| Lock-freie Übergabe des Render-Graphs | BESTÄTIGT (Code) | `AudioEngine::setGraph` (atomarer Austausch plus Garbage-Liste), `core/SpscQueue.h` |
| Geräteverlust | BESTÄTIGT (Code und Fehlerinjektion) | `DeviceManager.cpp:263-270` (`markLost`), `test_faults.cpp` |
| CPU / 100 Spuren | Gemessen am 2026-09-28 | `00_IMPORTANT/BENCHMARKS/BENCHMARK_2026-09-28*.md`, nur in dieser Umgebung, nicht auf dem Nutzer-PC |
| Langzeitstabilität | Gemessen am 2026-09-29 | `00_IMPORTANT/BENCHMARKS/SOAK_2026-09-29.md`: 2 simulierte Stunden, Inhalt MOCK |
| Echte Latenz und Dropouts auf dem Nutzer-PC | NICHT VERIFIZIERT | Nutzer meldet „xruns 0, WASAPI shared 48000 Hz 562 samples“ bei gelegentlichem Ticken; Ursache ungeklärt |

---

## 5. Build- und Teststatus (nur Nachgewiesenes)

| Was | Ergebnis | Wann |
|---|---|---|
| Letzter Build Linux und Windows | Erfolgreich, 0 Fehler. Warnungen wurden nicht protokolliert. | `build/roy_tests` 2026-10-23 16:13, `build-win/RoYStudio.exe` 2026-10-23 16:17, Commit `c2a64d0` |
| Portable ZIP | `dist/RoYStudio-0.2.0-PORTABLE-win64.zip`, 2026-10-23 16:17, aus `build-win`, gebaut mit `packaging/make_portable_windows.sh` | — |
| Linux-Testsuite (separat ausgeführt) | **229 Tests, 0 fehlgeschlagen, 102 138 Checks**, Exitcode 0 | 2026-10-28 |
| Windows-Testsuite unter Wine | 225 Tests, 2 fehlgeschlagen (`vst3/editor`, `vst3/clap editor`: „CreateWindowEx failed“, weil kein Bildschirm gesetzt war). Wiederholung von `vst3` mit Xvfb: **10/10 bestanden**. | 2026-10-28 |
| Windows-GUI-Selbsttest unter Wine (`RoYStudio.exe --audio null --selftest`) | **SELFTEST PASSED, 28 Schritte, 0 fehlgeschlagen** | 2026-10-28 |
| GUI-Selbsttest unter Wine ohne `--audio null` | 8 Fehler, alle Audiogerät („device init failed“), weil die Umgebung kein Gerät hat | 2026-10-13 |
| ASan, TSan, Soak | Laut `00_IMPORTANT/ROY_STUDIO_ULTIMATE_STATUS.md` bestanden. In dieser Bestandsaufnahme **nicht erneut ausgeführt**. | 2026-09-29 |
| Tests auf echter Windows-Hardware | **NICHT AUSGEFÜHRT** (automatisiert). Plan liegt in `00_IMPORTANT/TEST_REPORTS/WINDOWS_NATIVE_TEST_PLAN.md`. Es gibt nur manuelle Nutzerrückmeldungen. | — |

**Fehlende Tests**
- Klangqualität von Autotune und Presets (Hörtest).
- Latenz von VocalTune.
- Punch über das UI.
- Plugins von Drittherstellern.
- Dateidialoge auf echtem Windows.

## 6. Bekannte Fehler und Risiken

1. Gelegentliches Ticken auf dem Nutzer-PC, Ursache ungeklärt. Die Statusleiste zeigte 0 xruns. Vermutung: Autotune-Artefakte bei falscher Tonart. **NICHT VERIFIZIERT.**
2. Der Echtzeit-Pitch-Shifter mit zwei Taps erzeugt bei großen Verschiebungen hörbare Artefakte (Fensterwechsel). Keine Formant-Erhaltung: Die Stimme klingt bei Korrektur nach Mickymaus oder dunkler.
3. Kein ASIO. Unter WASAPI shared ist die Latenz höher (Puffer 562 Samples ≈ 11,7 ms bei 48 kHz, plus Systempuffer).
4. Der Ausgangs-Limiter wirkt nur live, nicht im Export. Das ist bewusst so; im Export übernimmt das die Master-Kette.
5. Die Lizenz ist offen: `RoYStudio/LICENSE_DECISION_PENDING.md` („LICENSE DECISION PENDING“).
6. Das Repository heißt noch wie die alte Bot-Vorlage. Im Wurzelverzeichnis liegen Bot-Dateien (`app.ts`, `render.yaml`, `node_modules`), die nicht zu RoY Studio gehören.
7. Die Bezeichnung „KI“ wäre für die Analyse-Module irreführend. Sie sind regelbasiert.

## 7. Backups und Sicherungsstatus

| Sicherung | Status |
|---|---|
| Git-Historie | 74 Commits auf `claude/bold-franklin-fybkj5`, gepusht nach origin |
| `00_IMPORTANT/SNAPSHOTS/` | 0000 enthält ein echtes Archiv (`repo_c9c2b4a.tar.gz` mit `MANIFEST.sha256`). 0001–0005 (2026-09-28/29) enthalten nur `SNAPSHOT_INFO.txt` mit Commit-Verweisen. |
| `00_IMPORTANT/BACKUPS/` | Leer (nur `.gitkeep`) |
| Projekt-Backups | Pro Song automatisch in `BACKUPS/` und `RECOVERY/` (Code in `ProjectIO.cpp`, `Session.cpp`) |
| Build-Artefakte (`dist/`) | Liegen in `.gitignore`, nicht im Git; nur lokal in der Cloud-Umgebung, die vergänglich ist |

## 8. Was für ein professionelles Vocal-Studio fehlt

- Hochwertige Echtzeit-Tonhöhenkorrektur mit Formant-Erhaltung (PSOLA/WSOLA oder Phase-Vocoder mit Formant-Hüllkurve), Dry/Wet, Humanize und gemeldeter Latenz.
- Grafischer Noten-Editor für Vocals: Noten ziehen, trennen, Übergänge und Vibrato pro Note.
- Punch-in/out im UI und Pre-Roll.
- Sidechain im UI.
- Latenz-Messung und Kompensation der Aufnahme (Round-Trip-Messung).
- Echte KI- oder ML-Funktionen: Rauschunterdrückung, Stem-Trennung, Preset-Empfehlung. Nur mit freien Lizenzen und lokal.
- Validierung auf echter Hardware nach `WINDOWS_NATIVE_TEST_PLAN.md`.

## 9. Empfohlene Reihenfolge

| Priorität | Aufgabe |
|---|---|
| P1 | Ticken klären: Diagnose auf dem Nutzer-PC, Pitch-Shifter-Artefakte messen |
| P1 | VocalTune 2: Formant-Erhaltung, Dry/Wet, Latenzmeldung |
| P2 | Grafischer Vocal-Noten-Editor auf Basis von `PitchEditorData` und `RegionPitch` |
| P2 | Punch-in/out, Pre-Roll, Latenzkompensation |
| P3 | Sidechain-UI und UI für die Region-Befehle |
| P3 | Optionale lokale ML-Module (frei lizenziert, außerhalb des Prozesses, wie beim Stem-Adapter) |
| Laufend | Lizenzentscheidung, Bereinigung der Bot-Dateien im Repository |

## 10. Vorschlag für die nächsten GMBs

| GMB | Inhalt |
|---|---|
| GMB 13 – VocalTune 2 | Echtzeit-PSOLA mit Formant-Erhaltung, Dry/Wet, Humanize, Latenz an die Engine melden; Tests für Artefakt-Pegel, Latenz und Tonhöhen-Treffer. **Empfohlen als nächster Schritt.** |
| GMB 14 – Vocal Note Editor | Noten ziehen, trennen und verbinden, Übergänge, alles mit Undo |
| GMB 15 – Recording Pro | Punch-UI, Pre-Roll, Round-Trip-Latenzmessung und -Kompensation |
| GMB 16 – Routing UI | Sidechain, Region-Werkzeuge |
| GMB 17 – Hardware-Validierung | Testplan auf dem Nutzer-PC, Diagnosepaket auswerten |

---

## Zusammenfassung

**Nachweislich programmiert** ist eine vollständige DAW-Basis in C++20:
- Engine, Projekte mit Recovery, Aufnahme mit Takes, Comping und Loop-Recording.
- Playlist mit Schnitt, MIDI, Beat Lab, 808, Sampler.
- Mixer mit 20 eigenen Effekten, Presets und Mastering.
- Export als WAV, FLAC und MP3.
- CLAP- und VST3-Host in einer Sandbox.
- Echtzeit-Tonhöhenkorrektur `RoyVocalTune` und Offline-Korrektur Pitch Guardian.

**Nachweislich funktionieren**, in automatisierten Tests auf Linux und Wine mit Null-Audiogerät, alle oben als BESTÄTIGT markierten Punkte:
- 229/229 Tests unter Linux.
- 28/28 Schritte im GUI-Selbsttest unter Wine.

**Gebaut und getestet** wurde Commit `c2a64d0` am 2026-10-23 (Build) und am 2026-10-28 (Tests, separat ausgeführt). Echte Windows-Hardware wurde nicht automatisiert getestet.

**Nur teilweise fertig oder nicht integriert:**
- Manueller Noten-Editor.
- Echtzeit-Autotune ohne Formant-Erhaltung, Dry/Wet und Latenzmeldung.
- Punch-in/out, Sidechain-UI, Region-Werkzeuge.
- Stem-Trennung nur in Basisqualität.

**Für das professionelle Vocal-Studio fehlen:**
- Autotune in Profi-Qualität.
- Grafischer Vocal-Editor.
- Latenzkompensation.
- Echte KI-Funktionen. Derzeit gibt es keine; die Analyse-Module sind regelbasierte DSP.
- Validierung auf echter Hardware.

Ein Modul namens **„RoYAutoTune“ existiert im untersuchten Stand nicht.**

**Nächster sinnvoller Schritt:** GMB 13 – VocalTune 2 (Formant-Erhaltung, Dry/Wet, Latenz), zusammen mit der Klärung des Tickens auf dem Nutzer-PC.
