@echo off
rem About 200 automated tests (5-10 minutes). Some RoY TEST plugins crash or hang ON PURPOSE -
rem RoY must keep running, that is what is tested. Everything stays inside the RoY folder.
setlocal
cd /d "%~dp0AutomatedTests"
set ROY_TESTKIT_LOCAL=1
set "ROY_USER_DIR=%~dp0..\UserData\AutomatedTests"
set "TEMP=%~dp0..\UserData\Temp"
set "TMP=%~dp0..\UserData\Temp"
if not exist "%TEMP%" mkdir "%TEMP%"
echo RoY Studio - automated tests, please wait (5-10 minutes)...
roy_tests.exe > "%~dp0automated_tests_result.txt" 2>&1
findstr /C:"tests," "%~dp0automated_tests_result.txt"
findstr /B /C:"[FAIL]" "%~dp0automated_tests_result.txt"
echo.
echo Full output: TestKit\automated_tests_result.txt
pause
