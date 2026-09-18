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
exit /b 1
:have_rc
"%RC%" /nologo /fo obj\app.res res\app.rc
if errorlevel 1 (
    echo [ERROR] resource compile failed
    exit /b 1
)
rem .res must be converted to a COFF obj for the linker. Keep the name flat
rem ("app_res.obj"): a double extension ("app.res.obj") makes cl treat the
rem file as C++ source when listed before /link.
cvtres /nologo /MACHINE:X64 /OUT:obj\app_res.obj obj\app.res
if errorlevel 1 (
    echo [ERROR] resource convert failed
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
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /TP ^
   /Foobj\ ^
   src\hook.c src\nav.c src\send.c src\act.c src\config.c src\keymap.c src\main.c ^
   src\autodisp.c src\bridge.c src\wps.c src\winmove.c src\drag.c src\ime.c ^
   src\folder.c src\help.c src\clip.c ^
   /link /OUT:bin/CapsEnhance.exe ^
   obj\app_res.obj ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib imm32.lib winmm.lib ^
   /SUBSYSTEM:WINDOWS

if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

rem Settings editor: a separate exe on purpose -- the main program's thread
rem hosts the keyboard hook, so it must never own a modal editor window.
rem It links src\config.c: the form reads/writes config values through the
rem same ConfigGet*/ConfigSet* the main program uses, so ranges and key names
rem are never duplicated in the UI.
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /TP /Isrc ^
   /Foobj\ ^
   settings\settings.c src\config.c ^
   /link /OUT:bin/CapsEnhanceSettings.exe ^
   user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib uuid.lib ^
   /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo [ERROR] settings build failed
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
    exit /b 1
)

rem Clipboard overlay: a fourth exe, for the same reason as the help overlay --
rem it opens Ditto's SQLite database, decodes clipboard formats and writes them
rem back to the clipboard, none of which belongs on the hook-hosting thread.
rem
rem The SQLite amalgamation is compiled on its OWN cl line on purpose:
rem   - /W0 /analyze-  -- third-party code is not held to this repo's /W4 + /analyze
rem                       zero-warning baseline (it would never pass it).
rem   - no /TP         -- it is C, not C++; forcing C++ breaks the amalgamation.
rem   - /bigobj        -- an amalgamation is one huge TU; the default section
rem                       limit is exceeded otherwise.
rem It is linked into CapsEnhanceClip.exe ONLY, so the other three exes do not
rem grow by a single byte.
cl /nologo /O2 /W0 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /bigobj /c ^
   /D_CRT_SECURE_NO_WARNINGS /Ithirdparty ^
   /Foobj\sqlite3mc.obj ^
   thirdparty\sqlite3mc_amalgamation.c
if errorlevel 1 (
    echo [ERROR] sqlite3 build failed
    exit /b 1
)

rem colorcode.c: 色值识别（纯函数，只有剪贴板浮层用）—— 单独编一个 obj，
rem 和 sqlite 一样只链进 CapsEnhanceClip.exe，其余三个 exe 不涨一个字节。
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /TP /Isrc /c ^
   /Foobj\colorcode.obj src\colorcode.c
if errorlevel 1 (
    echo [ERROR] colorcode build failed
    exit /b 1
)

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /TP /Isrc /Ithirdparty ^
   /Foobj\ ^
   clip\clip.c ^
   /link /OUT:bin/CapsEnhanceClip.exe ^
   obj\sqlite3mc.obj obj\colorcode.obj ^
   user32.lib gdi32.lib shell32.lib ole32.lib gdiplus.lib bcrypt.lib advapi32.lib ^
   comctl32.lib ^
   /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo [ERROR] clipboard overlay build failed
    exit /b 1
)

rem Report what actually got built, with which toolchain. Cheap here, and it
rem turns "the two exes differ, why?" into a one-line diff of the logs.
for %%F in (bin\CapsEnhance.exe) do echo [OK] bin/CapsEnhance.exe built. size=%%~zF bytes
for %%F in (bin\CapsEnhanceSettings.exe) do echo [OK] bin/CapsEnhanceSettings.exe built. size=%%~zF bytes
for %%F in (bin\CapsEnhanceClip.exe) do echo [OK] bin/CapsEnhanceClip.exe built. size=%%~zF bytes
where certutil >nul 2>&1 && certutil -hashfile bin\CapsEnhance.exe SHA256 | findstr /R /V "SHA256 CertUtil hash" 
