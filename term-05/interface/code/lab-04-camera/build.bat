@echo off
rem Command-line build with MSVC. Adjust VCVARS if Visual Studio is installed elsewhere.
set "VCVARS=D:\d\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>nul
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /utf-8 /std:c++17 /O2 /EHsc /W4 /DUNICODE /D_UNICODE main.cpp /Fobin\ /Fe:bin\CamMonitor.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED
