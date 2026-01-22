@echo off
REM UnoDOS Floppy Writer for Windows
REM Writes UnoDOS image to a physical floppy disk
REM
REM Usage: writeflop.bat [image_file] [drive_letter]
REM   image_file  - Path to .img file (default: build\unodos-144.img)
REM   drive_letter - Floppy drive letter (default: A)
REM
REM Example: writeflop.bat build\unodos-144.img A

setlocal enabledelayedexpansion

echo ========================================
echo UnoDOS Floppy Writer for Windows
echo ========================================
echo.

REM Set defaults
set "IMAGE=%~1"
set "DRIVE=%~2"

if "%IMAGE%"=="" (
    if exist "build\unodos-144.img" (
        set "IMAGE=build\unodos-144.img"
    ) else if exist "build\unodos.img" (
        set "IMAGE=build\unodos.img"
    ) else (
        echo Error: No image file found.
        echo Run 'make' or 'make floppy144' first to build the image.
        echo Or specify an image file: writeflop.bat path\to\image.img
        goto :error
    )
)

if "%DRIVE%"=="" set "DRIVE=A"

REM Validate image exists
if not exist "%IMAGE%" (
    echo Error: Image file not found: %IMAGE%
    goto :error
)

REM Get image size
for %%A in ("%IMAGE%") do set "IMAGE_SIZE=%%~zA"
echo Image: %IMAGE% (%IMAGE_SIZE% bytes)
echo Drive: %DRIVE%:
echo.

echo WARNING: All data on drive %DRIVE%: will be destroyed!
echo.
set /p "CONFIRM=Continue? (Y/N): "
if /i not "%CONFIRM%"=="Y" (
    echo Aborted.
    goto :end
)

echo.
echo Writing image to %DRIVE%:...
echo.

REM Check for dd (from GnuWin32, Cygwin, or Git Bash)
where dd >nul 2>&1
if %errorlevel%==0 (
    echo Using dd...
    dd if="%IMAGE%" of=\\.\%DRIVE%: bs=512
    if %errorlevel%==0 (
        echo.
        echo Write complete!
        goto :success
    ) else (
        echo dd failed, trying alternate method...
    )
)

REM Check for PowerShell method
echo Using PowerShell...
powershell -Command "& { $bytes = [System.IO.File]::ReadAllBytes('%IMAGE%'); $stream = [System.IO.File]::Open('\\.\%DRIVE%:', 'Open', 'Write'); $stream.Write($bytes, 0, $bytes.Length); $stream.Close(); Write-Host 'Write complete!' }"
if %errorlevel%==0 goto :success

REM If all else fails, suggest RawWrite
echo.
echo Error: Could not write to floppy.
echo.
echo Please try one of these alternatives:
echo.
echo 1. Run this script as Administrator
echo.
echo 2. Use RawWrite for Windows:
echo    Download from: https://www.chrysocome.net/rawwrite
echo    Then: rawwrite.exe dd if=%IMAGE% of=%DRIVE%:
echo.
echo 3. Use Win32 Disk Imager:
echo    Download from: https://sourceforge.net/projects/win32diskimager/
echo.
goto :error

:success
echo.
echo ========================================
echo Success! Floppy is ready to boot.
echo ========================================
goto :end

:error
echo.
echo Operation failed.
exit /b 1

:end
endlocal
