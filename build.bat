@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Builds Pafta on Windows with Visual Studio 2022 (the C++ desktop workload).
rem Run from a Developer Command Prompt or Developer PowerShell:
rem
rem   build.bat            debug build (build\dev-msvc)
rem   build.bat run        debug build, then start the editor
rem   build.bat test       debug build, then run the tests
rem   build.bat release    release build (same folder, Release configuration)
rem   build.bat clean      delete build\dev-msvc
rem
rem The first build configures the folder; later builds only compile what changed.
setlocal
cd /d "%~dp0"

set "mode=%~1"
if "%mode%"=="" set "mode=debug"

if /i "%mode%"=="clean" (
    if exist build\dev-msvc rmdir /s /q build\dev-msvc
    echo Removed build\dev-msvc
    exit /b 0
)

set "config=Debug"
if /i "%mode%"=="release" set "config=Release"
if /i not "%mode%"=="debug" if /i not "%mode%"=="run" if /i not "%mode%"=="test" if /i not "%mode%"=="release" (
    echo Usage: build.bat [debug^|run^|test^|release^|clean]
    exit /b 2
)

if not exist build\dev-msvc\CMakeCache.txt (
    cmake --preset dev-msvc || exit /b 1
)
cmake --build build\dev-msvc --config %config% --parallel || exit /b 1

if /i "%mode%"=="run" start "" build\dev-msvc\Debug\pafta.exe
if /i "%mode%"=="test" ctest --test-dir build\dev-msvc -C Debug --output-on-failure || exit /b 1
if /i "%mode%"=="release" echo Built build\dev-msvc\Release\pafta.exe
exit /b 0
