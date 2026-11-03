@echo off
setlocal
cd /d "%~dp0.."

echo [1/7] Cleaning old output (kill running exe, remove build/dist)...
taskkill /f /im DocForge.exe >nul 2>&1
if exist "build\DocForge" rmdir /s /q "build\DocForge" >nul 2>&1
if exist "dist\DocForge" rmdir /s /q "dist\DocForge" >nul 2>&1
if exist "dist\DocForge.exe" del /f /q "dist\DocForge.exe" >nul 2>&1

echo [2/7] Installing core build dependencies...
python -m pip install --upgrade -r app\build-requirements.txt
if errorlevel 1 goto :err

echo [3/7] Optional OCR dependencies - failure is OK...
python -m pip install -r app\ocr-requirements.txt
if errorlevel 1 echo    OCR deps unavailable on this Python - building WITHOUT OCR

echo [4/7] Import smoke test + headless GUI click-through test...
python app\smoke_test.py
if errorlevel 1 goto :err
python app\gui_test.py
if errorlevel 1 goto :err

echo [5/7] Building DocForge (onedir, faster start-up)...
python -m PyInstaller --noconfirm --clean app\DocForge_onedir.spec
if errorlevel 1 goto :err

echo [6/7] Running the packaged exe self test (real conversion inside the exe)...
set DOCFORGE_SELFTEST=1
"dist\DocForge\DocForge.exe"
if errorlevel 1 echo    SELF TEST FAILED - see %%TEMP%%\docforge_selftest.log
if errorlevel 1 goto :err
set DOCFORGE_SELFTEST=

echo [7/7] Done.
echo Output folder: %CD%\dist\DocForge\
echo Run:           %CD%\dist\DocForge\DocForge.exe
echo.
echo Next: run app\build_installer.bat to build a setup.exe for distribution.
pause
exit /b 0

:err
echo.
echo Build FAILED. See the messages above.
echo If it is a PermissionError on a .dll: close any DocForge.exe / Explorer
echo window on dist\ and run this script again.
pause
exit /b 1
