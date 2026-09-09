@echo off
rem ===== build + run word-jump unit test =====
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
   /Isrc /c /Fobin\nav_test.obj src\nav.c || exit /b 1
cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE /TC ^
   /Isrc /c /Fobin\test_nav.obj test\test_nav.c || exit /b 1
cl /nologo /MT /utf-8 /DUNICODE /D_UNICODE ^
   bin\test_nav.obj bin\nav_test.obj ^
   /link /OUT:bin\test_nav.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib advapi32.lib ^
   /SUBSYSTEM:CONSOLE || exit /b 1
echo [OK] compiled. running...
".\bin\test_nav.exe"
exit /b %errorlevel%