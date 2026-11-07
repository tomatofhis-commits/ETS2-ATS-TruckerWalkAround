@echo off
setlocal
cd /d "%~dp0"

echo ========================================================
echo [TruckerWalkAround] Building Plugin DLLs (ATS & ETS2)
echo ========================================================

set "VCVARS_BAT=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS_BAT%" (
    echo [ERROR] Visual Studio x64 Build Tools not found.
    exit /b 1
)

call "%VCVARS_BAT%" >nul 2>&1

echo [1/2] Compiling TruckerWalkAround_ATS.dll...
cl.exe /O2 /LD /MT /EHsc /utf-8 trucker_walk_around_ats.cpp user32.lib psapi.lib /link /OUT:TruckerWalkAround_ATS.dll
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile TruckerWalkAround_ATS.dll
    exit /b 1
)

echo [2/2] Compiling TruckerWalkAround_ETS2.dll...
cl.exe /O2 /LD /MT /EHsc /utf-8 trucker_walk_around_ets2.cpp user32.lib psapi.lib /link /OUT:TruckerWalkAround_ETS2.dll
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile TruckerWalkAround_ETS2.dll
    exit /b 1
)

del /f /q *.obj *.exp *.lib >nul 2>&1
echo.
echo [SUCCESS] Both TruckerWalkAround_ATS.dll and TruckerWalkAround_ETS2.dll built successfully!
