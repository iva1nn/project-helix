@echo off
rem Fear Effect setup window. Double-click FearEffect-Setup.exe, or: setup.bat
rem The HTTP helper runs with no console; the Edge/Chrome app window is the UI.
setlocal EnableExtensions
cd /d "%~dp0" || exit /b 1

echo(%*| findstr /i /c:"--self-test" /c:"--console" >nul && goto foreground

if exist "%SystemRoot%\System32\wscript.exe" (
  wscript //nologo "%~dp0setup\silent.vbs" %*
  exit /b 0
)

:foreground
where pythonw >nul 2>nul && (
  pythonw -B setup\helix_setup.py %*
  exit /b %ERRORLEVEL%
)
where python >nul 2>nul && (
  python -B setup\helix_setup.py %*
  exit /b %ERRORLEVEL%
)
where py >nul 2>nul && (
  py -3 -B setup\helix_setup.py %*
  exit /b %ERRORLEVEL%
)
echo Python 3 is needed to open the setup window.
pause
exit /b 1
