@echo off
chcp 65001 >nul
echo ============================================
echo   UE Editor Process Cleanup
echo ============================================
echo.
echo Killing UE related processes...
echo.

taskkill /F /IM UnrealEditor.exe /T 2>nul
taskkill /F /IM CrashReportClientEditor.exe /T 2>nul
taskkill /F /IM LiveCodingConsole.exe /T 2>nul
taskkill /F /IM zenserver.exe /T 2>nul
taskkill /F /IM UnrealEditor-Win64-DebugGame.exe /T 2>nul

echo.
echo Waiting 3 seconds...
timeout /t 3 /nobreak >nul

echo.
echo ===== Remaining UE processes (should be 0) =====
tasklist 2>nul | findstr /I "UnrealEditor CrashReport LiveCoding zen"

echo.
echo ============================================
echo   Cleanup done. You can restart the editor.
echo ============================================
echo.
pause
