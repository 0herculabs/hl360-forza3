@echo off
rem Extracts the FM3 Ultimate Collection discs and installs the Disc 2 content.
rem Usage: install_game.cmd "<Disc 1 .iso>" ["<Disc 2 .iso>"]
rem   Disc 1 -> game\disc1  (game data, default.xex)
rem   Disc 2 -> game\disc2, then its content packages -> user\0000000000000000\4D53084D\00000002
setlocal
set "ROOT=%~dp0.."
set "TOOL=%ROOT%\out\build\win-amd64-release\fm3tool.exe"
if not exist "%TOOL%" (
  echo fm3tool.exe not found. Run scripts\build.cmd first.
  exit /b 1
)
if "%~1"=="" (
  echo Usage: %~nx0 "Disc 1.iso" ["Disc 2.iso"]
  exit /b 2
)
"%TOOL%" iso "%~1" "%ROOT%\game\disc1" || exit /b 1
if "%~2"=="" exit /b 0
"%TOOL%" iso "%~2" "%ROOT%\game\disc2" || exit /b 1
"%TOOL%" install-dlc "%ROOT%\game\disc1" "%ROOT%\game\disc2" "%ROOT%\user" || exit /b 1
