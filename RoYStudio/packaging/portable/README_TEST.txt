RoY Studio 0.2.0 BETA – PORTABLE TEST BUILD für Windows 10/11 (64-Bit)
=========================================================================

UNSIGNED DEVELOPMENT/TEST BUILD – nicht signiert, kein fertiges Produkt.
Lizenz von RoY Studio: LICENSE DECISION PENDING (siehe LICENSE_DECISION_PENDING.txt).
Kostenlos: keine Installation, keine Treiber, keine Plugins oder Tools zum Kaufen nötig.


1. ENTPACKEN
------------
- Optional, aber empfohlen: Rechtsklick auf die ZIP-Datei > Eigenschaften > unten "Zulassen"
  anhaken > OK. (Das gilt nur für diese eine Datei; Windows-Schutz bleibt überall sonst aktiv.)
- ZIP entpacken in einen einfachen Ordner, z. B.  C:\RoYStudioTest
  (NICHT nach "C:\Program Files" – dort darf ein portables Programm nicht schreiben).


2. STARTEN
----------
Doppelklick auf:   START_ROY_STUDIO.bat      (oder direkt RoYStudio.exe)

Windows SmartScreen kann warnen ("Der Computer wurde durch Windows geschützt"), weil der
Testbuild nicht signiert ist. So erkennst du, dass es unser Testbuild ist:
  - Klick auf "Weitere Informationen": App = RoYStudio.exe, Herausgeber = "Unbekannter Herausgeber"
  - Prüfsumme vergleichen (optional): PowerShell im Ordner öffnen und eingeben
        Get-FileHash .\RoYStudio.exe
    Der Wert muss mit der Zeile für RoYStudio.exe in SHA256SUMS.txt übereinstimmen.
  - Dann "Trotzdem ausführen". SmartScreen bitte NICHT abschalten – das ist nicht nötig.

Mikrofon: Falls der Eingang stumm bleibt: Windows-Einstellungen > Datenschutz und Sicherheit >
Mikrofon > "Desktop-Apps den Zugriff auf Ihr Mikrofon erlauben" = Ein.


3. ERSTER START: WELCOME TO ROY STUDIO (ca. 2 Minuten)
-----------------------------------------------------
STEP 1 OUTPUT DEVICE  – Lautsprecher / Headset / Audio-Interface wählen, PLAY TEST TONE drücken
                         (1 Sekunde Piepton). Standard: WASAPI Shared (kein Extra-Treiber).
                         Optional: "WASAPI Exclusive" für weniger Latenz.
STEP 2 INPUT DEVICE   – Mikrofon / Interface wählen, sprechen: LIVE INPUT LEVEL bewegt sich.
                         TEST MICROPHONE nimmt 3 Sekunden auf und spielt sie sofort ab.
STEP 3 BUFFER         – Start mit 256. Bei Aussetzern erscheint eine Warnung -> 512 wählen.
STEP 4 SAMPLE RATE    – 48000 Hz (Standard). Nur unterstützte Werte werden angeboten.
STEP 5 MIDI           – Keyboard wird angezeigt, sonst einfach SKIP.
SYSTEM CHECK          – PASS / WARNING / FAIL für Windows, x64, CPU, RAM, Audio, MIDI,
                         Plugin-Host, MP3, Schreibzugriff, Speicherplatz.
Dann "FINISH + START FIRST REAL MUSIC SESSION".
Alles bleibt später im Menü Audio änderbar (Audio > Setup check).


4. FIRST REAL MUSIC SESSION (Menü Help > FIRST REAL MUSIC SESSION)
-------------------------------------------------------------------
Ein Fenster rechts führt dich durch 43 Schritte:
  A BEAT        neues Projekt, 140 BPM, Drum Pattern, Kick, Snare, Hi-Hat, 808, 808-Noten,
                abspielen, Arrangement in der Playlist, Mixer, Beat anhören
  B VOCAL       Vocal Track, Input wählen, ARM, Eingangspegel, Count-In, Aufnahme 10–20 s,
                Stopp, sofort abspielen   (Kopfhörer benutzen, sonst Rückkopplung!)
  C VOCAL EDIT  Pitch Analysis, Pitch Editor, OFF-KEY Anzeige, Pitch Guardian Preview,
                A/B Original / Corrected  (die Originalaufnahme wird NIE überschrieben)
  D MIX         Pegel, EQ, Kompressor, De-Esser, Reverb (alles im MIXER sichtbar und änderbar)
  E EXPORT      Ordner wählen (Browse...), WAV, FLAC, MP3, OPEN EXPORT FOLDER
  F PLUGINS     RoY TEST VST3 + CLAP: laden, Editor öffnen, Parameter ändern, Automation,
                speichern, schließen, öffnen, State wiederhergestellt?
