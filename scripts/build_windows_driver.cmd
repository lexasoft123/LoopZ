@echo off
setlocal
cd /d "%~dp0.."
set "LOOPZ_DRIVER=%CD%\native\platform\windows\driver"
set "LOOPZ_KITS=%LOOPZ_DRIVER%\packages"
where nuget.exe >nul 2>&1
if errorlevel 1 (
  echo NuGet CLI is required. Install the official nuget.exe and add it to PATH.
  exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -version "[17.0,18.0)" -products * -requires Component.Microsoft.Windows.DriverKit.BuildTools -property installationPath`) do set "LOOPZ_VS=%%I"
if not defined LOOPZ_VS (
  echo Install Visual Studio 2022 Windows Driver Kit Build Tools and x64 Spectre-mitigated libraries.
  exit /b 1
)
nuget.exe install Microsoft.Windows.WDK.x64 -Version 10.0.26100.6584 -OutputDirectory "%LOOPZ_KITS%" -NonInteractive
if errorlevel 1 exit /b 1
set "PATH=%LOOPZ_KITS%\Microsoft.Windows.WDK.x64.10.0.26100.6584\c\bin\10.0.26100.0\x64;%PATH%"
"%LOOPZ_VS%\MSBuild\Current\Bin\amd64\MSBuild.exe" "%LOOPZ_DRIVER%\LoopZAudio.vcxproj" /p:Configuration=Release /p:Platform=x64 /p:SignMode=Off /m:4 /nologo
if errorlevel 1 exit /b 1
if not exist "%LOOPZ_DRIVER%\build\package" mkdir "%LOOPZ_DRIVER%\build\package"
copy /y "%LOOPZ_DRIVER%\build\x64\LoopZAudio.sys" "%LOOPZ_DRIVER%\build\package\" >nul
copy /y "%LOOPZ_DRIVER%\build\x64\LoopZAudio.inf" "%LOOPZ_DRIVER%\build\package\" >nul
copy /y "%LOOPZ_DRIVER%\LICENSE" "%LOOPZ_DRIVER%\build\package\" >nul
"%LOOPZ_KITS%\Microsoft.Windows.WDK.x64.10.0.26100.6584\c\bin\10.0.26100.0\x86\Inf2Cat.exe" /driver:"%LOOPZ_DRIVER%\build\package" /os:10_VB_X64 /uselocaltime
if errorlevel 1 exit /b 1
echo Unsigned build only. No driver was installed, signed, or loaded.
