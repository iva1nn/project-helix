@echo off
rem Fear Effect native port: the installer. Double-click it after putting your disc images into discs\ and
rem extracting the asset pack(s) into this folder. It gets Python (embeddable, from python.org) once, then runs
rem src\installer\install.py, which does the rest (README.md). Options: install.bat --help
setlocal EnableExtensions DisableDelayedExpansion
title Fear Effect native port - installer
set "HERE=%~dp0"
set "PYVER=3.11.9"
set "PYURL=https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip"
set "PYSHA=009D6BF7E3B2DDCA3D784FA09F90FE54336D5B60F0E0F305C37F400BF83CFD3B"
set "RC=1"

rem --- where we run from
if "%HERE:~0,2%"=="\\" goto err_unc
if not exist "%HERE%src\installer\install.py" goto err_unpacked
cd /d "%HERE%" || goto err_unc
if /i "%PROCESSOR_ARCHITECTURE%"=="x86" if not defined PROCESSOR_ARCHITEW6432 goto err_32bit
set "TC=%HERE%.toolchain"
set "PY=%TC%\python\python.exe"
set "PYZIP=%TC%\downloads\python-%PYVER%-embed-amd64.zip"
if not exist "%TC%\downloads" mkdir "%TC%\downloads" 2>nul
if not exist "%TC%\downloads" goto err_readonly
echo test> "%TC%\downloads\.write_test" 2>nul || goto err_readonly
del "%TC%\downloads\.write_test" 2>nul

rem --- Python (kept in .toolchain\python; checked each run, fetched again if broken)
call :python_ok && goto run
if exist "%TC%\python" rd /s /q "%TC%\python" 2>nul
call :zip_ok && goto unpack
echo Downloading Python %PYVER% (embeddable, 11 MB) from python.org ...
del "%PYZIP%" 2>nul
where curl.exe >nul 2>nul && curl.exe -L --fail --silent --show-error --retry 3 --retry-delay 5 --connect-timeout 30 -o "%PYZIP%.part" "%PYURL%" && move /y "%PYZIP%.part" "%PYZIP%" >nul
if not exist "%PYZIP%" call :download_ps
if not exist "%PYZIP%" goto err_download
call :zip_ok || (del "%PYZIP%" 2>nul & goto err_checksum)

:unpack
if exist "%TC%\python.tmp" rd /s /q "%TC%\python.tmp" 2>nul
mkdir "%TC%\python.tmp"
where tar.exe >nul 2>nul && tar.exe -xf "%PYZIP%" -C "%TC%\python.tmp" 2>nul
if not exist "%TC%\python.tmp\python.exe" powershell -NoProfile -NonInteractive -Command "Expand-Archive -Force -LiteralPath $env:PYZIP -DestinationPath ($env:TC + '\python.tmp')" >nul 2>nul
if not exist "%TC%\python.tmp\python.exe" goto err_unpack
move "%TC%\python.tmp" "%TC%\python" >nul || goto err_unpack
call :python_ok || goto err_python

:run
"%PY%" -B "%HERE%src\installer\install.py" %*
set "RC=%errorlevel%"
goto end

rem --- helpers
:python_ok
if not exist "%PY%" exit /b 1
"%PY%" -c "import ssl, zipfile, tarfile, hashlib, json, urllib.request" >nul 2>nul
exit /b %errorlevel%

:zip_ok
if not exist "%PYZIP%" exit /b 1
set "GOT="
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%PYZIP%" SHA256 2^>nul') do if not defined GOT set "GOT=%%h"
if not defined GOT exit /b 1
set "GOT=%GOT: =%"
if /i "%GOT%"=="%PYSHA%" exit /b 0
exit /b 1

:download_ps
powershell -NoProfile -NonInteractive -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; foreach ($i in 1..3) { try { Invoke-WebRequest -UseBasicParsing -TimeoutSec 60 -Uri $env:PYURL -OutFile ($env:PYZIP + '.part'); Move-Item -Force ($env:PYZIP + '.part') $env:PYZIP; exit 0 } catch { Write-Host ('  attempt ' + $i + ': ' + $_.Exception.Message); Start-Sleep 5 } }; exit 1"
exit /b 0

rem --- messages
:err_unc
echo INSTALL STOPPED: this folder is on a network path. Copy the whole folder to a local drive,
echo for example C:\Games\FearEffect, and run install.bat there.
goto end
:err_unpacked
echo INSTALL STOPPED: install.bat was started from inside the ZIP file, or the folder is incomplete.
echo Extract the whole ZIP first, for example to C:\Games\FearEffect, then run install.bat from there.
goto end
:err_32bit
echo INSTALL STOPPED: this PC runs 32-bit Windows. The port needs 64-bit Windows 10 or 11.
goto end
:err_readonly
echo INSTALL STOPPED: cannot write into this folder: %HERE%
echo Folders such as C:\Program Files are protected. Move the whole folder to a place of your own,
echo for example C:\Games\FearEffect, and run install.bat again.
goto end
:err_download
echo.
echo INSTALL STOPPED: Python could not be downloaded: no internet connection, or python.org did not answer.
echo Check the connection and run install.bat again. Without internet on this PC: download
echo     %PYURL%
echo on another PC and put the file into
echo     %TC%\downloads\
echo then run install.bat again.
goto end
:err_checksum
echo INSTALL STOPPED: the downloaded Python file is damaged: its checksum is wrong. Run install.bat again.
echo If this keeps happening, a proxy or antivirus may be changing downloads.
goto end
:err_unpack
echo INSTALL STOPPED: Python could not be unpacked into %TC%\python.
echo An antivirus program may be blocking it, or the disk is full. Run install.bat again.
goto end
:err_python
echo INSTALL STOPPED: the downloaded Python does not run. An antivirus program may have removed some of its
echo files: allow the folder %TC% in the antivirus, then run install.bat again.
if exist "%TC%\python" rd /s /q "%TC%\python" 2>nul
goto end

:end
echo.
if not "%FE_NO_PAUSE%"=="1" pause
exit /b %RC%
