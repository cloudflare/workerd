@ECHO off
SETLOCAL EnableDelayedExpansion

@REM cmd.exe script to ensure external/ directory is present for clangd.

FOR /f %%i IN ('bazel info output_path') do SET "output_path=%%i"
FOR /f %%i IN ('bazel info workspace') do SET "workspace=%%i"
SET "external=%output_path%\..\..\..\external"

@REM Delete the convenience junction external if it exists (it maybe stale if it does).
rmdir  "%workspace%\external" 2>NUL

@REM Create the convenience junction external anew for easy access to sources and for
@REM VSCode to be able to open files at points of error when compilation fails.
MKLINK /J "%workspace%\external" "%external%"

SET "compile_commands=%workspace%\compile_commands.json"
IF EXIST "%compile_commands%" (
  ECHO.WARNING: This workspace has a compile_commands.json file, but workerd
  ECHO.has moved to using compile_flags.txt for clangd instead. To improve
  ECHO.code completion and navigation in your editor, consider running:
  ECHO.
  ECHO.  DEL /q "%compile_commands%"
  ECHO.
)
