@echo off
setlocal

set "MOVIE_DIR=%~dp0..\game_files\data\movie"
set "STASH=%MOVIE_DIR%\_rexglue_disabled_wmv"

if not exist "%MOVIE_DIR%" (
  echo Movie folder was not found:
  echo   %MOVIE_DIR%
  exit /b 1
)

if not exist "%STASH%" (
  echo No disabled boot movie folder was found.
  exit /b 0
)

for %%F in (MEV14.WMV MEV15.WMV MEV17.WMV) do (
  if exist "%STASH%\%%F" (
    move /Y "%STASH%\%%F" "%MOVIE_DIR%\%%F" >nul
  )
)

echo ReXGlue boot movie workaround is disabled.
