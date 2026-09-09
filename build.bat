@echo off
rem ===== CapsEnhance portable build script (MSVC) =====
setlocal

set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSW%" -latest -products * -property installationPath`) do set "VS=%%i"

set "VCVARS=%VS%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [ERROR] vcvars64.bat not found: %VCVARS%
    exit /b 1
)

call "%VCVARS%" >nul

if not exist "bin" mkdir bin

cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE /TC ^
   /Fobin\ ^
   src\hook.c src\nav.c src\main.c ^
   /link /OUT:bin/CapsEnhance.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib advapi32.lib ^
   /SUBSYSTEM:WINDOWS

if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] bin/CapsEnhance.exe built.