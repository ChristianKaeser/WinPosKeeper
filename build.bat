@echo off
setlocal

:: Find Visual Studio installation via vswhere
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
    echo ERROR: vswhere.exe not found. Is Visual Studio installed? > build_log.txt
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -property installationPath`) do set VS_PATH=%%i
if not defined VS_PATH (
    echo ERROR: No Visual Studio installation found. > build_log.txt
    exit /b 1
)
call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=x86 >nul 2>&1

cd /d "%~dp0"
echo === BUILD START === > build_log.txt 2>&1
msbuild MonitorKeeper.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 /verbosity:normal >> build_log.txt 2>&1
echo === EXIT CODE: %ERRORLEVEL% === >> build_log.txt 2>&1
