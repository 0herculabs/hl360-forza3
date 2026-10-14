@echo off
rem Builds glslangValidator (SPIR-V compiler for the Vulkan shader headers) from
rem the SDK's glslang sources into out\glslang.
call "%~dp0devenv.cmd"
cmake -S "%~dp0..\thirdparty\rexglue-sdk\thirdparty\glslang" -B "%~dp0..\out\glslang" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DENABLE_OPT=OFF -DENABLE_CTEST=OFF -DBUILD_TESTING=OFF -DENABLE_HLSL=OFF -DSKIP_GLSLANG_INSTALL=ON -DCMAKE_CXX_FLAGS=/w -DCMAKE_POLICY_VERSION_MINIMUM=3.5 || exit /b 1
cmake --build "%~dp0..\out\glslang" --target glslangValidator || exit /b 1
