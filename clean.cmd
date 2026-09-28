@echo off
setlocal
if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
if not exist "%MSYS2_ROOT%\usr\bin\bash.exe" (
  echo MSYS2 was not found at "%MSYS2_ROOT%". Set MSYS2_ROOT to its install folder. 1>&2
  exit /b 1
)
if not defined SMS_ARCH set "SMS_ARCH=64"
set "MSYSTEM=MINGW%SMS_ARCH%"
set "CHERE_INVOKING=1"
set "PATH=%MSYS2_ROOT%\mingw%SMS_ARCH%\bin;%MSYS2_ROOT%\usr\bin;%PATH%"
pushd "%~dp0"
"%MSYS2_ROOT%\usr\bin\bash.exe" ./clean.sh %*
set "result=%ERRORLEVEL%"
popd
exit /b %result%
