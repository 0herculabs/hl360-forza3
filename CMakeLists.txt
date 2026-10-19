# fm3 - Forza Motorsport 3 recompiled for PC (ReXGlue)
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(fm3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FM3_BUILD_GAME "Build the game (requires Disc 1 and code generation)" ON)
include(generated/rexglue.cmake)

# Bootstrap codegen before configuring a game build so CMake discovers its
# source list. Tools-only builds must never depend on the disc or codegen.
if(FM3_BUILD_GAME AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sources.cmake")
    message(FATAL_ERROR "Run rexglue codegen fm3_manifest.toml before configuring the game, or use -DFM3_BUILD_GAME=OFF for a no-assets build")
endif()

# Keep all FM3 timing/readback settings; only select the platform backend.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/fm3.toml" FM3_PORT_CONFIG)
if(APPLE)
    string(REPLACE "gpu_backend = \"d3d12\"" "gpu_backend = \"vulkan\"" FM3_PORT_CONFIG "${FM3_PORT_CONFIG}")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fm3.toml" "${FM3_PORT_CONFIG}")

# --- Game ---------------------------------------------------------------------

if(FM3_BUILD_GAME)
    set(FM3_SOURCES
        src/main.cpp
        src/fm3_app.cpp
        src/crash_handler.cpp
        src/frame_stats.cpp
        src/file_signatures.cpp
    )

    if(WIN32)
        add_executable(fm3 WIN32 ${FM3_SOURCES})
    else()
        add_executable(fm3 ${FM3_SOURCES})
    endif()

    rexglue_setup_target(fm3 GPU_PLUGINS xenos)
    # GPU plugins load at runtime. Relink when an installed plugin changes so
    # the SDK's POST_BUILD copy cannot leave a stale renderer beside the game.
    if(TARGET rex::gpu-xenos)
        set_property(TARGET fm3 APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:rex::gpu-xenos>")
    endif()

    if(WIN32)
        # PDB so crash reports can name the recompiled functions (sub_XXXXXXXX).
        target_compile_options(fm3 PRIVATE -gline-tables-only)
        target_link_options(fm3 PRIVATE -g)
        target_link_libraries(fm3 PRIVATE dbghelp bcrypt)
    endif()

    target_include_directories(fm3 PRIVATE thirdparty/rexglue-sdk/thirdparty)
    add_custom_command(TARGET fm3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_BINARY_DIR}/fm3.toml $<TARGET_FILE_DIR:fm3>/fm3.toml)
endif()

# --- Launcher -----------------------------------------------------------------
#
# fm3-launcher: profiles, settings and the two-disc install wizard. SDL3 (static)
# comes from the SDK package; Dear ImGui, stb_image and toml++ from the SDK's
# thirdparty sources.

set(SDK_THIRDPARTY ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk/thirdparty)
set(IMGUI_DIR ${SDK_THIRDPARTY}/imgui)
find_package(SDL3 REQUIRED CONFIG)

# miniz (single-file release, zip64) reads the Project Forza Plus 3 archives.
# It is C; the project only enables C++.
enable_language(C)
include(FetchContent)
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(miniz)

add_executable(fm3-launcher
    launcher/src/main.cpp
    launcher/src/installer.cpp
    launcher/src/settings.cpp
    launcher/src/sha256.cpp
    src/crash_handler.cpp
    launcher/src/fm3_plus.cpp
    ${miniz_SOURCE_DIR}/miniz.c
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp
)
# The SDK include dir (pulled in by SDL3) also carries an imgui.h: keep these first.
target_include_directories(fm3-launcher BEFORE PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${SDK_THIRDPARTY}/stb
    ${SDK_THIRDPARTY}/tomlplusplus/include
    ${miniz_SOURCE_DIR}
)
set_target_properties(fm3-launcher PROPERTIES WIN32_EXECUTABLE $<BOOL:${WIN32}>)
target_link_libraries(fm3-launcher PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(fm3-launcher PRIVATE dbghelp)
endif()
# PDB for the crash report (src/crash_handler.cpp, shared with the game).
target_compile_options(fm3-launcher PRIVATE -gline-tables-only)
target_link_options(fm3-launcher PRIVATE -g)
# C++17: path::u8string() returns std::string there (C++20 made it u8string).
set_target_properties(fm3-launcher PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_dependencies(fm3-launcher fm3tool)

# Next to the game: the file list Disc 1 is verified against, and the port's
# own settings (fm3.user.toml, written by the launcher, overrides them).
add_custom_command(TARGET fm3-launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/launcher/data/game_files.sha256
        ${CMAKE_CURRENT_BINARY_DIR}/fm3.toml
        $<TARGET_FILE_DIR:fm3-launcher>)

# --- Tools --------------------------------------------------------------------

add_executable(fm3tool tools/fm3tool/main.cpp)
target_include_directories(fm3tool PRIVATE launcher/src)
target_link_libraries(fm3tool PRIVATE rex::runtime rex::kernel rex::system)

option(FM3_BUILD_TESTS "Build launcher regression tests" OFF)
if(FM3_BUILD_TESTS)
    enable_testing()
    add_executable(fm3-launcher-tests
        tests/launcher_tests.cpp
        launcher/src/installer.cpp
        launcher/src/fm3_plus.cpp
        launcher/src/sha256.cpp
        launcher/src/settings.cpp
        ${miniz_SOURCE_DIR}/miniz.c)
    target_include_directories(fm3-launcher-tests PRIVATE launcher/src ${miniz_SOURCE_DIR} ${SDK_THIRDPARTY}/tomlplusplus/include)
    target_link_libraries(fm3-launcher-tests PRIVATE SDL3::SDL3-static)
    set_target_properties(fm3-launcher-tests PROPERTIES CXX_STANDARD 17)
    add_test(NAME launcher-regressions COMMAND fm3-launcher-tests)
    add_executable(fm3-frame-stats-tests tests/frame_stats_tests.cpp src/frame_stats.cpp)
    target_include_directories(fm3-frame-stats-tests PRIVATE src)
    target_link_libraries(fm3-frame-stats-tests PRIVATE rex::runtime)
    add_test(NAME frame-stats-regressions COMMAND fm3-frame-stats-tests)
    add_executable(fm3-async-logging-tests tests/async_logging_tests.cpp)
    target_link_libraries(fm3-async-logging-tests PRIVATE rex::runtime)
    add_test(NAME async-logging-regressions COMMAND fm3-async-logging-tests)
    set_tests_properties(async-logging-regressions PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(fm3-host-path-tests tests/host_path_tests.cpp)
    target_link_libraries(fm3-host-path-tests PRIVATE rex::runtime)
    option(FM3_DISC_PATH_TESTS "Enable disc path tests against the selected SDK runtime" OFF)
    if(FM3_BUILD_GAME AND FM3_DISC_PATH_TESTS)
        add_test(NAME disc-path-regressions COMMAND fm3-host-path-tests "${CMAKE_CURRENT_SOURCE_DIR}/game/disc1")
    endif()
endif()
if(WIN32)
    add_custom_command(TARGET fm3tool POST_BUILD
        COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:fm3tool>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:fm3tool>;$<TARGET_FILE_DIR:fm3tool>>"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()
