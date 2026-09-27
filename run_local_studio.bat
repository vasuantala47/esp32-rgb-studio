@echo off
title ESP32 AI RGB Pro Studio
echo =============================================================
echo   ✨ Launching ESP32 AI RGB Pro Studio (Laptop USB Bridge)
echo =============================================================
echo Laptop Wi-Fi will remain connected to your normal Internet!
echo Commands and audio beats will stream over USB Serial to ESP32!
echo.
"%USERPROFILE%\.platformio\penv\Scripts\python.exe" "%~dp0serve.py"
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo PlatformIO Python not found, trying default python...
    python "%~dp0serve.py"
)
pause
