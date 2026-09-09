@echo off
rem ===== diagnostic probes: keytest.exe + uiprobe.exe (MSVC x64 console) =====
rem ASCII only: cmd.exe reads .bat as ANSI, non-ASCII comments break parsing.
setlocal

call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "bin" mkdir bin

cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Fobin\ ^
   test\keytest.c /link /OUT:bin/keytest.exe user32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Fobin\ ^
   test\uiprobe.c /link /OUT:bin/uiprobe.exe user32.lib ole32.lib oleaut32.lib oleacc.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

rem memtest: include nav.c/send.c directly, link only its own obj
cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /Fobin\ ^
   test\memtest.c /link /OUT:bin/memtest.exe user32.lib gdi32.lib advapi32.lib ole32.lib oleaut32.lib oleacc.lib psapi.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Fobin\ ^
   test\restarttest.c /link /OUT:bin/restarttest.exe user32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

echo [OK] bin/keytest.exe + bin/uiprobe.exe + bin/memtest.exe + bin/restarttest.exe built.
exit /b 0
:fail
echo [ERROR] build failed
exit /b 1
