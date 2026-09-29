@echo off
rem Automated FIRST REAL MUSIC SESSION with your real audio device (about 1 minute).
rem Plugin editor windows open and close by themselves. Results: TestKit\SessionTest\session_test_report.md
cd /d "%~dp0"
echo RoY Studio - automated music session test (about 1 minute, you will hear the test beat)...
"%~dp0..\RoYStudio.exe" --session-test "%~dp0SessionTest"
if errorlevel 1 (echo RESULT: some steps FAILED - see the report) else (echo RESULT: SESSION TEST PASSED)
start "" notepad "%~dp0SessionTest\session_test_report.md"
pause
