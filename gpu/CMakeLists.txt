# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
# Release archives: source paths in __FILE__ (log messages) relative to the repository, without
# the build machine's home directory.
get_filename_component(BB_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_compile_options("-ffile-prefix-map=${BB_REPO_ROOT}/=")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)
# sacredfire: libSceAvPlayer/libSceVideodec (FFmpeg) are Bloodborne's video path; CTR imports none
# of their functions (out/import_names.txt), so they are out of the build unless asked for.
option(BB_AVPLAYER "Build the AvPlayer/Videodec libraries (needs FFmpeg)" OFF)
# sacredfire: the Windows build (cmake/windows-x86_64-llvm-mingw.cmake) links the GPU library
# statically into the game executable and builds the executables here (docs/sacredfire/13-windows-port.md).

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(BB_LTO "Link-time optimization of bbgpu" ON)
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
if (WIN32)
    enable_language(RC)
    # No build-host paths in the binaries (__FILE__ in assertions and logs, debug info): the
    # archive must not name the machine it was built on, and two builds of one revision agree.
    add_compile_options("-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/..=." "-ffile-prefix-map=${WINDOWS_TARGET_ROOT}=sysroot" "-ffile-prefix-map=${LLVM_MINGW_ROOT}=toolchain")
endif()
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
find_package(magic_enum REQUIRED)
find_package(tsl-robin-map REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT WIN32)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
find_package(miniz REQUIRED)
find_package(Zydis REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)
if (WIN32)
    # sacredfire: the FidelityFX sources use MSVC's _countof and swprintf_s without including the
    # headers mingw-w64 declares them in (stdlib.h, wchar.h); every FSR target gets them first.
    get_property(ffx_targets DIRECTORY third_party/fsr-vulkan PROPERTY BUILDSYSTEM_TARGETS)
    foreach(ffx_target IN LISTS ffx_targets)
        get_target_property(ffx_type ${ffx_target} TYPE)
        if (ffx_type MATCHES "LIBRARY|EXECUTABLE" AND NOT ffx_type STREQUAL "INTERFACE_LIBRARY")
            target_compile_options(${ffx_target} PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bb_ffx_prelude.h")
        endif()
    endforeach()
endif()

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(depth_to_r16ui.comp depth_to_r16ui_comp)
add_host_shader(depth_to_r16ui.comp depth_to_r32ui_comp R32=1)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(fsr4_color.comp fsr4_decode_comp)
add_host_shader(fsr4_color.comp fsr4_encode_comp ENCODE=1)
add_host_shader(fsr4_reactive.comp fsr4_reactive_comp)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
set(AVPLAYER "")
if (BB_AVPLAYER)
    file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
    list(APPEND AVPLAYER shadps4/core/libraries/videodec/video_utils.cpp)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
endif()
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
# Windows: a DLL cannot leave the runtime_* symbols for the executable to resolve, so the
# library is static there and linked into sacredfire-game.exe below.
if (WIN32)
    set(BBGPU_KIND STATIC)
else()
    set(BBGPU_KIND SHARED)
endif()
add_library(bbgpu ${BBGPU_KIND} ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER})
if (NOT BB_AVPLAYER)
    target_compile_definitions(bbgpu PRIVATE BB_NO_AVPLAYER)
endif()
add_dependencies(bbgpu host_shaders)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
# Dear ImGui stays private to libbbgpu.so. The library is linked into bb-probe, so its exports are
# in the global symbol scope; a Vulkan layer carrying its own Dear ImGui without -Bsymbolic
# (vkShade 0.1) had its ImGui_ImplVulkan_Init bound to this copy, which called the Vulkan loader
# with the layer's down-chain physical device handle ("Invalid physicalDevice", abort).
set_source_files_properties(third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
target_include_directories(bbgpu PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
if (NOT WIN32)
    # Loaded with bb-probe at startup (DT_NEEDED, never dlopen): thread_local access without a
    # TLS descriptor call (_dl_tlsdesc_return was ~2% of the draw recording thread).
    target_compile_options(bbgpu PRIVATE -ftls-model=initial-exec)
    # Runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe.
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan)
target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 miniz::miniz Zydis::Zydis sirit)
if (BB_AVPLAYER)
    target_link_libraries(bbgpu PUBLIC PkgConfig::FFMPEG)
endif()
if (WIN32)
    # The POSIX names the bbport diagnostics use (shim/bb_win_compat.h) in every translation
    # unit; <windows.h> (through the Vulkan Win32 surface header) without min/max macros.
    target_compile_definitions(bbgpu PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000005)
    target_compile_options(bbgpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bb_win_compat.h)
    target_link_libraries(bbgpu PUBLIC Vulkan::Vulkan ws2_32 bcrypt ntdll synchronization winmm onecore)
