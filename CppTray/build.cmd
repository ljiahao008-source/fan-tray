@echo off
rem MechrevoMonitorTray C++ build script (MSVC)
rem Requires: Visual Studio 2022 Build Tools with VC toolchain

setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo [ERROR] VS Build Tools not found
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul

rc.exe /nologo /fo app.res app.rc || exit /b 1

cl.exe /nologo /std:c++17 /O2 /EHsc /DUNICODE /D_UNICODE /DNOMINMAX /W4 /utf-8 ^
  main.cpp tray.cpp widget.cpp monitor.cpp pawnio.cpp wmifan.cpp config.cpp autostart.cpp settingsdlg.cpp ^
  lockscreen.cpp lockscreendyn.cpp lockscreendlg.cpp ddcbrightness.cpp colortemp.cpp mainwindow.cpp ^
  /link /SUBSYSTEM:WINDOWS /OUT:MechrevoMonitorTray.exe app.res ^
  user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib comctl32.lib iphlpapi.lib comdlg32.lib || exit /b 1

if exist MechrevoMonitorTray.exe (
    echo [OK] BUILD SUCCESS - MechrevoMonitorTray.exe
) else (
    echo [ERROR] BUILD FAILED
    exit /b 1
)
