@echo off
echo ========================================================
echo Flashing merged.hex to nRF52832 using SEGGER J-Link
echo ========================================================
"C:\Program Files\SEGGER\JLink_V982\JLink.exe" -CommanderScript "%~dp0flash_merged.jlink"
if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo Flash Successful! Device is now running ACDetector V1.0.5.
    echo ========================================================
) else (
    echo.
    echo Flash Failed. Please ensure J-Link is connected.
)
pause
