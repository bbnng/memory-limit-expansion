@echo off
setlocal EnableExtensions
cd /d "%~dp0"
call :ensure_x86_cl || exit /b 1

if not exist build\obj mkdir build\obj
if not exist dist\Bin\loader mkdir dist\Bin\loader

rc /nologo /fo build\obj\memory_expansion.res src\memory_expansion.rc || exit /b 1
cl /nologo /std:c++17 /W4 /EHsc /MT /O2 /Oi /Ot /GL /arch:SSE2 /Oy- /DNOMINMAX /Isrc /Fobuild\obj\ /LD src\common.cpp src\d3d9_offload.cpp src\alloc_trace.cpp src\engine_patches.cpp src\split_server.cpp src\split_client.cpp build\obj\memory_expansion.res /Febuild\memory_expansion.vtm /link /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /IMPLIB:build\obj\memory_expansion.lib user32.lib kernel32.lib psapi.lib winmm.lib || exit /b 1
echo built build\memory_expansion.vtm

rc /nologo /fo build\obj\shaderapiempty.res src\shaderapiempty\shaderapiempty.rc || exit /b 1
cl /nologo /W4 /MT /O2 /Oi /Ot /GL /arch:SSE2 /Oy- /Isrc\shaderapiempty /Fobuild\obj\ /LD src\shaderapiempty\shaderapiempty.cpp build\obj\shaderapiempty.res /Febuild\shaderapiempty.dll /link /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /IMPLIB:build\obj\shaderapiempty.lib kernel32.lib || exit /b 1
echo built build\shaderapiempty.dll

copy /y build\memory_expansion.vtm dist\Bin\loader\ >nul || exit /b 1
copy /y package\memory_expansion.ini dist\Bin\loader\ >nul || exit /b 1
copy /y build\shaderapiempty.dll dist\Bin\ >nul || exit /b 1
copy /y LICENSE dist\ >nul || exit /b 1
copy /y NOTICE dist\ >nul || exit /b 1
echo staged release files in dist\
exit /b 0

:ensure_x86_cl
where cl >nul 2>&1 && (cl 2>&1 | findstr /c:"for x86" >nul) && exit /b 0
set "VSINSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL for %%v in (18 2022) do for %%e in (Community Professional Enterprise BuildTools) do if exist "%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\Common7\Tools\VsDevCmd.bat" set "VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\%%v\%%e"
if not defined VSINSTALL (echo error: Visual Studio C++ tools not found & exit /b 1)
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul 2>&1
where cl >nul 2>&1 || (echo error: VsDevCmd did not provide cl.exe & exit /b 1)
exit /b 0
