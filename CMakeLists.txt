cmake_minimum_required(VERSION 3.20)
project(sacred_native_cpu C CXX)
option(SACRED_STRICT "Require strict native guest execution" ON)
option(SACRED_RELOCATABLE "Link with RUNPATH $ORIGIN/lib (macOS: @executable_path/lib) only (distribution bundle)" OFF)
# Runtime libraries (CPU/x87, process, control, window, HLE) use -O2: the guest thread spends most of its time there
# (measured with `sample` in the open world). Generated shards keep the -O1 compile budget (docs/ARCHITECTURE.md).
set(SACRED_RUNTIME_OPT -O2 CACHE STRING "Optimization level of the runtime libraries")
set(SACRED_GENERATED_OPT -O1 CACHE STRING "Optimization level of the generated guest-code shards")
option(SACRED_BUILD_GAME "Link the complete generated game shard set" OFF)
# Distribution: the runtime ships without game code and loads the game module (runtime/modules/game_module.h) that
# the launcher compiles on the player's machine; the build tree also builds that module (libsacred_game) to test it.
option(SACRED_GAME_MODULE "Load the generated game code from a shared library (--game-module)" OFF)
# QA hooks: --qa-* options, SACRED_QA_* environment switches and the WinMain checkpoint asserts.
option(SACRED_QA "Compile the QA hooks (OFF for distribution)" ON)
# The distribution's FFmpeg may be GPL-configured: never for distributed binaries.
option(SACRED_ALLOW_SYSTEM_FFMPEG "Allow the system FFmpeg when the pinned LGPL prefix is absent" OFF)
set(SACRED_ASSETS "" CACHE PATH
    "Read-only Sacred Gold installation (default: $SACRED_ASSETS); asset-dependent tests are skipped without it")
if(NOT SACRED_ASSETS AND DEFINED ENV{SACRED_ASSETS})
    set(SACRED_ASSETS "$ENV{SACRED_ASSETS}" CACHE PATH "" FORCE)
endif()
set(SACRED_PYTHON "${PROJECT_SOURCE_DIR}/build/venv/bin/python" CACHE FILEPATH
    "Python of the venv created by tools/bootstrap.sh")
set(SACRED_RECIPE "${PROJECT_SOURCE_DIR}/data/recipe.json.gz" CACHE FILEPATH
    "Translation recipe (tools/lift/recipe.py make); the export is used when empty")
set(SACRED_EXPORT_DIR "${PROJECT_SOURCE_DIR}/analysis/export" CACHE PATH
    "Ghidra IR export produced from the user's own game files (tools/bootstrap.sh --export)")
set(SACRED_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated" CACHE PATH
    "Generated game shards (tools/bootstrap.sh --generate); always inside a build directory")
if(SACRED_QA)
    add_compile_definitions(SACRED_QA=1)
