@echo off
rem Builds Orb.exe with MinGW-w64 (g++ and windres must be on PATH).  Optional arg: output file name.
cd /d "%~dp0"
set OUT=%~1
if "%OUT%"=="" set OUT=Orb.exe
if not exist orb.ico powershell -NoProfile -ExecutionPolicy Bypass -File make_icon.ps1 || exit /b 1
if not exist build mkdir build
rem GCC always links its own default-manifest.o; give it an empty one so ours (DPI + visual styles) is the only manifest.
type nul > build\empty.c
gcc -c build\empty.c -o build\default-manifest.o || exit /b 1
windres orb.rc -O coff -o build\orb_res.o || exit /b 1
g++ -Bbuild\ -O2 -std=c++17 -mwindows -static -s -fno-exceptions -fno-rtti orb.cpp build\orb_res.o -o "%OUT%" ^
    -ld3d11 -ldxgi -lole32 -lgdi32 -luser32 -lcomctl32 -lcomdlg32 -ldwmapi -lwinmm || exit /b 1
rmdir /s /q build
echo Built %~dp0%OUT%
