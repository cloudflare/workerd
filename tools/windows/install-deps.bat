@echo off
setlocal enabledelayedexpansion

rem Check this script is being run from an elevated cmd binary.
net session > NUL 2>&1
if %ERRORLEVEL% NEQ 0 (
  echo This script requires administrator privileges.
  exit /b 1
)

cls

goto :InstallDependencies

:AddToUserPathInEnvironment
rem Function to add an element to the users PATH as defined in HCU\Environment.
setlocal
set "PATH_TO_ADD=%~1"
for /F "usebackq tokens=3 skip=2" %%P IN (`reg query "HKEY_CURRENT_USER\Environment" /v PATH`) do set "USERPATH=%%P"
setx PATH "%PATH_TO_ADD%;!USERPATH!"
echo Added %PATH_TO_ADD% to default PATH.
endlocal
exit /b 0

:InstallDependencies

echo.Workerd dependency installation
echo.-------------------------------
echo.
echo.This script will configure your Windows machine for building workerd with
echo.bazel on Windows.
echo.
echo.* Step 1: Enable developer mode features.
echo.
rem This is based on https://learn.microsoft.com/en-us/windows/apps/get-started/developer-mode-features-and-debugging
reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock /v AllowDevelopmentWithoutDevLicense /t REG_DWORD /d 1 /f

echo.
echo.* Step 2: Enable 8.3 name support on this machine.
fsutil 8dot3name set 0

rem Install Visual Studio Code Community edition with C++ package.
rem
rem We add workloads on the command line with `--add` instead of using a vsconfig.json file, because
rem this allows for unattended installation. (With `--config` you still have to click OK in a dialog
rem box.) You can find the various workloads available for installation here:
rem
rem https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=vs-2022&preserve-view=true#desktop-development-with-c
winget install -e --id Microsoft.VisualStudio.2022.Community --version 17.12.3 --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.ASAN --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.Windows11SDK.22000"

echo.
echo.* Step 4: Install Python 3.
winget install Python3 -v 3.12.1 --override "/passive PrependPath=1"
@rem bazel requires a bazel3.exe binary, create a symlink for it.
mklink "%LOCALAPPDATA%\Programs\Python\Python312\python3.exe" "%LOCALAPPDATA%\Programs\Python\Python312\python.exe"

echo.
echo.* Step 5: Install msys2.
winget install msys2.msys2
setx BAZEL_SH "C:\msys64\usr\bin\bash.exe"

echo.
echo.* Step 6 Install msys2 tools suggested for bazel, from https://bazel.build/install/windows.
@rem Invoking pacman like this reports an error updating GNU info files, no idea how to fix.
C:\msys64\usr\bin\bash -c "/usr/bin/pacman -S --noconfirm zip unzip patch diffutils tcl
call :AddToUserPathInEnvironment C:\msys64\usr\bin

echo.
echo.* Step 7: Install bazelisk as %LOCALAPPDATA%\Programs\bazelisk\bazel.exe.
winget install bazelisk -l "%LOCALAPPDATA%\Programs\bazelisk" -r "bazel.exe"
call :AddToUserPathInEnvironment "%LOCALAPPDATA%\Programs\bazelisk"

echo.
echo.* Setup complete.
set /p GO="Press enter to exit."
exit 0
