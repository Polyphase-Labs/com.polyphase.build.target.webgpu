@echo off
REM Native Addon Build Script for Windows
REM Run this from the root of your addon folder (where package.json is)
REM
REM Usage: build.bat [config]
REM   config - Optional. "Debug", "Release", or "Both" (default: Both)
REM
REM Requirements:
REM   - Visual Studio with C++ tools installed
REM   - Run from a "Developer Command Prompt" or ensure cl.exe is in PATH

setlocal enabledelayedexpansion

set "ADDON_NAME=com.polyphase.build.target.webgpu"
set "BUILD_CONFIG=%~1"
if "%BUILD_CONFIG%"=="" set "BUILD_CONFIG=Both"

echo.
echo ========================================
echo  Building Native Addon: %ADDON_NAME%
echo  Configuration: %BUILD_CONFIG%
echo ========================================
echo.

if not exist "Source" (
    echo ERROR: Source directory not found!
    exit /b 1
)

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: cl.exe not found!
    echo Please run this from a Visual Studio Developer Command Prompt.
    exit /b 1
)

set "SOURCES="
for /r "Source" %%f in (*.cpp) do (
    set "SOURCES=!SOURCES! "%%f""
)

set "BUILD_FAILED=0"

if /i "%BUILD_CONFIG%"=="Debug" goto :BuildDebug
if /i "%BUILD_CONFIG%"=="Both" goto :BuildRelease
if /i "%BUILD_CONFIG%"=="Release" goto :BuildRelease
goto :Summary

:BuildRelease
echo Building Release configuration...
if not exist "build\Windows\x64\Release" mkdir "build\Windows\x64\Release"
pushd build\Windows\x64\Release
cl /nologo /EHsc /O2 /MD /LD ^
    /I"..\..\..\..\Source" ^
    /Fe:"%ADDON_NAME%.dll" /Fo:"%ADDON_NAME%_" ^
    /D "OCTAVE_PLUGIN_EXPORT" /D "NDEBUG" /D "PLATFORM_WINDOWS=1" ^
    !SOURCES! /link /DLL /MACHINE:X64
if errorlevel 1 ( popd & echo Release build FAILED! & set "BUILD_FAILED=1" ) else ( popd & echo Release build succeeded )
certutil -hashfile "build\Windows\x64\Release\%ADDON_NAME%.dll" SHA256 > "build\Windows\x64\Release\%ADDON_NAME%-Windows-x64-Release.sha256" 2>nul

if /i "%BUILD_CONFIG%"=="Release" goto :Summary

:BuildDebug
echo Building Debug configuration...
if not exist "build\Windows\x64\Debug" mkdir "build\Windows\x64\Debug"
pushd build\Windows\x64\Debug
cl /nologo /EHsc /Od /MDd /LD /Zi ^
    /I"..\..\..\..\Source" ^
    /Fe:"%ADDON_NAME%.dll" /Fo:"%ADDON_NAME%_" /Fd:"%ADDON_NAME%.pdb" ^
    /D "OCTAVE_PLUGIN_EXPORT" /D "_DEBUG" /D "PLATFORM_WINDOWS=1" ^
    !SOURCES! /link /DLL /MACHINE:X64 /DEBUG
if errorlevel 1 ( popd & echo Debug build FAILED! & set "BUILD_FAILED=1" ) else ( popd & echo Debug build succeeded )
certutil -hashfile "build\Windows\x64\Debug\%ADDON_NAME%.dll" SHA256 > "build\Windows\x64\Debug\%ADDON_NAME%-Windows-x64-Debug.sha256" 2>nul

:Summary
echo.
if "%BUILD_FAILED%"=="1" ( echo BUILD COMPLETED WITH ERRORS ) else ( echo Build Succeeded! )
echo Output: build\Windows\x64\[Debug|Release]\%ADDON_NAME%.dll

if "%BUILD_FAILED%"=="1" exit /b 1
endlocal
