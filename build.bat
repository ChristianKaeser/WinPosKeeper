@echo off
setlocal

:: Usage: build.bat [Win32^|x64]   (no argument builds both)
:: Output: Release\<platform>\WinPosKeeper.exe, full MSBuild output in build_log.txt

set PLATFORMS=%~1
if "%PLATFORMS%"=="" set PLATFORMS=Win32 x64

:: Find Visual Studio installation via vswhere
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
    echo ERROR: vswhere.exe not found. Is Visual Studio installed?
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS_PATH=%%i
if not defined VS_PATH (
    echo ERROR: No Visual Studio installation with the C++ desktop tools found.
    exit /b 1
)
call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=x86 >nul 2>&1

cd /d "%~dp0"
echo === BUILD START === > build_log.txt 2>&1
set BUILD_EXIT_CODE=0
for %%p in (%PLATFORMS%) do (
    echo === PLATFORM %%p === >> build_log.txt 2>&1
    msbuild WinPosKeeper.vcxproj /p:Configuration=Release /p:Platform=%%p /verbosity:normal >> build_log.txt 2>&1
    if errorlevel 1 (
        set BUILD_EXIT_CODE=1
        echo BUILD FAILED: %%p
    ) else (
        echo BUILD SUCCEEDED: Release\%%p\WinPosKeeper.exe
    )
)
echo === EXIT CODE: %BUILD_EXIT_CODE% === >> build_log.txt 2>&1

if not "%BUILD_EXIT_CODE%"=="0" echo See build_log.txt for details.
exit /b %BUILD_EXIT_CODE%
