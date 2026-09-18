@echo off
rem ===== build + run chord regression test =====
setlocal

call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "bin" mkdir bin
rem test_chord.c includes main.c, which now also uses config.c and keymap.c
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\config_chord.obj src\config.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\keymap_chord.obj src\keymap.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\winmove_chord.obj src\winmove.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\drag_chord.obj src\drag.c
rem /DCAP_TEST: folder.c µÄ FolderResolveTarget »á°Ñ"ÍË³ÉÄ¿Â¼"Õâ¸öÅĞ¶¨½á¹û
rem Í¨¹ı¿ÉÑ¡ĞÎ²Î´ø³öÀ´¸ø¶ÏÑÔ¿´£¨Õæ·µ»ØÖµÔÚÁ½ÖÖÇé¿öÏÂ¿ÉÄÜ³¤µÃÒ»Ñù£¬
rem ¹â±È×Ö·û´®·Ö²»³ö×ßµÄÊÇÄÄÒ»Ö§£©¡£
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /DCAP_TEST /TP ^
   /Isrc /c /Foobj\folder_chord.obj src\folder.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /DCAP_TEST /TP ^
   /Isrc /c /Foobj\test_chord.obj test\test_chord.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE ^
   bin\test_chord.obj bin\config_chord.obj bin\keymap_chord.obj bin\winmove_chord.obj bin\drag_chord.obj ^
   bin\folder_chord.obj ^
   /link /OUT:bin/test_chord.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib imm32.lib winmm.lib ^
   /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
rem åæ ‡ç©ºé—´ï¼ˆDPI æ„ŸçŸ¥ï¼‰æµ‹è¯•ï¼šç‹¬ç«‹ exeï¼Œå¿…é¡»æ²¡æœ‰ä»»ä½•çª—å£æ—¶è·‘
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\winmove_dpi.obj src\winmove.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\dpitest.obj test\dpitest.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE ^
   bin\dpitest.obj bin\winmove_dpi.obj ^
   /link /OUT:bin/dpitest.exe user32.lib gdi32.lib shell32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
echo [OK] compiled. running dpitest...
".\bin\dpitest.exe"
if errorlevel 1 exit /b 1
echo [OK] compiled. running...
".\bin\test_chord.exe"
exit /b %errorlevel%