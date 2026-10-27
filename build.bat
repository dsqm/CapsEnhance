@echo off
rem ===== CapsEnhance portable build script (MSVC x64) =====
rem ASCII only (cmd.exe reads .bat as ANSI).
setlocal

call "%~dp0build_env.bat"
if errorlevel 1 exit /b 1

if not exist "bin" mkdir bin
if not exist "obj" mkdir obj
if not exist "obj" mkdir obj

rem ---- icon resource: the exe's own icon (Explorer / taskbar / tray) ----
rem rc.exe lives in the SDK's bin, which build_env.bat does NOT put on PATH.
rem NOTE: %SDK% contains "(x86)" - never expand it inside a ( ) block, goto only.
set "RC=%SDK%\bin\%SDKVER%\x64\rc.exe"
if exist "%RC%" goto have_rc
set "RC=%SDK%\bin\%SDKVER%\x86\rc.exe"
if exist "%RC%" goto have_rc
echo [ERROR] rc.exe not found under "%SDK%\bin\%SDKVER%"
call :maybe_pause
exit /b 1
:have_rc
"%RC%" /nologo /fo obj\app.res res\app.rc
if errorlevel 1 (
    echo [ERROR] resource compile failed
    call :maybe_pause
    exit /b 1
)
rem .res must be converted to a COFF obj for the linker. Keep the name flat
rem ("app_res.obj"): a double extension ("app.res.obj") makes cl treat the
rem file as C++ source when listed before /link.
cvtres /nologo /MACHINE:X64 /OUT:obj\app_res.obj obj\app.res
if errorlevel 1 (
    echo [ERROR] resource convert failed
    call :maybe_pause
    exit /b 1
)

rem /W4 instead of /W3: this project has already been bitten by silent
rem sign/size mismatches in cross-process message plumbing, and the extra
rem level costs nothing.
rem
rem /Brepro = reproducible build. Without it the PE header carries a build
rem timestamp (TimeDateStamp) plus a toolset-dependent "Rich" header, so two
rem builds of the SAME source with the SAME toolchain still produce different
rem SHA256. With it the exe is byte-identical, which is what makes
rem "is this artifact the same as the one I built?" answerable at all.
rem (Requires VS2019 16.6+ / MSVC 19.26+, i.e. any current toolset.)
rem
rem NOTE: identical *size* across two machines still requires the same MSVC
rem toolset and Windows SDK version -- those two embed different instruction
rem selection and CRT code. See docs/DEVELOPMENT.md ("reproducible builds /
rem cross-machine hash differences").
rem
rem /TP = compile every TU as C++. The sources are still written in C style
rem (see src/cap.h's CINTERFACE bridge); the switch exists so the codebase can
rem adopt C++ where it pays off (type safety / RAII / std::string) file by
rem file, instead of in one big-bang rewrite. Cross-checked in CI by the same
rem /W4 + /analyze steps, which must stay at zero warnings.
rem Source list: src\sources.txt, shared verbatim with test\build_analyze.bat
rem via cl's response-file syntax. A new src\*.c is therefore added in ONE
rem place -- the two lists used to drift (child.c was linked but never analyzed).
rem NOTE: a cl response file has NO comment syntax (rem becomes an unknown
rem option) and any '@' inside it nests another response file (D8035), so the
rem list is bare filenames only.  Run from the repo root: the paths in it are
rem relative, exactly like the ones on this command line.
rem /std:c++17: pin.cpp is a straight port of PowerToys' AlwaysOnTop, which uses
rem structured bindings and inline static members. Other TUs are unaffected
rem (C++17 is a superset of what they use).
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /TP /std:c++17 ^
   /Foobj\ ^
   @src\sources.txt ^
   /link /OUT:bin/CapsEnhance.exe ^
   obj\app_res.obj ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib imm32.lib winmm.lib ^
   d2d1.lib dwmapi.lib ^
   /SUBSYSTEM:WINDOWS

if errorlevel 1 goto fail_main

rem Settings editor: a separate exe on purpose -- the main program's thread
rem hosts the keyboard hook, so it must never own a modal editor window.
rem It links src\config.c: the form reads/writes config values through the
rem same ConfigGet*/ConfigSet* the main program uses, so ranges and key names
rem are never duplicated in the UI. settings\settings_int.h holds the field-row
rem definition.
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /TP /Isrc ^
   /Foobj\ ^
   settings\settings.c src\config.c ^
   /link /OUT:bin/CapsEnhanceSettings.exe ^
   user32.lib gdi32.lib shell32.lib advapi32.lib ^
   /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo [ERROR] settings build failed
    call :maybe_pause
    exit /b 1
)

rem Help overlay: a third exe, for the same kind of reason as the settings editor.
rem It draws a whole keyboard with GDI+ (and keeps a 32bpp DIB for per-pixel alpha),
rem owns its own show/hide lifetime, and is expected to start cold in tens of ms.
rem The main program is the keyboard-hook host -- none of that belongs in it.
rem It links nothing from src\ except the shared keymap table (a header).
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /TP /Isrc ^
   /Foobj\ ^
   help\help.c ^
   /link /OUT:bin/CapsEnhanceHelp.exe ^
   user32.lib gdi32.lib shell32.lib ole32.lib gdiplus.lib ^
   /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo [ERROR] help overlay build failed
    call :maybe_pause
    exit /b 1
)

