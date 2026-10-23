@echo off
setlocal
set "LOOPZ_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%LOOPZ_VSWHERE%" (
  echo Install Visual Studio Build Tools with Desktop development with C++.
  exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%LOOPZ_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "LOOPZ_VS=%%I"
if not defined LOOPZ_VS exit /b 1
call "%LOOPZ_VS%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set "PATH=%LOOPZ_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
cd /d "%~dp0.."
call npm ci --ignore-scripts --prefix interface
if errorlevel 1 exit /b 1
call npm test --prefix interface
if errorlevel 1 exit /b 1
cmake -S . -B build\windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOOPZ_BUILD_WINDOWS_BACKEND=ON -DLOOPZ_BUILD_MACOS_BACKEND=OFF
if errorlevel 1 exit /b 1
cmake --build build\windows --parallel 4
if errorlevel 1 exit /b 1
ctest --test-dir build\windows --output-on-failure
if errorlevel 1 exit /b 1
cpack --config build\windows\CPackConfig.cmake -C Release
exit /b %ERRORLEVEL%
