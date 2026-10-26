@echo off
rem Build + run the native DNA editor on Windows (VS 2022 Build Tools + Panda3D 1.11 SDK at C:\Panda3D-1.11.0-x64).
rem Usage: build_windows.bat [run] [editor args...]
setlocal
set HERE=%~dp0
set VSDEV="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VSDEV% set VSDEV="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call %VSDEV% >nul || exit /b 1
if "%PANDA3D_ROOT%"=="" set PANDA3D_ROOT=C:\Panda3D-1.11.0-x64
cmake -S "%HERE%." -B "%HERE%build-win" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPANDA3D_ROOT="%PANDA3D_ROOT%" || exit /b 1
cmake --build "%HERE%build-win" || exit /b 1
if /i not "%1"=="run" exit /b 0
shift
set PATH=%PANDA3D_ROOT%\bin;%PATH%
cd /d "%HERE%."
"%HERE%build-win\app\dna-editor.exe" %1 %2 %3 %4 %5 %6
