@echo off
rem ===== diagnostic probes: keytest.exe + uiprobe.exe (MSVC x64 console) =====
rem ASCII only: cmd.exe reads .bat as ANSI, non-ASCII comments break parsing.
setlocal

call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "bin" mkdir bin

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Fobin\ ^
   test\keytest.c /link /OUT:bin/keytest.exe user32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Fobin\ ^
   test\uiprobe.c /link /OUT:bin/uiprobe.exe user32.lib ole32.lib oleaut32.lib oleacc.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

rem memtest: include nav.c/send.c directly, link only its own obj
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /Fobin\ ^
   test\memtest.c /link /OUT:bin/memtest.exe user32.lib gdi32.lib advapi32.lib ole32.lib oleaut32.lib oleacc.lib psapi.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Fobin\ ^
   test\restarttest.c /link /OUT:bin/restarttest.exe user32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Fobin\ ^
   test\traymemtest.c /link /OUT:bin/traymemtest.exe user32.lib psapi.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Fobin\ ^
   test\logsmoke.c /link /OUT:bin/logsmoke.exe user32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

rem 权限/COM 附着类排障（只编译；运行要看机器上有没有 WPS / explorer）：
rem   ilcheck  打印各进程的完整性级别（"管理员运行附不上普通权限的 WPS"就是它证的）
rem   lowrun   借 shell 令牌以普通权限启动任意程序（复现"普通权限能、管理员不能"）
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Fobin\ ^
   test\ilcheck.c /link /OUT:bin/ilcheck.exe advapi32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Fobin\ ^
   test\lowrun.c /link /OUT:bin/lowrun.exe userenv.lib advapi32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

rem 以普通权限跑：bin\lowrun.exe bin\wspset.exe bin\wspset.out
rem 用途：核对 WPS 的"直接置位"三招哪些可用（Selection.SetRange / Range().Select /
rem       Selection.Start 属性写入）以及回读是否一致。WPS 升级后若按词跳转落点变了，
rem       先跑它重新测一遍 —— 这是选型的证据来源，不是猜的。
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Fobin\ ^
   test\wspset.c /link /OUT:bin/wspset.exe user32.lib ole32.lib oleaut32.lib advapi32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

echo [OK] keytest + uiprobe + memtest + restarttest + traymemtest + logsmoke + ilcheck + lowrun + wspset built.
exit /b 0
:fail
echo [ERROR] build failed
exit /b 1
