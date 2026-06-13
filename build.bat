@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars64 failed & exit /b 1 )

set "SDKINC=C:\Users\ac837\Desktop\DeckLink_SDK_16.0\Blackmagic DeckLink SDK 16.0\Win\include"
cd /d "C:\Users\ac837\Desktop\DeckLinkPreview"

echo === MIDL: generating DeckLinkAPI_h.h / DeckLinkAPI_i.c ===
midl /nologo /h DeckLinkAPI_h.h /iid DeckLinkAPI_i.c /I "%SDKINC%" "%SDKINC%\DeckLinkAPI.idl"
if errorlevel 1 ( echo MIDL FAILED & exit /b 1 )

echo === CL: compiling DeckLinkPreview.exe ===
cl /nologo /EHsc /std:c++17 /O2 /W3 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 main.cpp DeckLinkAPI_i.c /Fe:DeckLinkPreview.exe ^
   /link opengl32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib ws2_32.lib /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
echo EXITCODE=%ERRORLEVEL%
