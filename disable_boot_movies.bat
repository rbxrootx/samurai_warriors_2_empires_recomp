@echo off
setlocal

set "MOVIE_DIR=%~dp0..\game_files\data\movie"
set "STASH=%MOVIE_DIR%\_rexglue_disabled_wmv"

if not exist "%MOVIE_DIR%" (
  echo Movie folder was not found:
  echo   %MOVIE_DIR%
  exit /b 1
)

if not exist "%STASH%" mkdir "%STASH%"

for %%F in (MEV14.WMV MEV15.WMV MEV17.WMV) do (
  if exist "%MOVIE_DIR%\%%F" (
    move /Y "%MOVIE_DIR%\%%F" "%STASH%\%%F" >nul
  )
)

echo ReXGlue boot movie workaround is enabled.
echo Disabled movies are stored in:
echo   %STASH%
