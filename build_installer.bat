@echo off
setlocal EnableExtensions
if "%~1"=="" goto :usage
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\release\Build-Installer.ps1" %*
exit /b %ERRORLEVEL%
:usage
echo Usage: build_installer.bat -StageRoot "verified-stage" -OutputDirectory "new-output-folder" [-CompilerPath "ISCC.exe"]
echo The installer consumes one verified payload. No executable rebuild or build-directory fallback is performed.
exit /b 2
