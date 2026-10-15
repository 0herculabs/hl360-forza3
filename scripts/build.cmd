@echo off
rem Builds fm3.exe, fm3-launcher.exe and fm3tool.exe.
rem Usage: build.cmd [release|debug|relwithdebinfo]   (default: release)
setlocal
set "CFG=%~1"
if "%CFG%"=="" set "CFG=release"
call "%~dp0devenv.cmd"
cd /d "%~dp0.."
rem A clean checkout has no generated sources for CMake to discover yet.
if not exist "generated\default\sources.cmake" (
  "thirdparty\rexglue-sdk\out\install\win-amd64\bin\rexglue.exe" codegen fm3_manifest.toml || exit /b 1
)
cmake --preset win-amd64-%CFG% -DREXSDK_DIR= -DCMAKE_PREFIX_PATH="%~dp0..\thirdparty\rexglue-sdk\out\install\win-amd64" || exit /b 1
cmake --build --preset win-amd64-%CFG% || exit /b 1
