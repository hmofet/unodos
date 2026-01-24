@echo off
REM UnoDOS Boot Floppy Writer for Windows CMD
REM Run as Administrator
REM Usage: boot.bat [DriveLetter]

setlocal enabledelayedexpansion

set DRIVE=%1
if "%DRIVE%"=="" set DRIVE=A

set SCRIPT_DIR=%~dp0
set PROJECT_DIR=%SCRIPT_DIR%..

echo Pulling latest from GitHub...
cd /d "%PROJECT_DIR%"
git fetch origin 2>nul
git reset --hard origin/master 2>nul
if errorlevel 1 (
    echo Git failed, using local version
) else (
    echo Updated!
)

set IMAGE=%PROJECT_DIR%\build\unodos-144.img
if not exist "%IMAGE%" set IMAGE=%PROJECT_DIR%\build\unodos.img
if not exist "%IMAGE%" (
    echo ERROR: No image found in build directory
    exit /b 1
)

echo Writing to %DRIVE%:...

REM Use PowerShell for raw disk write
powershell -Command "$bytes = [System.IO.File]::ReadAllBytes('%IMAGE%'); $stream = [System.IO.File]::Open('\\.\%DRIVE%:', 'Open', 'Write', 'None'); $stream.Write($bytes, 0, $bytes.Length); $stream.Flush(); $stream.Close()"

if errorlevel 1 (
    echo FAILED! Run as Administrator.
    exit /b 1
)

echo Done! Boot floppy ready.
