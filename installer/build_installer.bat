@echo off
setlocal
cd /d "%~dp0"
echo ================================================
echo  Temper - Build installer (Inno Setup)
echo ================================================
echo.

set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"

if exist "%ISCC%" goto have_iscc
echo ERROR: Inno Setup 6 not found at:
echo        %ISCC%
echo        Install it from https://jrsoftware.org/isinfo.php
exit /b 1

:have_iscc
if exist "..\build\dist\Temper.clap" goto have_clap
echo ERROR: build\dist\Temper.clap not found.
echo        Run build.bat first.
exit /b 1

:have_clap
if exist "..\build\dist\Temper.vst3" goto have_vst3
echo ERROR: build\dist\Temper.vst3 not found.
echo        Run build.bat first.
exit /b 1

:have_vst3
"%ISCC%" "Temper.iss"
if errorlevel 1 (
    echo ERROR: Inno Setup compile failed.
    exit /b 1
)

echo.
echo Installer built. See installer\output\
endlocal
