@setlocal
@echo off

@rem Git can put git.exe in bin, cmd, or mingw64\bin. Select the first
@rem installation on PATH that has a Bash executable alongside it.
set "BASH="
for /F "tokens=* USEBACKQ" %%F in (`where git.exe`) do (
  for %%B in ("%%~dpFbash.exe" "%%~dpF..\bin\bash.exe" "%%~dpF..\..\bin\bash.exe") do (
    if not defined BASH if exist "%%~B" set "BASH=%%~fB"
  )
)

if not defined BASH (
  echo Could not find Git's bash.exe from git.exe on PATH. >&2
  exit /b 1
)

@rem Change to the directory of this script (tools/windows) to
@rem run bash script in (tools/unix).
cd /d "%~dp0"
"%BASH%" ../unix/workspace-status.sh
