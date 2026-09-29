RoY Studio - BETA test kit
==========================

This folder contains the RoY Studio programs and a self-contained automated test kit for
validating the build on a real machine (the development environment only has Linux + Wine
and no audio hardware). Nothing here installs anything or changes system settings.

Programs
  roy_studio(.exe)       the DAW (Windows: Direct3D 11 GUI)
  roy_cli(.exe)          command line: selftest, export, check, recovery, plugins, devices
  roy_plugin_host(.exe)  sandbox process for CLAP/VST3 plugins - keep it next to roy_studio
  roy_mp3lame(.dll/.so)  LAME 3.100 MP3 encoder (LGPL-2.0, separate library, replaceable)
  roy_bench(.exe)        performance benchmark (--baseline <old benchmark.json> flags regressions)
  roy_soak(.exe)         accelerated long-session test (memory/handle growth), writes soak.md:
                         roy_soak.exe --cycles 240 --plugin test_plugins\roy_test_gain.clap --out C:\RoYSoak
                         (synthetic MOCK session content; needs no audio device)

Test kit
  roy_tests(.exe)        full automated suite; finds test_plugins\ next to itself
  test_plugins\          RoY TEST plugins (CLAP + VST3, incl. deliberate crash/hang plugins)
                         and a MOCK stem-separation engine - test-only, not real products

Quick check (about 10 minutes)
  1. roy_tests.exe                                   -> "... tests, 0 failed"
  2. roy_cli.exe selftest C:\RoYTest\cli             -> "CLI SELFTEST PASSED"
  3. roy_studio.exe --selftest C:\RoYTest\gui        -> C:\RoYTest\gui\selftest_report.txt says PASSED
                                                        (uses your default audio device)
  Then follow 00_IMPORTANT/TEST_REPORTS/WINDOWS_NATIVE_TEST_PLAN.md from the repository.

Notes
  * The crash/hang test plugins crash on purpose; Windows may show them in its reliability log.
    RoY itself must keep running - that is what is being tested.
  * Logs:            %APPDATA%\RoYStudio\roy_studio.log
  * Crash reports:   %APPDATA%\RoYStudio\CrashReports\
  * Licences of all third-party components: licenses\ and licenses\THIRD_PARTY_NOTICES.md
