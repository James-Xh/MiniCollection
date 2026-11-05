@echo off
setlocal

set "QT_ROOT=C:\Qt\5.15.2\msvc2019_64"
if not exist "%QT_ROOT%\bin\qmake.exe" (
  echo Qt was not found at %QT_ROOT%
  echo Edit QT_ROOT in this script to match your Qt x64 installation.
  exit /b 1
)

cmake -S "%~dp0." -B "%~dp0build-win64" ^
  -G "Visual Studio 16 2019" -A x64 ^
  -DCMAKE_PREFIX_PATH="%QT_ROOT%"
if errorlevel 1 exit /b 1

cmake --build "%~dp0build-win64" --config Release
if errorlevel 1 exit /b 1

set "RELDIR=%~dp0build-win64\Release"
set "DIST=%~dp0dist-win64"
if not exist "%DIST%" mkdir "%DIST%"
for %%F in ("%RELDIR%\*.exe") do copy /Y "%%~fF" "%DIST%\%%~nxF" >nul
rem Remove the legacy English-named console left by older builds.
if exist "%DIST%\MseedConsumer.exe" del /Q "%DIST%\MseedConsumer.exe"
if not exist "%DIST%\config.ini" copy /Y "%~dp0config.ini" "%DIST%\config.ini" >nul

rem The console is the only GUI executable; the watchdog keeps its English name.
set "MAIN_EXE="
for %%F in ("%DIST%\*.exe") do (
  if /I not "%%~nxF"=="MseedConsumerWatchdog.exe" set "MAIN_EXE=%%~fF"
)
if not defined MAIN_EXE (
  echo Console executable was not found in %DIST%
  exit /b 1
)
"%QT_ROOT%\bin\windeployqt.exe" --release --no-translations "%MAIN_EXE%"
if errorlevel 1 exit /b 1

echo.
echo Build completed: %MAIN_EXE%
endlocal
