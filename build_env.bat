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
if exist "%VSW%" (
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
