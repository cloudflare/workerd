@setlocal enabledelayedexpansion
@echo off

@rem find git.exe
for /F "tokens=* USEBACKQ" %%F in (`where git.exe`) do (
  set "GIT=%%F"
  goto :found_git
)
:found_git

@rem Git's bash is located at %GITINSTALLDIR%\bin\bash.exe
@rem Git's git.exe is typically located at %GITINSTALLDIR%\cmd\git.exe,
@rem %GITINSTALLDIR%\mingw64\bin\git.exe, or %GITINSTALLDIR%\bin\git.exe.
set "_GITEXEDIR=%GIT:\cmd\git.exe=%"
set "_GITEXEDIR=%_GITEXEDIR:\mingw64\bin\git.exe=%"
set "GITEXEDIR=%_GITEXEDIR:\bin\git.exe=%"
set "BASH=%GITEXEDIR%\bin\bash.exe"

@rem Change to the directory of this script (tools/windows) to
@rem run bash script in (tools/unix).
cd "%~dp0"
"%BASH%" -c ../unix/workspace-status.sh
