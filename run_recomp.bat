@echo off
setlocal

set "ROOT=%~dp0"
set "EXE=%ROOT%out\build\win-amd64-debug\samurai_warriors_2_empires.exe"
set "GAME_ROOT=%ROOT%..\game_files"
set "MOD_ROOT=%ROOT%mods\loose"
set "LOG=%ROOT%runtime.log"
set "MOVE_WINDOW=%ROOT%tools\move_recomp_window.ps1"
set "INPUT_ARGS=--audio_mute=true --mnk_mode=true --mnk_capture_mouse=true --mnk_sensitivity=1.0 --keybind_a=Space --keybind_b=Shift --keybind_x=LMB --keybind_y=RMB --keybind_left_trigger=Z --keybind_right_trigger=Control --keybind_left_shoulder=Q --keybind_right_shoulder=F --keybind_lstick_up=W --keybind_lstick_down=S --keybind_lstick_left=A --keybind_lstick_right=D --keybind_lstick_press=C --keybind_rstick_press=MMB --keybind_dpad_up=Up --keybind_dpad_down=Down --keybind_dpad_left=Left --keybind_dpad_right=Right --keybind_back=Tab --keybind_start=Escape"

if not exist "%EXE%" (
  echo Recomp executable was not found:
  echo   %EXE%
  exit /b 1
)

if not exist "%GAME_ROOT%\default.xex" (
  echo Extracted game files were not found:
  echo   %GAME_ROOT%
  exit /b 1
)

pushd "%ROOT%out\build\win-amd64-debug"
if exist "%MOVE_WINDOW%" (
  start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%MOVE_WINDOW%" -Monitor nonprimary -WaitSeconds 20
)
if exist "%MOD_ROOT%" (
  "%EXE%" "--game_data_root=%GAME_ROOT%" "--mod_data_root=%MOD_ROOT%" "--log_file=%LOG%" --log_level=info %INPUT_ARGS%
) else (
  "%EXE%" "--game_data_root=%GAME_ROOT%" "--log_file=%LOG%" --log_level=info %INPUT_ARGS%
)
set "EXIT_CODE=%ERRORLEVEL%"
popd

exit /b %EXIT_CODE%
