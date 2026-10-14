@echo off
rem Builds fm3.exe, fm3-launcher.exe and fm3tool.exe.
rem Usage: build.cmd [release|debug|relwithdebinfo]   (default: release)
setlocal
set "CFG=%~1"
if "%CFG%"=="" set "CFG=release"
call "%~dp0devenv.cmd"
cd /d "%~dp0.."
cmake --preset win-amd64-%CFG% -DREXSDK_DIR= -DCMAKE_PREFIX_PATH="%~dp0..\thirdparty\rexglue-sdk\out\install\win-amd64" || exit /b 1
cmake --build --preset win-amd64-%CFG% || exit /b 1
