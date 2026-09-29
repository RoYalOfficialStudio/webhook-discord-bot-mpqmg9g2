@echo off
rem PASS / WARNING / FAIL check of this PC for RoY Studio (audio, MIDI, plugin host, MP3, disk)
cd /d "%~dp0"
echo RoY Studio - system check...
"%~dp0..\roy_cli.exe" system-check > "%~dp0system_check.txt" 2>&1
type "%~dp0system_check.txt"
echo.
echo Saved to TestKit\system_check.txt
pause
