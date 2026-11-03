@echo off
setlocal
cd /d "%~dp0.."

echo [1/6] Cleaning old output (kill running exe, remove build/dist)...
taskkill /f /im DocForge.exe >nul 2>&1
if exist "build\DocForge" rmdir /s /q "build\DocForge" >nul 2>&1
if exist "dist\DocForge" rmdir /s /q "dist\DocForge" >nul 2>&1
if exist "dist\DocForge.exe" del /f /q "dist\DocForge.exe" >nul 2>&1

echo [2/6] Installing core build dependencies...
python -m pip install --upgrade -r app\build-requirements.txt
if errorlevel 1 goto :err

echo [3/6] Optional OCR dependencies - failure is OK...
python -m pip install -r app\ocr-requirements.txt
if errorlevel 1 echo    OCR deps unavailable on this Python - building WITHOUT OCR

echo [4/6] Import smoke test...
python app\smoke_test.py
if errorlevel 1 goto :err

echo [5/6] Building DocForge.exe (may take a few minutes)...
python -m PyInstaller --noconfirm --clean app\DocForge.spec
if errorlevel 1 goto :err

echo [6/6] Done.
echo Output: %CD%\dist\DocForge.exe
pause
exit /b 0

:err
echo.
echo Build FAILED. See the messages above.
echo If it is a PermissionError on a .dll: close any DocForge.exe / Explorer
echo window on dist\ and run this script again.
pause
exit /b 1
