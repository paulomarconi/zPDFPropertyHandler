@echo off
rem Unregisters zPDFPropertyHandler (64-bit and, if installed, 32-bit), restoring the previous
rem .pdf property handler and Explorer property lists, and removes %ProgramFiles%\zPDFPropertyHandler.
rem Files still loaded by Explorer are deleted at the next restart.
rem Usage:  uninstall.cmd   (asks for administrator rights if needed)
setlocal
net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

set "DEST=%ProgramFiles%\zPDFPropertyHandler"
call :remove_one "%DEST%\x86" "%SystemRoot%\SysWOW64\regsvr32.exe" x86
call :remove_one "%DEST%" "%SystemRoot%\System32\regsvr32.exe" x64

rmdir "%DEST%\x86" >nul 2>&1
rmdir "%DEST%" >nul 2>&1
if exist "%DEST%\x86" call :delete_at_reboot "%DEST%\x86"
if exist "%DEST%" call :delete_at_reboot "%DEST%"
echo Uninstalled.
echo.
pause
exit /b 0

rem ---------------------------------------------------------------------------
rem :remove_one <dir> <regsvr32 path> <label>
:remove_one
set "DLL=%~1\zPDFPropertyHandler.dll"
if exist "%DLL%" (
    "%~2" /u /s "%DLL%"
    if errorlevel 1 echo Unregistration reported an error ^(%~3^).
    del /f /q "%DLL%" >nul 2>&1
    if exist "%DLL%" move /y "%DLL%" "%DLL%.%RANDOM%%RANDOM%.old" >nul
    echo Unregistered ^(%~3^).
)
del /f /q "%~1\zPDFPropertyHandler.propdesc" >nul 2>&1
for %%F in ("%~1\*.old") do (
    del /f /q "%%~fF" >nul 2>&1
    if exist "%%~fF" call :delete_at_reboot "%%~fF"
)
exit /b 0

:delete_at_reboot
powershell -NoProfile -Command "$k = Add-Type -Name K -Namespace W -PassThru -MemberDefinition '[DllImport(\"kernel32.dll\", CharSet = CharSet.Unicode)] public static extern bool MoveFileEx(string a, string b, int f);'; if (-not $k::MoveFileEx('%~1', [NullString]::Value, 4)) { exit 1 }"
if errorlevel 1 (
    echo Could not schedule deletion of %~1
) else (
    echo In use, will be deleted at the next restart: %~nx1
)
exit /b 0
