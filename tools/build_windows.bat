@echo off
REM Build the Windows x64 game with the windows-x64 preset.
REM Needs Visual Studio 2022, LLVM (clang++), CMake and glslangValidator.
REM Usage: tools\build_windows.bat [cmake --build args]
setlocal
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :error
set "PATH=C:\Program Files\LLVM\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
if defined VULKAN_SDK set "PATH=%VULKAN_SDK%\Bin;%PATH%"
cd /d "%~dp0.."
if not exist build\windows-x64\build.ninja (
    cmake --preset windows-x64
    if errorlevel 1 goto :error
)
cmake --build --preset windows-x64 %*
if errorlevel 1 goto :error
echo ==^> Done: build\windows-x64\src\opencmr2.exe
exit /b 0
:error
echo ==^> BUILD FAILED
exit /b 1
