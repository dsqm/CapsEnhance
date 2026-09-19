@echo off
rem ===== Debug CRT (/MTd) pass: heap-block validation guard =====
rem ASCII only (cmd.exe reads .bat as ANSI).
rem
rem Why this exists: the normal test run links the Release CRT (/MT), which does
rem NOT validate heap blocks. A "wrote past end of heap buffer" bug (e.g. the
rem ConfigUpgradeFile BOM capacity miscalculation) therefore passes the whole
rem suite silently and only blows up in the user's Release build. This pass
rem runs the pure-logic config test on the Debug CRT, where an out-of-bounds
rem write aborts at free() with a heap-corruption dialog / exit code 3.
setlocal

call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "obj\test" mkdir obj\test
if not exist "obj" mkdir obj

cl /nologo /Od /W4 /MTd /utf-8 /DUNICODE /D_UNICODE /TP /Isrc /Foobj\ ^
   src\config.c src\keymap.c test\test_config.c ^
   /link /OUT:obj\test\test_config_dbg.exe ^
   user32.lib advapi32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1

echo [OK] compiled (Debug CRT). running test_config_dbg...
".\obj\test\test_config_dbg.exe"
exit /b %errorlevel%
