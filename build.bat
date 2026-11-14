@echo off
rem Builds the 32-bit mod DLL (dinput8.dll).
rem Output goes to build\ next to this file, or to %FC2FT_BUILD% if set
rem (use that when this folder's path is too long for the compiler).
set "BT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%BT%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set "PATH=%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
if not defined FC2FT_BUILD set "FC2FT_BUILD=%~dp0build"
cmake -S "%~dp0." -B "%FC2FT_BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%FC2FT_BUILD%" || exit /b 1
