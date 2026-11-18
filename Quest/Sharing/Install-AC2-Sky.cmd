@echo off
setlocal
powershell.exe -STA -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Quest.ps1" -SkyOnly %*
set "InstallResult=%ERRORLEVEL%"
if not "%InstallResult%"=="0" echo Sky transfer did not finish. Read the error above.
pause
exit /b %InstallResult%
