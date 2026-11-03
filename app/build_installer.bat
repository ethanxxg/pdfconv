@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0.."

echo ============================================================
echo  DocForge installer build  (Inno Setup)
echo ============================================================
echo.

rem ---------------------------------------------------------------- 1/4
echo [1/4] Checking the packaged app in dist\DocForge ...
if not exist "dist\DocForge\DocForge.exe" goto :noapp
echo    OK - dist\DocForge\DocForge.exe

rem ---------------------------------------------------------------- 2/4
echo.
echo [2/4] Looking for Inno Setup (ISCC.exe) ...
set "ISCC="
call :find_iscc
if defined ISCC goto :compile
echo    Not installed - downloading Inno Setup (about 10 MB) ...
call :install_iscc
if errorlevel 1 goto :nodl
call :find_iscc
if not defined ISCC goto :nodl
echo    Installed: !ISCC!

rem ---------------------------------------------------------------- 3/4
:compile
echo.
echo [3/4] Compiling app\DocForge.iss ...
del /f /q "dist\installer\DocForge-*-setup.exe" >nul 2>&1
"!ISCC!" "app\DocForge.iss"
if errorlevel 1 goto :compilefail

rem ---------------------------------------------------------------- 4/4
echo.
echo [4/4] Done.
for %%F in ("dist\installer\DocForge-*-setup.exe") do echo    Installer: %%~fF
echo.
echo    Note: the installer is NOT code-signed, so Windows SmartScreen may
echo    warn on first run ("More info" - "Run anyway").
echo.
pause
exit /b 0

rem ================================================================ helpers
:find_iscc
if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" & goto :eof
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" & goto :eof
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe" & goto :eof
if exist "%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe" & goto :eof
if exist "%ProgramFiles(x86)%\Inno Setup 7\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 7\ISCC.exe" & goto :eof
goto :eof

:install_iscc
set "NUPKG=%TEMP%\innosetup.nupkg"
set "ISDIR=%TEMP%\innosetup_pkg"
set "ISEXE="
if exist "%NUPKG%" del /f /q "%NUPKG%" >nul 2>&1
if exist "%ISDIR%" rmdir /s /q "%ISDIR%" >nul 2>&1

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ProgressPreference='SilentlyContinue';" ^
  "try { Invoke-WebRequest -Uri 'https://community.chocolatey.org/api/v2/package/innosetup' -OutFile '%NUPKG%' -UseBasicParsing } catch { Write-Host $_.Exception.Message; exit 1 };" ^
  "Expand-Archive -Path '%NUPKG%' -DestinationPath '%ISDIR%' -Force"
if errorlevel 1 exit /b 1

for %%F in ("%ISDIR%\tools\innosetup-*.exe") do set "ISEXE=%%~fF"
if not defined ISEXE exit /b 1

echo    Installing per-user (no admin prompt, removable from Settings) ...
"%ISEXE%" /VERYSILENT /CURRENTUSER /NORESTART /SUPPRESSMSGBOXES /SP-
if errorlevel 1 exit /b 1
exit /b 0

rem ================================================================ errors
:noapp
echo.
echo    ERROR: dist\DocForge\DocForge.exe not found.
echo    Run app\build_exe_onedir.bat first, then run this script again.
echo.
pause
exit /b 1

:nodl
echo.
echo    ERROR: could not install Inno Setup automatically.
echo    Please install it manually from https://jrsoftware.org/isdl.php
echo    then run this script again.
echo.
pause
exit /b 1

:compilefail
echo.
echo    ERROR: ISCC failed. See the messages above.
pause
exit /b 1
