@echo off
rem Builds the RoY Studio Windows test packages with free tools inside WSL (Windows Subsystem for Linux).
rem Only needed if you do NOT have the ready-made RoYStudio-0.2.0-PORTABLE-win64.zip.
rem Requirements (free): WSL with Ubuntu 24.04  ->  in PowerShell (admin):  wsl --install -d Ubuntu-24.04
rem Then put the RoY source folder somewhere on your PC and double-click this file.
cd /d "%~dp0.."
where wsl >nul 2>nul || (echo WSL is not installed. PowerShell as admin: wsl --install -d Ubuntu-24.04 & pause & exit /b 1)
wsl bash -lc "cd \"$(wslpath '%CD%')\" && packaging/build_windows_test_package.sh"
if errorlevel 1 (echo BUILD FAILED - see the messages above & pause & exit /b 1)
echo.
echo Done: dist\RoYStudio-*-PORTABLE-win64.zip and build-win\RoYStudio-*-win64.exe
explorer dist
pause
