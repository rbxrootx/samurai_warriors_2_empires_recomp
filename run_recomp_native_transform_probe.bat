@echo off
setlocal

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_recomp_native_transform_probe.ps1" %*
exit /b %ERRORLEVEL%
