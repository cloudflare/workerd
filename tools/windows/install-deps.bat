@echo off
setlocal enabledelayedexpansion

rem Check this script is being run from an elevated cmd binary.
net session > NUL 2>&1
if %ERRORLEVEL% NEQ 0 (
  echo This script requires administrator privileges.
  exit /b 1
)

cls

echo.Workerd dependency installation
echo.-------------------------------
echo.
echo.This script will configure your Windows machine for building workerd with
echo.bazel on Windows.
echo.
echo.Some of the steps will open windows for you to approve.
echo.
echo.* Step 1: Enable developer mode features.
echo.
rem This is based on https://learn.microsoft.com/en-us/windows/apps/get-started/developer-mode-features-and-debugging
reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock /v AllowDevelopmentWithoutDevLicense /t REG_DWORD /d 1 /f

echo.
echo.* Step 2: Enable 8.3 name support on this machine.
fsutil 8dot3name set 0

rem Install Visual Studio Code Community edition with Desktop development with C++ package.
rem ----------------------------------------------------------------------------------------
rem
rem The config in vsconfig.json is the config for "Desktop development with C++".
rem
rem The config is determined by first installing Visual Studio Community Edition on your
rem devbox. Then run `c:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe` and click
rem `More -> Export Configuration Settings`, select an config file to write, then `Review Details`
rem and select "Desktop development with C++".
echo.
echo.* Step 3: Install Visual Studio Code Community edition with Desktop development with C++ package.
winget install "Microsoft.VisualStudio.2022.Community" --override "install --config %~dp0\vsconfig.json"

echo.
echo.* Step 4: Install Python 3.
winget install Python3 -v 3.11.3 --override "/passive PrependPath=1"
@rem bazel requires a bazel3.exe binary, create a symlink for it.
mklink "%LOCALAPPDATA%\Programs\Python\Python311\python3.exe" "%LOCALAPPDATA%\Programs\Python\Python311\python.exe"

echo.
echo.* Step 5: Install msys2.
winget install msys2.msys2
setx BAZEL_SH "C:\msys64\usr\bin\bash.exe"

echo.
echo.* Step 6 Install msys2 tools suggested for bazel, from https://bazel.build/install/windows.
@rem Invoking pacman like this reports an error updating GNU info files, no idea how to fix.
c:\msys64\usr\bin\bash -c "/usr/bin/pacman -S --noconfirm zip unzip patch diffutils"

echo.
echo.* Step 7: Install LLVM compiler toolchain.
winget install "LLVM" --version 15.0.7

echo.
echo.* Step 8: Install bazelisk as %LOCALAPPDATA%\Programs\bazelisk\bazel.exe.
winget install bazelisk -l %LOCALAPPDATA%\Programs\bazelisk -r "bazel.exe"
rem Add bazelisk to users path. First we need to extract users path from the registry, then extend.
for /F "usebackq tokens=3 skip=2" %%P IN (`reg query "HKEY_CURRENT_USER\Environment" /v PATH`) do set "USERPATH=%%P"
setx PATH "%LOCALAPPDATA%\Programs\bazelisk;!USERPATH!"
echo Added %LOCALAPPDATA%\Programs\bazelisk to default PATH.

echo.
echo.* Setup complete.
set /p GO="Press enter to exit."
exit 0