endif()
if(APPLE)
    # macOS Apple Silicon: SDL2, FreeType and pkg-config come from the private prefix built by
    # tools/build_macos_deps.sh; only that prefix (and the pinned FFmpeg) is visible to pkg-config, never Homebrew.
    if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        message(FATAL_ERROR "macOS builds target Apple Silicon (arm64) only")
    endif()
    set(SACRED_MACOS_DEPS "${PROJECT_SOURCE_DIR}/build/macos-deps/prefix" CACHE PATH
        "Private dependency prefix built by tools/build_macos_deps.sh")
    if(NOT EXISTS "${SACRED_MACOS_DEPS}/bin/pkg-config")
        message(FATAL_ERROR "${SACRED_MACOS_DEPS} is missing. "
            "Run tools/build_macos_deps.sh (or pass -DSACRED_MACOS_DEPS=<prefix>).")
    endif()
    set(PKG_CONFIG_EXECUTABLE "${SACRED_MACOS_DEPS}/bin/pkg-config" CACHE FILEPATH "" FORCE)
    set(ENV{PKG_CONFIG_LIBDIR} "${SACRED_MACOS_DEPS}/lib/pkgconfig")
    # The OpenGL framework is deprecated, not removed; the renderer stays on the 4.1 core profile
    # (docs/ARCHITECTURE.md).
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()
if(NOT APPLE)
    # Linux: SDL2 and FreeType from tools/build_linux_deps.sh when that prefix exists (the distribution bundle uses
    # it); otherwise the system's development packages.
    set(SACRED_LINUX_DEPS "${PROJECT_SOURCE_DIR}/build/linux-deps/prefix" CACHE PATH
        "Private SDL2/FreeType prefix built by tools/build_linux_deps.sh (used when it exists)")
    if(EXISTS "${SACRED_LINUX_DEPS}/lib/pkgconfig/sdl2.pc")
        set(ENV{PKG_CONFIG_PATH} "${SACRED_LINUX_DEPS}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
        message(STATUS "SDL2 and FreeType from ${SACRED_LINUX_DEPS}")
    endif()
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
if(SACRED_RELOCATABLE)
    # Distribution: source and object paths in debug info and in the macOS debug map relative to the source tree, so
    # no path of the build machine ends up in a shipped binary.
    add_compile_options("-ffile-prefix-map=${PROJECT_SOURCE_DIR}=.")
    if(APPLE)
        add_link_options("LINKER:-oso_prefix,${PROJECT_SOURCE_DIR}/")
    endif()
endif()
# Guest faults, thread exits and process exits are C++ exceptions that unwind through generated C frames and
# sacred_drive. That needs unwind tables on every C object under both GCC and clang, so request them explicitly.
add_compile_options("$<$<COMPILE_LANGUAGE:C>:-fexceptions>")
file(GLOB SOFTFLOAT_SOURCES CONFIGURE_DEPENDS
    third_party/bochs/softfloat3e/*.cc
    third_party/bochs/softfloat3e/8086-SSE/*.cc
    third_party/bochs/fpu/*.cc)
# Shared on purpose: Bochs numerics are LGPL-2.1+, and a separate shared object lets users relink/replace them (LGPL
# section 6).
add_library(sacred_softfloat SHARED ${SOFTFLOAT_SOURCES})
target_include_directories(sacred_softfloat PUBLIC third_party/bochs
    third_party/bochs/softfloat3e/include)
target_compile_options(sacred_softfloat PRIVATE -include config.h)
# Test-only static archive (libsacred_softfloat.a) for the vendored recomp-kit and CPU differential harnesses; never
# linked into sacred-native.
add_library(sacred_softfloat_static STATIC ${SOFTFLOAT_SOURCES})
set_target_properties(sacred_softfloat_static PROPERTIES OUTPUT_NAME sacred_softfloat)
target_include_directories(sacred_softfloat_static PUBLIC third_party/bochs third_party/bochs/softfloat3e/include)
target_compile_options(sacred_softfloat_static PRIVATE -include config.h)
add_library(sacred_cpu STATIC runtime/cpu/x87_arithmetic.cpp
    runtime/cpu/x87_transcendental.cpp runtime/cpu/x87_state.cpp runtime/cpu/simd.cpp)
target_include_directories(sacred_cpu PUBLIC third_party/recomp-kit/runtime)
target_link_libraries(sacred_cpu PUBLIC sacred_softfloat)
target_compile_options(sacred_cpu PRIVATE -Wall -Wextra -Werror)
foreach(target sacred_cpu sacred_softfloat sacred_softfloat_static)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(${target} PRIVATE ${SACRED_RUNTIME_OPT} -g1 -fno-fast-math -ffp-contract=off)
endforeach()
enable_testing()
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
# No JSON file in data/ or tools/lift/ carries bytes of the game, only lengths and sizes.
add_test(NAME data_no_game_bytes COMMAND "${Python3_EXECUTABLE}" -I
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/check_no_game_bytes.py")
add_executable(cpu_state_tests tests/cpu/state_tests.c)
target_compile_options(cpu_state_tests PRIVATE -Wall -Wextra -Werror
    -fno-fast-math -ffp-contract=off)
target_link_libraries(cpu_state_tests PRIVATE sacred_cpu)
add_test(NAME cpu_state COMMAND cpu_state_tests)
add_executable(x87_fast_tests tests/cpu/x87_fast_tests.c)
target_compile_options(x87_fast_tests PRIVATE -Wall -Wextra -Werror -O1
    -fno-fast-math -ffp-contract=off)
target_link_libraries(x87_fast_tests PRIVATE sacred_cpu)
add_test(NAME cpu_x87_fast COMMAND x87_fast_tests)
if(EXISTS "${SACRED_PYTHON}")
    # Region flag liveness and block ticks of the shard generator (tools/lift/generate.py).
    add_test(NAME lift_flag_liveness COMMAND "${SACRED_PYTHON}" -m pytest -q -p no:cacheprovider
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lift/test_flag_liveness.py")
    add_test(NAME lift_resolution_table COMMAND "${SACRED_PYTHON}" -m pytest -q -p no:cacheprovider
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lift/test_resolution.py")
    # Translation recipe: the export's listing without the game's bytes (tools/lift/recipe.py).
    add_test(NAME lift_recipe COMMAND "${SACRED_PYTHON}" -m pytest -q -p no:cacheprovider
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lift/test_recipe.py")
    # The translator's own PE reader against pefile (third_party/recomp-kit/tools/recomp/pe.py).
    add_test(NAME lift_pe_reader COMMAND ${CMAKE_COMMAND} -E env "SACRED_ASSETS=${SACRED_ASSETS}"
        "${SACRED_PYTHON}" -m pytest -q -p no:cacheprovider "${CMAKE_CURRENT_SOURCE_DIR}/tests/lift/test_pe.py")
    # The translator's own x86 decoder against capstone, on every instruction of the game when a recipe is at hand
    # (third_party/recomp-kit/tools/recomp/x86dec.py).
    add_test(NAME lift_x86_decoder COMMAND ${CMAKE_COMMAND} -E env "SACRED_ASSETS=${SACRED_ASSETS}"
        "SACRED_RECIPE=${SACRED_RECIPE}" "${SACRED_PYTHON}" -m pytest -q -p no:cacheprovider
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lift/test_x86dec.py")
endif()
# The whole translation, byte for byte against data/generated-digest.json (tools/lift/generated_digest.py): guards
# the translator's machinery (PE reader, decoder, Python) against changing the generated C. About a minute.
if(SACRED_ASSETS AND (SACRED_RECIPE OR EXISTS "${SACRED_EXPORT_DIR}/sacred-startup"))
    find_package(Python3 3.11 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME lift_generated_identical COMMAND ${CMAKE_COMMAND} -E env
            "SACRED_ASSETS=${SACRED_ASSETS}" "SACRED_RECIPE=${SACRED_RECIPE}"
            "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/lift/generated_digest.py" run
            --out "${CMAKE_CURRENT_BINARY_DIR}/generated-digest")
        set_tests_properties(lift_generated_identical PROPERTIES TIMEOUT 900 LABELS slow)
    endif()
endif()

find_package(Threads REQUIRED)
add_library(sacred_process STATIC platform/guest_arena.cpp platform/digest.cpp
    runtime/process/memory.cpp runtime/process/loader.cpp)
target_include_directories(sacred_process PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(sacred_process PRIVATE -Wall -Wextra -Werror ${SACRED_RUNTIME_OPT} -g1)
add_executable(process_tests tests/process/loader_tests.cpp)
target_link_libraries(process_tests PRIVATE sacred_process)
target_compile_options(process_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
add_test(NAME process_fixtures COMMAND process_tests)
# Opt-in screen size: Settings.cfg keys and the guest writes of a resolution table (a fixture table, not the
# generated one).
add_executable(hd_resolution_tests tests/process/resolution_tests.cpp runtime/modules/resolution.cpp)
target_link_libraries(hd_resolution_tests PRIVATE sacred_process)
target_compile_options(hd_resolution_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
add_test(NAME hd_resolution COMMAND hd_resolution_tests)
# The launcher's settings page: Settings.cfg editing, option choices, screen sizes.
add_executable(launcher_settings_tests tests/launcher/settings_tests.cpp runtime/launcher/settings.cpp)
target_include_directories(launcher_settings_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(launcher_settings_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
add_test(NAME launcher_settings COMMAND launcher_settings_tests)
add_test(NAME process_fixtures_16k COMMAND process_tests --host-page 16384)
if(SACRED_ASSETS)
    add_test(NAME process_images COMMAND process_tests --assets "${SACRED_ASSETS}")
    add_test(NAME process_images_16k COMMAND process_tests --host-page 16384 --assets "${SACRED_ASSETS}")
else()
    message(STATUS "SACRED_ASSETS not set: asset-dependent tests are not registered")
endif()

add_library(sacred_control STATIC runtime/process/control.cpp
    runtime/process/boundary.c third_party/recomp-kit/runtime/seh.cpp)
if(NOT SACRED_GAME_MODULE)
    # The Sacred.exe stand-ins call generated code: with a game module they are compiled into it.
    target_sources(sacred_control PRIVATE runtime/sacred/native.c)
endif()
# The runtime arms neither a memory watch nor surface dirty tracking (x86.h recomp_watch, recomp_dirty): guest
# stores carry no check for them. The host MMU checks the generated code's guest accesses (x86.h RECOMP_MMU_GUEST,
# the fault handler in control.cpp).
target_compile_definitions(sacred_control PUBLIC SACRED_DRIVER_CHECKPOINTS=1 RECOMP_NO_STORE_HOOKS=1 RECOMP_MMU_GUEST=1)
target_link_libraries(sacred_control PUBLIC sacred_process sacred_cpu Threads::Threads)
target_compile_options(sacred_control PRIVATE -Wall -Wextra -Werror ${SACRED_RUNTIME_OPT} -g1)
# Native stand-ins for the hottest granny.dll functions, under the floating-point contract of the generated code.
add_library(sacred_granny STATIC runtime/granny/native.c runtime/granny/install.cpp)
target_link_libraries(sacred_granny PUBLIC sacred_control)
target_compile_options(sacred_granny PRIVATE -Wall -Wextra -Werror ${SACRED_RUNTIME_OPT} -g1
    -fno-fast-math -ffp-contract=off)
add_executable(boundary_tests tests/process/boundary_tests.c runtime/process/boundary.c)
target_include_directories(boundary_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    third_party/recomp-kit/runtime)
target_compile_options(boundary_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
add_test(NAME driver_checkpoint_mask COMMAND boundary_tests)
set(CONTROL_GENERATED "${CMAKE_BINARY_DIR}/generated-tests/control")
# The fixture image is assembled with GNU binutils for i386 (tests/control/generate.py); macOS has none.
find_program(SACRED_X86_AS x86_64-linux-gnu-as)
find_program(SACRED_X86_OBJCOPY x86_64-linux-gnu-objcopy)
if(SACRED_ASSETS AND EXISTS "${SACRED_EXPORT_DIR}/instructions.jsonl.gz" AND EXISTS "${SACRED_PYTHON}"
   AND SACRED_X86_AS AND SACRED_X86_OBJCOPY)
    add_custom_command(
        OUTPUT "${CONTROL_GENERATED}/translated.c" "${CONTROL_GENERATED}/fixtures.h"
        COMMAND ${CMAKE_COMMAND} -E env "SACRED_GEN_DIR=${CONTROL_GENERATED}" "SACRED_EXPORT_DIR=${SACRED_EXPORT_DIR}"
            "${SACRED_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/control/generate.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS tests/control/generate.py tests/control/fixtures.S tests/control/test_emission.py
            third_party/recomp-kit/tools/recomp/translate.py
            "${SACRED_EXPORT_DIR}/instructions.jsonl.gz"
        VERBATIM)
    add_executable(control_tests tests/control/control_tests.cpp
        "${CONTROL_GENERATED}/translated.c" "${CONTROL_GENERATED}/fixtures.h")
    target_include_directories(control_tests PRIVATE "${CONTROL_GENERATED}")
    target_link_libraries(control_tests PRIVATE sacred_control)
    target_compile_options(control_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
    add_test(NAME control_fixtures COMMAND control_tests "${SACRED_ASSETS}" "${CONTROL_GENERATED}")
    set_tests_properties(control_fixtures PROPERTIES TIMEOUT 30)
else()
    message(STATUS "control_fixtures skipped: needs SACRED_ASSETS, SACRED_EXPORT_DIR/instructions.jsonl.gz, "
        "the bootstrap venv and x86_64-linux-gnu-as/objcopy")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(OpenGL REQUIRED)
# The game's movies (WMV/WMA) are read with FFmpeg; a build without it skips every movie, as the game does when one
# cannot be opened.
option(SACRED_MOVIES "Play the game's movies (needs the pinned LGPL FFmpeg of tools/build_ffmpeg.sh)" ON)
if(SACRED_MOVIES)
    if(DEFINED ENV{SACRED_FFMPEG_PREFIX})
        set(_sacred_ffmpeg_default "$ENV{SACRED_FFMPEG_PREFIX}")
    else()
        set(_sacred_ffmpeg_default "${PROJECT_SOURCE_DIR}/build/ffmpeg/prefix")
    endif()
    set(SACRED_FFMPEG_PREFIX "${_sacred_ffmpeg_default}" CACHE PATH
        "Pinned minimal LGPL FFmpeg built by tools/build_ffmpeg.sh; the system FFmpeg (pkg-config) is used when absent")
    if(EXISTS "${SACRED_FFMPEG_PREFIX}/lib/pkgconfig/libavcodec.pc")
        set(ENV{PKG_CONFIG_PATH} "${SACRED_FFMPEG_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
        if(NOT SACRED_RELOCATABLE)
            set(CMAKE_BUILD_RPATH "${SACRED_FFMPEG_PREFIX}/lib")
            if(APPLE)
                list(APPEND CMAKE_BUILD_RPATH "${SACRED_MACOS_DEPS}/lib")
            endif()
        endif()
    elseif(SACRED_ALLOW_SYSTEM_FFMPEG)
        message(WARNING "Using the distribution FFmpeg: it may be GPL-configured. "
            "Do not distribute binaries built this way.")
    else()
        message(FATAL_ERROR "The pinned LGPL FFmpeg prefix ${SACRED_FFMPEG_PREFIX} is missing. "
            "Run tools/build_ffmpeg.sh, or pass "
            "-DSACRED_FFMPEG_PREFIX=<prefix>, or -DSACRED_ALLOW_SYSTEM_FFMPEG=ON for a local-only build.")
    endif()
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)
    add_library(sacred_movie STATIC platform/movie_decoder.cpp)
    target_include_directories(sacred_movie PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(sacred_movie PUBLIC PkgConfig::FFMPEG)
    target_compile_options(sacred_movie PRIVATE -Wall -Wextra -Werror -O1 -g1)
    add_executable(movie_decode_tests tests/movie/decode_tests.cpp)
    target_link_libraries(movie_decode_tests PRIVATE sacred_movie)
    target_compile_options(movie_decode_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
    if(SACRED_ASSETS)
        add_test(NAME movie_decode COMMAND movie_decode_tests "${SACRED_ASSETS}")
        set_tests_properties(movie_decode PROPERTIES TIMEOUT 300)
    endif()
else()
    add_library(sacred_movie STATIC platform/movie_decoder_none.cpp)
    target_include_directories(sacred_movie PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(sacred_movie PRIVATE -Wall -Wextra -Werror -O1 -g1)
endif()
add_executable(movie_pacer_tests tests/movie/pacer_tests.cpp)
target_include_directories(movie_pacer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(movie_pacer_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
add_test(NAME movie_pacer COMMAND movie_pacer_tests)
if(DEFINED ENV{SACRED_FONT_PREFIX})
    set(_sacred_font_default "$ENV{SACRED_FONT_PREFIX}")
else()
    set(_sacred_font_default "${PROJECT_SOURCE_DIR}/build/font-deps/usr")
endif()
set(SACRED_FONT_PREFIX "${_sacred_font_default}" CACHE PATH "Optional private font development prefix")
set(_font_pkg_path "$ENV{PKG_CONFIG_PATH}")
set(_font_pkg_args "${PKG_CONFIG_ARGN}")
if(NOT EXISTS "${SACRED_LINUX_DEPS}/lib/pkgconfig/freetype2.pc" AND EXISTS
    "${SACRED_FONT_PREFIX}/include/freetype2/ft2build.h" )
    set(ENV{PKG_CONFIG_PATH} "${SACRED_FONT_PREFIX}/lib/${CMAKE_LIBRARY_ARCHITECTURE}/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    list(APPEND PKG_CONFIG_ARGN
        "--define-variable=prefix=${SACRED_FONT_PREFIX}"
        "--define-variable=includedir=${SACRED_FONT_PREFIX}/include"
        "--define-variable=libdir=${SACRED_FONT_PREFIX}/lib/${CMAKE_LIBRARY_ARCHITECTURE}")
endif()
pkg_check_modules(FONTS REQUIRED IMPORTED_TARGET freetype2)
set(ENV{PKG_CONFIG_PATH} "${_font_pkg_path}")
set(PKG_CONFIG_ARGN "${_font_pkg_args}")
# GL tests: Linux renders offscreen through Mesa llvmpipe (EGL). macOS has no EGL, so SDL's offscreen driver
# cannot create a GL context there; the tests use hidden Cocoa windows on the native OpenGL 4.1 core profile.
if(APPLE)
    set(SACRED_GL_TEST_ENV "SDL_VIDEODRIVER=cocoa")
else()
    set(SACRED_GL_TEST_ENV
        "SDL_VIDEODRIVER=offscreen;MESA_SHADER_CACHE_DISABLE=true;LIBGL_ALWAYS_SOFTWARE=1;GALLIUM_DRIVER=llvmpipe")
endif()
add_library(sacred_window STATIC platform/window_host.cpp platform/renderer_gl.cpp platform/font.cpp
    platform/font_match.cpp
    platform/fvf.cpp)
target_include_directories(sacred_window PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sacred_window PUBLIC PkgConfig::SDL2 OpenGL::GL PRIVATE PkgConfig::FONTS)
target_compile_options(sacred_window PRIVATE -Wall -Wextra -Werror ${SACRED_RUNTIME_OPT} -g1)
# Our XML reader and writer (platform/xml_native.cpp) against bytes and trees libxml2 produced.
add_executable(xml_native_tests tests/xml/xml_native_tests.cpp platform/xml_native.cpp)
target_include_directories(xml_native_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(xml_native_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
add_test(NAME xml_native COMMAND xml_native_tests)
add_executable(font_match_tests tests/menu/font_match_tests.cpp)
target_link_libraries(font_match_tests PRIVATE sacred_window PkgConfig::FONTS)
target_compile_options(font_match_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
add_test(NAME menu_font_match COMMAND font_match_tests)
add_executable(host_tests tests/menu/host_tests.cpp)
target_link_libraries(host_tests PRIVATE sacred_window)
target_compile_options(host_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
add_test(NAME menu_host_queue COMMAND host_tests)
set_tests_properties(menu_host_queue PROPERTIES TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
add_executable(cursor_tests tests/menu/cursor_tests.cpp)
target_link_libraries(cursor_tests PRIVATE sacred_window)
target_compile_options(cursor_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
add_test(NAME menu_virtual_cursor COMMAND cursor_tests)
set_tests_properties(menu_virtual_cursor PROPERTIES TIMEOUT 60
    ENVIRONMENT "${SACRED_GL_TEST_ENV}")
add_library(sacred_startup STATIC platform/system_info.cpp platform/calendar.cpp platform/file_metadata.cpp
    runtime/win32/startup.cpp runtime/win32/threads.cpp runtime/win32/strings.cpp runtime/win32/locale.cpp
    runtime/win32/system.cpp runtime/win32/input.cpp runtime/win32/virtual.cpp runtime/win32/menu.cpp
    runtime/win32/gdi.cpp runtime/win32/crt.cpp runtime/win32/registry.cpp runtime/win32/winsock.cpp
    platform/net_socket.cpp
    runtime/ddraw/ddraw.cpp runtime/ddraw/device.cpp runtime/ddraw/surfaces.cpp runtime/ddraw/surface_pixels.cpp
    runtime/ddraw/texture_pack.cpp runtime/ddraw/texture_pak.cpp runtime/ddraw/texture_upscale.cpp
    runtime/ddraw/menu_backdrops.cpp runtime/ddraw/ground_mesh.cpp runtime/ddraw/gpu_skin.cpp
    runtime/ddraw/vertices.cpp runtime/ole/com.cpp runtime/ole/movie_playback.cpp runtime/miles/miles.cpp
    runtime/miles/mixer.cpp runtime/miles/wav.cpp
    platform/xml_native.cpp runtime/xml/xml.cpp)
target_include_directories(sacred_startup SYSTEM PRIVATE third_party/minimp3 third_party/dr_libs)
target_link_libraries(sacred_startup PUBLIC sacred_window sacred_movie)
target_link_libraries(sacred_startup PUBLIC sacred_control)
target_compile_options(sacred_startup PRIVATE -Wall -Wextra -Werror ${SACRED_RUNTIME_OPT} -g1)

add_executable(menu_tests tests/menu/menu_tests.cpp)
target_link_libraries(menu_tests PRIVATE sacred_startup)
target_compile_options(menu_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
if(SACRED_ASSETS)
    add_test(NAME menu_boundary COMMAND menu_tests "${SACRED_ASSETS}")
    set_tests_properties(menu_boundary PROPERTIES TIMEOUT 30)
endif()
add_executable(window_tests tests/menu/window_tests.cpp)
target_link_libraries(window_tests PRIVATE sacred_startup)
target_compile_options(window_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
if(SACRED_ASSETS)
    add_test(NAME menu_window_callbacks COMMAND window_tests "${SACRED_ASSETS}")
    set_tests_properties(menu_window_callbacks PROPERTIES TIMEOUT 30
        ENVIRONMENT "${SACRED_GL_TEST_ENV}")
endif()
add_executable(render_tests tests/menu/render_tests.cpp)
target_link_libraries(render_tests PRIVATE sacred_window)
target_compile_options(render_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
add_test(NAME menu_render_backend COMMAND render_tests)
set_tests_properties(menu_render_backend PROPERTIES TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    ENVIRONMENT "${SACRED_GL_TEST_ENV}")
# Offline texture index / export for texture packs (tools/textures): the shim's texture hash over the texture paks.
add_executable(texture_index tools/textures/texture_index.cpp runtime/ddraw/texture_pack.cpp
    runtime/ddraw/texture_pak.cpp runtime/ddraw/texture_upscale.cpp
    runtime/ddraw/surface_pixels.cpp)
target_link_libraries(texture_index PRIVATE sacred_process)
target_compile_options(texture_index PRIVATE -Wall -Wextra -Werror -O2 -g1)
# The engine's texture upscaler (TEXTURE_UPSCALE) against the rules of tools/textures/texture_pack_build.py; with
# SOURCE and EXPECTED directories it compares byte for byte with the script's output.
add_executable(texture_upscale_tests tests/menu/texture_upscale_tests.cpp runtime/ddraw/texture_pack.cpp
    runtime/ddraw/texture_pak.cpp runtime/ddraw/texture_upscale.cpp
    runtime/ddraw/surface_pixels.cpp)
target_link_libraries(texture_upscale_tests PRIVATE sacred_process)
target_compile_options(texture_upscale_tests PRIVATE -Wall -Wextra -Werror -O2 -g1)
add_test(NAME menu_texture_upscale COMMAND texture_upscale_tests)
if(SACRED_ASSETS)
    add_test(NAME menu_texture_upscale_paks COMMAND texture_upscale_tests --game "${SACRED_ASSETS}")
endif()
add_executable(surface_tests tests/menu/surface_tests.cpp)
target_link_libraries(surface_tests PRIVATE sacred_startup)
target_compile_options(surface_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
add_test(NAME menu_surface_pixels COMMAND surface_tests)
add_executable(vertex_tests tests/menu/vertex_tests.cpp)
target_link_libraries(vertex_tests PRIVATE sacred_startup)
target_compile_options(vertex_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
add_test(NAME menu_vertex_decode COMMAND vertex_tests)
# Split menu pictures (data/menu-backgrounds): the pairs load and put the game's screen back in their middle.
add_executable(menu_split_tests tests/menu/menu_split_tests.cpp)
target_link_libraries(menu_split_tests PRIVATE sacred_startup)
target_compile_options(menu_split_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
if(SACRED_ASSETS)
    add_test(NAME menu_split_backgrounds COMMAND menu_split_tests "${SACRED_ASSETS}"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/menu-backgrounds")
endif()
add_executable(graphics_tests tests/menu/graphics_tests.cpp)
target_link_libraries(graphics_tests PRIVATE sacred_startup)
target_compile_options(graphics_tests PRIVATE -Wall -Wextra -Werror -O1 -g1)
if(SACRED_ASSETS)
    add_test(NAME menu_graphics_com COMMAND graphics_tests "${SACRED_ASSETS}")
    set_tests_properties(menu_graphics_com PROPERTIES TIMEOUT 30
        ENVIRONMENT "${SACRED_GL_TEST_ENV};SDL_AUDIODRIVER=dummy")
endif()

if(SACRED_BUILD_GAME)
    if(NOT SACRED_STRICT)
        message(FATAL_ERROR "The game requires SACRED_STRICT=ON")
    endif()
    if(NOT EXISTS "${SACRED_GENERATED_DIR}/sources.cmake" AND NOT SACRED_GAME_MODULE)
        message(FATAL_ERROR "Generated game shards are missing in ${SACRED_GENERATED_DIR}. "
            "Run tools/bootstrap.sh (see README).")
    endif()
    if(EXISTS "${SACRED_GENERATED_DIR}/sources.cmake")
        include("${SACRED_GENERATED_DIR}/sources.cmake")
    endif()
    if(SACRED_GAME_MODULE AND NOT SACRED_GENERATED)
        # The runtime alone (distribution): no game files on this machine, the launcher builds the module later.
        message(STATUS "No generated shards: building the runtime without a game module")
    elseif(SACRED_GAME_MODULE)
        # Same compile contract as the launcher's build (tools/launcher/build_game.py): position independent, only
        # sacred_game_module exported, the runtime's helpers resolved against the executable at load time.
        add_library(sacred_generated MODULE ${SACRED_GENERATED} runtime/sacred/native.c)
        set_target_properties(sacred_generated PROPERTIES OUTPUT_NAME sacred_game C_VISIBILITY_PRESET hidden
            POSITION_INDEPENDENT_CODE ON)
        target_include_directories(sacred_generated PRIVATE
            $<TARGET_PROPERTY:sacred_control,INTERFACE_INCLUDE_DIRECTORIES>)
        target_compile_definitions(sacred_generated PRIVATE
            $<TARGET_PROPERTY:sacred_control,INTERFACE_COMPILE_DEFINITIONS>)
        if(APPLE)
            target_link_options(sacred_generated PRIVATE -undefined dynamic_lookup)
        endif()
    else()
        add_library(sacred_generated STATIC ${SACRED_GENERATED})
        target_link_libraries(sacred_generated PUBLIC sacred_control)
    endif()
    if(TARGET sacred_generated)
        target_compile_options(sacred_generated PRIVATE ${SACRED_GENERATED_OPT} -g1 -fno-fast-math -ffp-contract=off)
    endif()
    # The AArch64 machine outliner turned the region functions (external since the direct calls) into hundreds of
    # thousands of shared fragments, a branch-and-link pair on most memory accesses: +40% game-thread time.
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-mno-outline SACRED_HAS_NO_OUTLINE)
    if(SACRED_HAS_NO_OUTLINE AND TARGET sacred_generated)
        target_compile_options(sacred_generated PRIVATE -mno-outline)
    endif()
    add_executable(sacred-native runtime/modules/main.cpp runtime/modules/profiler.cpp runtime/modules/machine.cpp
        runtime/modules/resolution.cpp
        runtime/modules/game_module.cpp)
    if(SACRED_QA)
        target_sources(sacred-native PRIVATE runtime/modules/qa_script.cpp runtime/modules/oracle.cpp
            runtime/modules/call_trace.cpp)
    endif()
    if(SACRED_GAME_MODULE)
        # Every helper the generated code may call must be in the executable and visible to the module.
        target_compile_definitions(sacred-native PRIVATE SACRED_GAME_MODULE_DYNAMIC=1)
        set_target_properties(sacred-native PROPERTIES ENABLE_EXPORTS ON)
        target_link_libraries(sacred-native PRIVATE
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,sacred_control,sacred_cpu,sacred_process>"
            sacred_startup sacred_granny ${CMAKE_DL_LIBS})
        if(TARGET sacred_generated)
            add_dependencies(sacred-native sacred_generated)
        endif()
    else()
        target_link_libraries(sacred-native PRIVATE sacred_generated sacred_startup sacred_granny)
    endif()
    target_compile_options(sacred-native PRIVATE -Wall -Wextra -Werror -O1 -g1)
    if(SACRED_RELOCATABLE)
        # Distribution build (tools/dist/make_bundle.sh): no absolute build paths in the binary.
        if(APPLE)
            set_target_properties(sacred-native PROPERTIES BUILD_WITH_INSTALL_RPATH ON
                INSTALL_RPATH "@executable_path/lib")
        else()
            set_target_properties(sacred-native PROPERTIES BUILD_WITH_INSTALL_RPATH ON
                INSTALL_RPATH "\$ORIGIN/lib")
        endif()
    endif()
endif()

# The window a player starts (runtime/launcher/launcher.cpp): installation, options, the first-run build of the game
# module, then the game. It holds no game code and needs no generated shards.
add_executable(sacred-launcher runtime/launcher/launcher.cpp runtime/launcher/settings.cpp)
target_link_libraries(sacred-launcher PRIVATE sacred_window sacred_process)
target_compile_options(sacred-launcher PRIVATE -Wall -Wextra -Werror ${SACRED_RUNTIME_OPT})
if(SACRED_RELOCATABLE)
    if(APPLE)
        set_target_properties(sacred-launcher PROPERTIES BUILD_WITH_INSTALL_RPATH ON
            INSTALL_RPATH "@executable_path/lib")
    else()
        set_target_properties(sacred-launcher PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "\$ORIGIN/lib")
    endif()
endif()

add_custom_target(bundle
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/dist/make_bundle.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" USES_TERMINAL VERBATIM)

add_custom_target(check-portability
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_portability.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" USES_TERMINAL VERBATIM)