rem Report what actually got built, with which toolchain. Cheap here, and it
rem turns "the two exes differ, why?" into a one-line diff of the logs.
for %%F in (bin\CapsEnhance.exe) do echo [OK] bin/CapsEnhance.exe built. size=%%~zF bytes
for %%F in (bin\CapsEnhanceSettings.exe) do echo [OK] bin/CapsEnhanceSettings.exe built. size=%%~zF bytes
where certutil >nul 2>&1 && certutil -hashfile bin\CapsEnhance.exe SHA256 | findstr /R /V "SHA256 CertUtil hash" 
call :maybe_pause
exit /b 0

rem ---- failure reporting ----
rem Why this block exists: the single most common failure here is LNK1104 on
rem bin\CapsEnhance.exe, because the program is a tray app that is normally
rem RUNNING, and Windows cannot overwrite an exe while a process has it mapped.
rem The linker only prints "LNK1104: cannot open file", which reads like a
rem permission problem -- and a double-clicked window closes before it can be
rem read. So: say what it actually means, and pause.
:fail_main
rem Two very different causes land here, and they need different advice:/r/nrem   (a) a real compile/link error  -> the messages above already say where;
rem   (b) LNK1104 on bin\CapsEnhance.exe -> the program is still running, and
rem       Windows cannot overwrite an exe while a process has it mapped.
rem (b) is by far the most common one here (this is a tray app you leave
rem running), and the linker reports it as "cannot open file", which reads
rem like a permission problem. tasklist tells the two apart for free.
rem findstr, not find: Git Bash puts its own /usr/bin/find ahead of System32,
rem so a plain "find" silently runs the Unix one (seen when building from a
rem bash shell) and the probe always fails.
tasklist /fi "imagename eq CapsEnhance.exe" 2>nul | findstr /i /c:"CapsEnhance.exe" >nul
if not errorlevel 1 goto fail_locked
echo.
echo [ERROR] build failed -- see the compiler/linker messages above.
call :maybe_pause
exit /b 1

:fail_locked
echo.
echo [ERROR] build failed: bin\CapsEnhance.exe is locked by a running copy.
echo.
echo   Windows cannot overwrite an exe while a process has it mapped, so the
echo   linker can only say "LNK1104: cannot open file". Exit CapsEnhance
echo   first (tray icon, right click, exit), then run build.bat again.
echo   Same for CapsEnhanceSettings / Help if you have those open.
call :maybe_pause
exit /b 1

rem Do not hang CI: GitHub Actions sets CI=true. Interactive runs (double
rem click) pause so the message can actually be read.
:maybe_pause
if /i "%CI%"=="true" exit /b 0
pause
exit /b 0
