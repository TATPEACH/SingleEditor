@echo off
setlocal EnableExtensions
if "%~1"=="" goto :usage
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\release\New-ReleasePayload.ps1" %*
exit /b %ERRORLEVEL%
:usage
echo Usage: build_portable_release.bat -CandidateRoot "candidate" -VgmRoot "runtime" -IconsRoot "icons" -CrtRoot "official-x64-CRT" -GuiIdentityBundlePath "gui-diagnostics.zip" -BuildCachePath "CMakeCache.txt" -CoreReceiptPath "release-receipt.json" -OutputDirectory "new-stage"
echo This creates a verified staging payload from explicit inputs. Fixed portable promotion is a separate release action.
exit /b 2
