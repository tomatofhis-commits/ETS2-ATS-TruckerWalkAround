@echo off
setlocal
cd /d "%~dp0"

echo ======================================================================
echo  TruckerWalkAround - Full Build & Package Script (Split ATS & ETS2)
echo ======================================================================
echo.

echo [1/3] Building Plugin DLLs (TruckerWalkAround_ATS.dll & TruckerWalkAround_ETS2.dll)...
call build_dll.bat
if %errorlevel% neq 0 (
    echo [ERROR] DLL build failed!
    pause
    exit /b 1
)

echo.
echo [2/3] Building TruckerWalkAround.exe (C# WinForms GUI Manager)...
C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /target:winexe /out:TruckerWalkAround.exe /win32icon:TWAico.ico /platform:x64 /optimize+ TruckerWalkAroundManager.cs
if %errorlevel% neq 0 (
    echo [ERROR] Manager EXE build failed!
    pause
    exit /b 1
)

echo.
echo [3/3] Building TruckerWalkAround_Setup_v1.0.2.exe (Inno Setup)...
"G:\Program Files (x86)\Inno Setup 6\ISCC.exe" TruckerWalkAround_Setup.iss
if %errorlevel% neq 0 (
    echo [ERROR] Setup installer build failed!
    pause
    exit /b 1
)

echo.
echo ======================================================================
echo  [SUCCESS] All components built and packaged successfully!
echo  Installer Output: TruckerWalkAround_Setup_v1.0.2.exe
echo ======================================================================
pause
