@echo off
rem Double-click to run the service and open the browser.
rem Stops when this window is closed. For an always-on install use Docker or
rem register it with `sc create`, see service/README.md.
setlocal
cd /d "%~dp0.."

if "%PYTHON%"=="" set PYTHON=python
if "%PDFM_DATA%"=="" set PDFM_DATA=%~dp0data

"%PYTHON%" service\run.py --open %*
if errorlevel 1 pause
