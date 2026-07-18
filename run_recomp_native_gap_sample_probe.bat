@echo off
setlocal

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_recomp_native_transform_probe.ps1" -DumpGapSamples -DurationSeconds 60 -InitialDelaySeconds 5 -SampleLimit 128 -SampleBytes 65536 %*
exit /b %ERRORLEVEL%
