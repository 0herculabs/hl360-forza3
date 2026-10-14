@echo off
rem Builds and installs the ReXGlue SDK (thirdparty/rexglue-sdk -> out/install/win-amd64).
call "%~dp0devenv.cmd"
cd /d "%~dp0..\thirdparty\rexglue-sdk"
sh "%~dp0fix_symlinks.sh" >nul
for %%p in ("%~dp0..\patches\sdk\*.patch") do (
  git apply --reverse --check "%%p" 2>nul || git apply "%%p" || exit /b 1
)
cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON || exit /b 1
cmake --build out/build/win-amd64 --config Release --target install || exit /b 1
