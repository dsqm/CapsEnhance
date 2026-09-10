@echo off
rem ===== CapsEnhance: shared MSVC x64 toolchain setup =====
rem ASCII only: cmd.exe reads .bat as ANSI, non-ASCII comments break parsing.
rem vcvars64.bat is avoided on purpose: it needs registry probing (reg.exe) that
rem some environments block, and then leaves INCLUDE incomplete. Paths are set
rem explicitly instead, discovered from the VS install (works for any version).
rem
rem Gotchas handled here:
rem  1. "C:\Program Files (x86)" contains parentheses. They break any ( ) block
rem     or FOR set that expands such a path, so directories are resolved with
rem     pushd + a relative FOR, and errors use goto (never inline blocks).
rem  2. %ProgramFiles(x86)% cannot be expanded inline - its "(x86)" confuses the
rem     parser - so the x86 program-files dir is resolved by probing.

set "PFX86=C:\Program Files (x86)"
if not exist "%PFX86%" set "PFX86=C:\Program Files (x86)"
if not exist "%PFX86%" set "PFX86=%ProgramFiles%"

set "VS="
set "VSW=%PFX86%\Microsoft Visual Studio\Installer\vswhere.exe"
rem Optional pin: set CAPS_VSVER (e.g. "[17.0,18.0)") to force a VS generation.
rem Default is -latest, which is what makes a CI image update able to change the
rem toolset (and therefore the exe) under a green pipeline. Pin only when you
rem need byte-comparable artifacts across time.
if defined CAPS_VSVER (
    if exist "%VSW%" (
        for /f "usebackq tokens=*" %%i in (`"%VSW%" -latest -products * -version "%CAPS_VSVER%" -property installationPath`) do set "VS=%%i"
    )
)
if not defined VS if exist "%VSW%" (
    for /f "usebackq tokens=*" %%i in (`"%VSW%" -latest -products * -property installationPath`) do set "VS=%%i"
)
if not defined VS if exist "C:\VS2022\VC\Auxiliary\Build\vcvars64.bat" set "VS=C:\VS2022"
if not defined VS goto no_vs

rem ---- newest MSVC toolset ----
set "MSVCDIR="
pushd "%VS%\VC\Tools\MSVC"
if errorlevel 1 goto no_msvc
for /d %%d in (*) do set "MSVCDIR=%%d"
popd
if not defined MSVCDIR goto no_msvc
set "MSVC=%VS%\VC\Tools\MSVC\%MSVCDIR%"

rem ---- newest Windows SDK ----
set "SDK=%PFX86%\Windows Kits\10"
set "SDKVER="
pushd "%SDK%\Include"
if errorlevel 1 goto no_sdk
for /d %%d in (*) do set "SDKVER=%%d"
popd
if not defined SDKVER goto no_sdk

set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"

rem ---- Report the toolchain that was actually selected ----
rem Two machines (or two runner images) can build the same source into
rem different exe sizes simply because MSVC / Windows SDK differ: instruction
rem selection, inlined CRT code and header implementations all move with the
rem version. Printing this makes such a difference a one-line log diff
rem instead of a mystery. VS picked by vswhere -latest, so a newer VS
rem appearing on a CI image silently changes the toolset -- this line is how
rem you notice.
echo [ENV] VS=%VS%
echo [ENV] MSVC=%MSVCDIR%
echo [ENV] SDK=%SDKVER%
exit /b 0

:no_vs
echo [ERROR] Visual Studio not found
exit /b 1

:no_msvc
echo [ERROR] MSVC toolset not found under %VS%
exit /b 1

:no_sdk
echo [ERROR] Windows SDK not found under %SDK%
exit /b 1
