@echo off
rem Creates UserData\Diagnostics\RoYStudio_Diagnostics_<time>.zip (anonymised; nothing is uploaded)
cd /d "%~dp0"
"%~dp0..\roy_cli.exe" diagnostic-package
if exist "%~dp0..\UserData\Diagnostics" start "" explorer "%~dp0..\UserData\Diagnostics"
pause