else()
    target_link_libraries(bbgpu PUBLIC PkgConfig::X11)
    # shim/glibc_compat.cpp: acosf/sqrtf bound to their pre-2.43 versions (portable releases).
    target_link_options(bbgpu PRIVATE -Wl,--wrap=acosf -Wl,--wrap=sqrtf)
    set_source_files_properties(shim/glibc_compat.cpp PROPERTIES COMPILE_OPTIONS "-fno-lto")
endif()

# sacredfire (Windows): the game executable, the GPU check and the launcher, built here where
# CMake knows the static GPU library's dependencies (scripts/build-windows.sh). Linux builds them
# with build.sh as before.
if (WIN32)
    set(BB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/..)
    file(GLOB BB_RUNTIME CONFIGURE_DEPENDS ${BB_SRC}/src/runtime*.c)
    file(GLOB BB_ATRAC9 CONFIGURE_DEPENDS ${BB_SRC}/third_party/LibAtrac9/C/src/*.c)
    add_library(atrac9 STATIC ${BB_ATRAC9})
    target_compile_options(atrac9 PRIVATE -w)
    set_target_properties(atrac9 PROPERTIES C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION OFF)
    set(BB_C_WARNINGS -Wall -Wextra -Werror -Wno-unused-but-set-global)
    set(BB_WIN_DEFS _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000005 WIN32_LEAN_AND_MEAN NOMINMAX)
    add_executable(sacredfire-game ${BB_SRC}/src/probe.c ${BB_RUNTIME} ${BB_SRC}/src/platform/platform_win.c
        ${BB_SRC}/src/vulkan_smoke.c ${BB_SRC}/packaging/windows/sacredfire-game.rc)
    set_target_properties(sacredfire-game PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_compile_options(sacredfire-game PRIVATE $<$<COMPILE_LANGUAGE:C>:${BB_C_WARNINGS}>)
    target_compile_definitions(sacredfire-game PRIVATE ${BB_WIN_DEFS})
    target_include_directories(sacredfire-game PRIVATE ${BB_SRC} ${BB_SRC}/src)
    target_link_libraries(sacredfire-game PRIVATE bbgpu atrac9 Zydis::Zydis onecore bcrypt ws2_32 winmm -pthread)
    # The PS4 range (64 GiB - 1 TiB) must stay free of host allocations: no high-entropy ASLR
    # (image ASLR and the other mitigations stay on; docs/sacredfire/13-windows-port.md). The
    # guest main thread runs on the process stack: 16 MiB, committed (no stack probes).
    # Console subsystem (the log is stdout; SDL3's pkg-config flags would make it a GUI program).
    target_link_options(sacredfire-game PRIVATE -Wl,--disable-high-entropy-va -Wl,--subsystem,console "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
    add_executable(bb-gpu-capabilities ${BB_SRC}/tools/gpu_capabilities.c ${BB_SRC}/packaging/windows/sacredfire-game.rc)
    target_compile_options(bb-gpu-capabilities PRIVATE ${BB_C_WARNINGS})
    target_compile_definitions(bb-gpu-capabilities PRIVATE ${BB_WIN_DEFS})
    target_link_libraries(bb-gpu-capabilities PRIVATE Vulkan::Vulkan PkgConfig::SDL3)
    target_link_options(bb-gpu-capabilities PRIVATE -Wl,--subsystem,console)
    # SacredFire.exe: the launcher window (native Win32) and what run.sh does (game check,
    # preparation, patches, start) behind its Play button or --play.
    add_executable(sacredfire WIN32 ${BB_SRC}/packaging/windows/sacredfire_launcher.c ${BB_SRC}/packaging/windows/sacredfire-launcher.rc)
    set_target_properties(sacredfire PROPERTIES C_STANDARD 11 OUTPUT_NAME SacredFire)
    target_compile_options(sacredfire PRIVATE ${BB_C_WARNINGS} -municode) # wWinMain: UTF-16 arguments
    target_link_options(sacredfire PRIVATE -municode -mwindows)
    target_compile_definitions(sacredfire PRIVATE ${BB_WIN_DEFS})
    target_link_libraries(sacredfire PRIVATE comctl32 shlwapi ole32 shell32 uuid user32 gdi32)
    # Windows versions of the runtime tests and the ABI/TLS/memory probes (tests/windows): built
    # with `ninja windows-tests`, run on real Windows (Wine as a diagnostic).
    add_custom_target(windows-tests)
    foreach(probe abi_probe tls_probe memory_probe file_probe)
        add_executable(${probe} EXCLUDE_FROM_ALL ${BB_SRC}/tests/windows/${probe}.c ${BB_SRC}/src/platform/platform_win.c)
        set_target_properties(${probe} PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
        target_compile_options(${probe} PRIVATE ${BB_C_WARNINGS} -UNDEBUG)
        target_compile_definitions(${probe} PRIVATE ${BB_WIN_DEFS})
        target_include_directories(${probe} PRIVATE ${BB_SRC} ${BB_SRC}/src)
        target_link_libraries(${probe} PRIVATE Zydis::Zydis onecore bcrypt -pthread)
        target_link_options(${probe} PRIVATE -Wl,--disable-high-entropy-va)
        add_dependencies(windows-tests ${probe})
    endforeach()
    foreach(test runtime sema eventflag)
        add_executable(${test}-test EXCLUDE_FROM_ALL ${BB_SRC}/tests/test_${test}.c ${BB_RUNTIME} ${BB_SRC}/src/platform/platform_win.c)
        set_target_properties(${test}-test PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
        target_compile_options(${test}-test PRIVATE ${BB_C_WARNINGS} -UNDEBUG) # the tests are assert()s
        target_compile_definitions(${test}-test PRIVATE ${BB_WIN_DEFS})
        target_include_directories(${test}-test PRIVATE ${BB_SRC} ${BB_SRC}/src)
        target_link_libraries(${test}-test PRIVATE bbgpu atrac9 Zydis::Zydis onecore bcrypt ws2_32 -pthread)
        target_link_options(${test}-test PRIVATE -Wl,--disable-high-entropy-va)
        add_dependencies(windows-tests ${test}-test)
    endforeach()
    # AppContent alone, as build.sh links it on Linux.
    add_executable(content-test EXCLUDE_FROM_ALL ${BB_SRC}/tests/test_content.c ${BB_SRC}/src/runtime_content.c ${BB_SRC}/src/platform/platform_win.c)
    set_target_properties(content-test PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_compile_options(content-test PRIVATE ${BB_C_WARNINGS} -UNDEBUG)
    target_compile_definitions(content-test PRIVATE ${BB_WIN_DEFS})
    target_include_directories(content-test PRIVATE ${BB_SRC} ${BB_SRC}/src)
    target_link_libraries(content-test PRIVATE Zydis::Zydis onecore bcrypt -pthread)
    add_dependencies(windows-tests content-test)
    # ufc3 online (UFC-25): the deterministic rcp/rsqrt patch.
    add_executable(deterministic-fp-test EXCLUDE_FROM_ALL ${BB_SRC}/tests/test_deterministic_fp.c ${BB_SRC}/src/runtime_fp.c ${BB_SRC}/src/platform/platform_win.c)
    set_target_properties(deterministic-fp-test PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_compile_options(deterministic-fp-test PRIVATE ${BB_C_WARNINGS} -UNDEBUG)
    target_compile_definitions(deterministic-fp-test PRIVATE ${BB_WIN_DEFS})
    target_include_directories(deterministic-fp-test PRIVATE ${BB_SRC} ${BB_SRC}/src)
    target_link_libraries(deterministic-fp-test PRIVATE Zydis::Zydis onecore bcrypt -pthread)
    add_dependencies(windows-tests deterministic-fp-test)
endif()

# Explicit test targets; neither launches the game nor needs a Vulkan device.
add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)
add_executable(label-guard-test EXCLUDE_FROM_ALL ../tests/test_label_guard.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_compile_options(label-guard-test PRIVATE -UNDEBUG)
target_include_directories(label-guard-test PRIVATE .. shim shadps4)
target_link_libraries(label-guard-test PRIVATE bbgpu)
add_executable(write-window-merge-test EXCLUDE_FROM_ALL ../tests/test_write_window_merge.cpp)
target_compile_options(write-window-merge-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(write-window-merge-test PRIVATE ..)
add_executable(page-watch-perms-test EXCLUDE_FROM_ALL ../tests/test_page_watch_perms.cpp)
target_compile_options(page-watch-perms-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(page-watch-perms-test PRIVATE ..)
add_executable(settings-test EXCLUDE_FROM_ALL ../tests/test_settings.cpp shim/bbport_settings.cpp)
target_compile_options(settings-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(settings-test PRIVATE .. shim)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)

add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)
