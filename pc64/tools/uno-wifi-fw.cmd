@echo off
REM Windows double-click launcher for uno-wifi-fw.py.
REM Puts the Intel WiFi firmware onto an UnoDOS USB stick.
setlocal
where py >nul 2>&1 && ( py "%~dp0uno-wifi-fw.py" %* ) || ( python "%~dp0uno-wifi-fw.py" %* )
echo.
pause
