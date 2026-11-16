@echo off
rem Started by RecompLauncher after install.bat --prepare. A normal install uses the Desktop shortcut.
setlocal EnableExtensions EnableDelayedExpansion
set "HERE=%~dp0"
set "GAME=fe2"
set "REST="

:nextarg
if "%~1"=="" goto picked
if /i "%~1"=="--game" (
  set "GAME=%~2"
  shift
  shift
  goto nextarg
)
set "ARG=%~1"
if /i "!ARG:~0,7!"=="--game=" (
  set "GAME=!ARG:~7!"
  shift
  goto nextarg
)
set "REST=!REST! "%~1""
shift
goto nextarg

:picked
if /i "%GAME%"=="fe1" goto launch
if /i "%GAME%"=="fe2" goto launch
echo Use --game fe1 or --game fe2.
exit /b 3

:launch
if /i "%GAME%"=="fe1" (
  set "DIR=%HERE%Fear Effect"
) else (
  set "DIR=%HERE%Fear Effect 2"
)
set "EXE=%DIR%\%GAME%port.exe"
if not exist "%EXE%" (
  echo This game has not been built yet. Set it up from its page in RecompLauncher.
  exit /b 3
)
set "CUE="
set "DISC=%HERE%discs\%GAME%"
set "LIST=%TEMP%\fe-play-%GAME%.txt"
dir /b /a-d "%DISC%\*(Disc 1)*.cue" >"%LIST%" 2>nul
for /f "usebackq delims=" %%f in ("%LIST%") do (
  findstr /b /l /c:"REM written by the installer" "%DISC%\%%f" >nul
  if not errorlevel 1 set "CUE=%DISC%\%%f"
)
if defined CUE goto havecue
for /f "usebackq delims=" %%f in ("%LIST%") do set "CUE=%DISC%\%%f"
:havecue
del "%LIST%" 2>nul
setlocal DisableDelayedExpansion
cd /d "%DIR%"
if defined CUE (
  "%EXE%" "%CUE%" %REST%
) else (
  "%EXE%" %REST%
)
exit /b %ERRORLEVEL%
