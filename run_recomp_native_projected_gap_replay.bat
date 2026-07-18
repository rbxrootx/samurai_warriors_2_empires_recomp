@echo off
setlocal

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_recomp_native_transform_probe.ps1" -ProjectedGapReplay -DurationSeconds 60 -InitialDelaySeconds 5 -ReplayDrawLimit 12 %*
exit /b %ERRORLEVEL%
