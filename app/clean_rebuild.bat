@echo off
setlocal
cd /d "%~dp0.."

echo [1/4] Killing running DocForge processes...
taskkill /f /im DocForge*.exe >nul 2>&1

echo [2/4] Removing old build and dist folders...
call :rmtree "build"
if errorlevel 1 goto :err
call :rmtree "dist"
if errorlevel 1 goto :err

echo [3/4] Import smoke test...
python app\smoke_test.py
if errorlevel 1 goto :err

echo [4/4] Building (onedir)...
python -m PyInstaller --noconfirm --clean app\DocForge_onedir.spec
if errorlevel 1 goto :err

echo Done: %CD%\dist\DocForge\DocForge.exe
pause
exit /b 0

:rmtree
if not exist %1 exit /b 0
rmdir /s /q %1 >nul 2>&1
if exist %1 (
  echo    %1 still locked - retry in 3s...
  timeout /t 3 >nul
  rmdir /s /q %1 >nul 2>&1
)
if exist %1 (
  echo    STILL locked: close Explorer / antivirus on %1 (or reboot), then rerun.
  exit /b 1
)
exit /b 0

:err
echo.
echo Build FAILED. See the messages above.
pause
exit /b 1