Jeder Schritt: "DO IT" macht ihn automatisch (normale Befehle, Strg+Z geht),
oder du machst ihn selbst und klickst "done by hand". Ergebnisse werden gespeichert.


5. WO LIEGT WAS (portabel – alles in diesem Ordner)
---------------------------------------------------
RoYStudio.exe             das Programm
RoYPluginHost.exe         Plugin-Sandbox (jedes Plugin läuft getrennt – ein Absturz trifft RoY nicht)
roy_cli.exe               Kommandozeilen-Werkzeug (Systemcheck, Diagnose, Export)
roy_mp3lame.dll           MP3-Encoder (LAME 3.100, LGPL, austauschbar)
Plugins\                  RoY TEST VST3 + CLAP (werden automatisch gefunden)
Samples\Drums\            selbst erzeugte Drum-Samples (Kick, Snare, Clap, Hats, 808)
Projects\                 DEINE PROJEKTE (Standard-Speicherort)
UserData\                 Einstellungen, Logs, Crash-Reports, Diagnosepakete (entsteht beim Start)
TestKit\                  Testprojekt, Test-WAVs, synthetische Stimme, MIDI, TEST_RESULTS.md,
                          automatische Tests
licenses\                 Lizenzen aller Fremdkomponenten
Nichts wird in %APPDATA% oder die Registry geschrieben. Löschen = Ordner löschen
(vorher Projects\ sichern, wenn du deine Projekte behalten willst).


6. TESTKIT (Ordner TestKit\)
----------------------------
Projects\RoY Test Beat\   fertiges Test-Projekt (File > Open ... RoY Test Beat.roy)
Audio\                    Testtöne 440 Hz / 1 kHz, synthetische Stimme mit schiefen Tönen
MIDI\                     808-Linie und Akkorde als .mid (in PIANO ROLL importierbar)
TEST_RESULTS.md           Checkliste zum Ausfüllen
RUN_SYSTEM_CHECK.bat      Systemcheck als Text (system_check.txt)
RUN_SESSION_TEST.bat      automatischer Durchlauf aller Musik-Schritte mit deinem Audiogerät
RUN_AUTOMATED_TESTS.bat   ca. 200 automatische Tests (5–10 Minuten; einige Test-Plugins
                          stürzen ABSICHTLICH ab – RoY muss weiterlaufen, das wird getestet)
CREATE_DIAGNOSTIC_PACKAGE.bat   Diagnose-ZIP erstellen
Alle Testdaten sind selbst erzeugt – keine fremden Samples, keine Urheberrechtsprobleme.


7. CRASH RECOVERY TESTEN
------------------------
Projekt ändern, mindestens 60 Sekunden warten (Autosave), RoYStudio.exe im Task-Manager
beenden, RoY neu starten, Projekt öffnen -> "RECOVER PROJECT" muss angeboten werden.


8. WENN ETWAS NICHT FUNKTIONIERT
--------------------------------
Help > CREATE DIAGNOSTIC PACKAGE  (oder TestKit\CREATE_DIAGNOSTIC_PACKAGE.bat)
-> UserData\Diagnostics\RoYStudio_Diagnostics_<Zeit>.zip
Enthält: Logs, Build-Version, Audio-Geräte, Plugin-Scan, Crash-Reports (Text), Systemcheck,
Ergebnisse der Music Session. NICHT enthalten: Benutzername (ersetzt), Projekte, Audio,
Passwörter, Tokens, persönliche Dateien. Es wird NICHTS automatisch hochgeladen.


9. BITTE ZURÜCKSCHICKEN
-----------------------
1. TestKit\TEST_RESULTS.md (ausgefüllt)
2. UserData\Diagnostics\RoYStudio_Diagnostics_<Zeit>.zip
3. optional: TestKit\automated_tests_result.txt und TestKit\SessionTest\session_test_report.md


BEKANNTE EINSCHRÄNKUNGEN
------------------------
- Nicht signiert (SmartScreen-Hinweis, siehe oben).
- Kein ASIO in diesem Build (WASAPI Shared/Exclusive funktioniert ohne Extra-Treiber).
- Benötigt Windows 10 (1809+) oder 11, 64-Bit, DirectX-11-fähige Grafik.
- Eigene Plugins: später mit  roy_cli.exe plugin-compat <Plugin-Ordner> --editor --out report.md
