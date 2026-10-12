@echo off
setlocal

set "QT_ROOT=C:\Qt\5.15.2\msvc2019_64"
if not exist "%QT_ROOT%\bin\qmake.exe" (
  echo Qt was not found at %QT_ROOT%
  echo Edit QT_ROOT in this script to match your Qt x64 installation.
  exit /b 1
)

cmake -S "%~dp0" -B "%~dp0build-win64" ^
  -G "Visual Studio 16 2019" -A x64 ^
  -DCMAKE_PREFIX_PATH="%QT_ROOT%"
if errorlevel 1 exit /b 1

cmake --build "%~dp0build-win64" --config Release
if errorlevel 1 exit /b 1

set "DIST=%~dp0dist-win64"
if not exist "%DIST%" mkdir "%DIST%"
copy /Y "%~dp0build-win64\Release\MseedConsumer.exe" "%DIST%\MseedConsumer.exe" >nul
if not exist "%DIST%\config.ini" copy /Y "%~dp0config.ini" "%DIST%\config.ini" >nul
"%QT_ROOT%\bin\windeployqt.exe" --release --no-translations "%DIST%\MseedConsumer.exe"
if errorlevel 1 exit /b 1

echo.
echo Build completed: %DIST%\MseedConsumer.exe
endlocal
