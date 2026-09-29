@echo off
rem Installs zPDFPropertyHandler into %ProgramFiles%\zPDFPropertyHandler and registers it:
rem   build\zPDFPropertyHandler.dll      -> 64-bit (Explorer, Windows Search, 64-bit apps)
rem   build\x86\zPDFPropertyHandler.dll  -> 32-bit (file dialogs of 32-bit apps), if it was built
rem Usage:  install.cmd   (asks for administrator rights if needed)
setlocal
net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

set "RC=1"
set "BUILD=%~dp0..\build"
set "DEST=%ProgramFiles%\zPDFPropertyHandler"
if not exist "%BUILD%\zPDFPropertyHandler.dll" (
    echo DLL not found: %BUILD%\zPDFPropertyHandler.dll
    echo Build it first with: mingw32-make dll   ^(and optionally: mingw32-make x86^)
    goto :done
)

call :install_one "%BUILD%\zPDFPropertyHandler.dll" "%DEST%" "%SystemRoot%\System32\regsvr32.exe" x64
if errorlevel 1 goto :done

if exist "%BUILD%\x86\zPDFPropertyHandler.dll" (
    if exist "%SystemRoot%\SysWOW64\regsvr32.exe" (
        call :install_one "%BUILD%\x86\zPDFPropertyHandler.dll" "%DEST%\x86" "%SystemRoot%\SysWOW64\regsvr32.exe" x86
        if errorlevel 1 goto :done
    )
) else (
    echo No 32-bit build found ^(build\x86^); skipping x86.
)

set "RC=0"
echo.
echo Restart Explorer (Task Manager ^> Windows Explorer ^> Restart) or sign out and in
echo so that already-running Explorer windows use the new handler.
echo To refresh Windows Search results: Indexing Options ^> Advanced ^> Rebuild.

:done
echo.
pause
exit /b %RC%

rem ---------------------------------------------------------------------------
rem :install_one <source dll> <dest dir> <regsvr32 path> <label>
:install_one
set "DLL=%~2\zPDFPropertyHandler.dll"
if not exist "%~2" mkdir "%~2"
if not exist "%~2" (
    echo Cannot create %~2
    exit /b 1
)
rem A DLL loaded by Explorer or another process can't be overwritten, but it can be renamed.
if exist "%DLL%" move /y "%DLL%" "%DLL%.%RANDOM%%RANDOM%.old" >nul
if exist "%DLL%" (
    echo Cannot replace %DLL%
    exit /b 1
)
call :cleanup_old "%~2"
copy /y "%~1" "%DLL%" >nul
if errorlevel 1 (
    echo Copy failed: %~1 -^> %DLL%
    exit /b 1
)
"%~3" /s "%DLL%"
if errorlevel 1 (
    echo Registration failed ^(%~4^).
    exit /b 1
)
echo Installed and registered ^(%~4^): %DLL%
exit /b 0

rem :cleanup_old <dir> - deletes replaced DLLs; ones still loaded are deleted at the next restart.
:cleanup_old
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
