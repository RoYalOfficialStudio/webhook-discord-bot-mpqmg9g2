@echo off
rem RoY Studio portable test build - starts RoY from this folder (no installation, no admin rights)
cd /d "%~dp0"
if not exist "%~dp0RoYStudio.exe" (
  echo RoYStudio.exe not found. Please unzip the whole ZIP file first, then start this file again.
  pause
  exit /b 1
)
start "" "%~dp0RoYStudio.exe"
