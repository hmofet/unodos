@echo off
REM create-test-floppy.bat
REM Creates a FAT12 test floppy for UnoDOS filesystem testing
REM Run as Administrator

setlocal

set DRIVE=A:
set FILENAME=TEST.TXT

echo ========================================
echo UnoDOS Test Floppy Creator
echo ========================================
echo.

REM Check if drive exists
if not exist %DRIVE%\ (
    echo ERROR: Drive %DRIVE% not found!
    echo Please insert a floppy disk and try again.
    pause
    exit /b 1
)

echo Target drive: %DRIVE%
echo.

REM Warn about data loss
echo WARNING: This will FORMAT the floppy and ERASE ALL DATA!
set /p CONFIRM="Type YES to continue, or press Ctrl+C to cancel: "
if not "%CONFIRM%"=="YES" (
    echo Operation cancelled.
    pause
    exit /b 0
)

echo.
echo Step 1: Formatting floppy as FAT12...
echo.

REM Format as FAT12 (FAT on floppy = FAT12 automatically)
format %DRIVE% /FS:FAT /V:TEST /Q

if errorlevel 1 (
    echo.
    echo ERROR: Format failed!
    pause
    exit /b 1
)

echo.
echo Format completed successfully!
echo.

echo Step 2: Creating TEST.TXT file...
echo.

REM Create test file with multi-cluster content (>512 bytes)
REM Using echo with redirection
(
    echo CLUSTER 1: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
    echo CLUSTER 2: BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
) > %DRIVE%\%FILENAME%

if not exist %DRIVE%\%FILENAME% (
    echo.
    echo ERROR: Failed to create %FILENAME%
    pause
    exit /b 1
)

REM Check file size
for %%F in (%DRIVE%\%FILENAME%) do set FILESIZE=%%~zF

echo Created: %DRIVE%\%FILENAME%
echo Size: %FILESIZE% bytes
echo.

if %FILESIZE% LSS 512 (
    echo WARNING: File is smaller than 512 bytes - will not test multi-cluster
)

echo Step 3: Verification...
echo.
echo File exists: YES
echo Name format: %FILENAME% (8.3 format, uppercase)
echo Filesystem: FAT12
echo.

echo ========================================
echo SUCCESS! Test floppy is ready.
echo ========================================
echo.
echo Testing Instructions:
echo 1. Boot UnoDOS from the first floppy
echo 2. Wait for the keyboard demo prompt
echo 3. Press F to start filesystem test
echo 4. When prompted, remove UnoDOS floppy
echo 5. Insert this TEST floppy (drive %DRIVE%)
echo 6. Press any key to continue
echo 7. Observe test results:
echo    - Mount: OK
echo    - Open TEST.TXT: OK
echo    - Read: OK - File contents:
echo    - CLUSTER 1: AAA...
echo    - CLUSTER 2: BBB...
echo.
echo Expected: Both clusters should be displayed on screen
echo.
pause
