@echo off
setlocal

rem --- Locate Visual Studio (Build Tools or full IDE) via vswhere ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo vswhere.exe not found - is Visual Studio / Build Tools installed? & exit /b 1 )

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH ( echo No Visual Studio install with the C++ x64 toolset was found & exit /b 1 )

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars64 failed & exit /b 1 )

rem --- DeckLink SDK include dir (contains DeckLinkAPI.idl) ---
set "SDKINC=C:\Users\PC1\Desktop\DeckLink_SDK_16.0\Blackmagic DeckLink SDK 16.0\Win\include"
if not exist "%SDKINC%\DeckLinkAPI.idl" ( echo DeckLinkAPI.idl not found at "%SDKINC%" & exit /b 1 )

rem --- Build from the directory this script lives in ---
cd /d "%~dp0"

echo === MIDL: generating DeckLinkAPI_h.h / DeckLinkAPI_i.c ===
midl /nologo /h DeckLinkAPI_h.h /iid DeckLinkAPI_i.c /I "%SDKINC%" "%SDKINC%\DeckLinkAPI.idl"
if errorlevel 1 ( echo MIDL FAILED & exit /b 1 )

echo === CL: compiling DeckLinkPreview.exe ===
cl /nologo /EHsc /std:c++17 /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 main.cpp DeckLinkAPI_i.c /Fe:DeckLinkPreview.exe ^
   /link opengl32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib ws2_32.lib shell32.lib /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
echo EXITCODE=%ERRORLEVEL%
