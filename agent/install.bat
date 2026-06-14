@echo off
rem One-time installer for the KVMShare target agent. Copies the agent off the
rem (removable) USB drive into %APPDATA%, registers it to start at logon, starts
rem it now, and disables AutoPlay popups for a clean experience.
set "DEST=%APPDATA%\KVMShare"
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%~dp0agent.vbs" "%DEST%\agent.vbs" >nul
copy /y "%~dp0agent.ps1" "%DEST%\agent.ps1" >nul

reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v KVMShareAgent /t REG_SZ /d "wscript.exe \"%DEST%\agent.vbs\"" /f >nul

rem Stop the USB drive from auto-opening a folder / AutoPlay prompt each time.
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\AutoplayHandlers" /v DisableAutoplay /t REG_DWORD /d 1 /f >nul

rem (Re)start the agent now without showing a window. Stop any previous instance
rem first (old wscript launcher and any running powershell agent).
taskkill /f /im wscript.exe >nul 2>&1
taskkill /f /im powershell.exe >nul 2>&1
timeout /t 1 /nobreak >nul
start "" wscript.exe "%DEST%\agent.vbs"
