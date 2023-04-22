@echo off
echo.* Environment configured for workerd builds with bazel
echo.

rem Set environment variables for bazel to use when invoked from
rem the command-line.
set "BAZEL_LLVM=C:\Program Files\LLVM"
set BAZEL_SH=C:\msys64\usr\bin\bash.exe
set "BAZEL_VC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC"
set BAZEL_WINSDK_FULL_VERSION=10.0.22000.0
