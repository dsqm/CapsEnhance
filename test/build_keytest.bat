@echo off
rem ===== diagnostic probes: keytest.exe + uiprobe.exe (MSVC x64 console) =====
rem ASCII only: cmd.exe reads .bat as ANSI, non-ASCII comments break parsing.
setlocal

call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "obj\test" mkdir obj\test
if not exist "obj" mkdir obj

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Foobj\ ^
   test\keytest.c /link /OUT:obj\test\keytest.exe user32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Foobj\ ^
   test\uiprobe.c /link /OUT:obj\test\uiprobe.exe user32.lib ole32.lib oleaut32.lib oleacc.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

rem memtest: include nav.c/send.c directly, link only its own obj (+ nav_word.obj,
rem which nav.c calls into)
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Isrc /c /Foobj\navword_mem.obj src\nav_word.c
if errorlevel 1 goto fail
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Isrc /Foobj\ ^
   test\memtest.c /link /OUT:obj\test\memtest.exe obj\navword_mem.obj user32.lib gdi32.lib advapi32.lib ole32.lib oleaut32.lib oleacc.lib psapi.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Foobj\ ^
   test\restarttest.c /link /OUT:obj\test\restarttest.exe user32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Foobj\ ^
   test\traymemtest.c /link /OUT:obj\test\traymemtest.exe user32.lib psapi.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Foobj\ ^
   test\logsmoke.c /link /OUT:obj\test\logsmoke.exe user32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

rem Privilege / COM-attach diagnostics (compile only; running them needs WPS or explorer):
rem   ilcheck  prints each process integrity level (proves "elevated cannot attach to WPS")
rem   lowrun   starts any program at normal integrity via the shell token
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Foobj\ ^
   test\ilcheck.c /link /OUT:obj\test\ilcheck.exe advapi32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Foobj\ ^
   test\lowrun.c /link /OUT:obj\test\lowrun.exe userenv.lib advapi32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

rem Run at normal integrity: obj\test\lowrun.exe obj\test\wspset.exe obj\test\wspset.out
rem Purpose: check which of the three WPS "set caret directly" tricks work
rem       (Selection.SetRange / Range().Select / Selection.Start) and whether read-back matches.
rem       Re-run it after a WPS upgrade if word-jump lands elsewhere -- evidence, not guesswork.
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Foobj\ ^
   test\wspset.c /link /OUT:obj\test\wspset.exe user32.lib ole32.lib oleaut32.lib advapi32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

echo [OK] keytest + uiprobe + memtest + restarttest + traymemtest + logsmoke + ilcheck + lowrun + wspset built.
exit /b 0
:fail
echo [ERROR] build failed
exit /b 1
