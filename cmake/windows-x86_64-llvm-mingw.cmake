# SacredFire: CMake toolchain for the Windows x86-64 build made on a Linux host with llvm-mingw
# (docs/sacredfire/13-windows-port.md). Set by scripts/build-windows.sh and build-deps.sh:
#   LLVM_MINGW_ROOT      the unpacked llvm-mingw-<date>-ucrt-ubuntu-*-x86_64 directory
#   WINDOWS_TARGET_ROOT  the sysroot the target libraries are installed into (out/windows/sysroot)
# Libraries, headers and packages come from WINDOWS_TARGET_ROOT and the toolchain's own
# x86_64-w64-mingw32 tree only; programs (glslang, python, cmake helpers) from the Linux host.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_SYSTEM_VERSION 10.0)

if (NOT DEFINED LLVM_MINGW_ROOT)
    set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
endif()
if (NOT DEFINED WINDOWS_TARGET_ROOT)
    set(WINDOWS_TARGET_ROOT "$ENV{WINDOWS_TARGET_ROOT}")
endif()
if (NOT IS_DIRECTORY "${LLVM_MINGW_ROOT}/bin")
    message(FATAL_ERROR "LLVM_MINGW_ROOT (${LLVM_MINGW_ROOT}) has no bin/: run scripts/windows/fetch-deps.sh")
endif()
set(BB_TRIPLE x86_64-w64-mingw32)
foreach(tool clang clang++ windres ar ranlib strip dlltool)
    if (NOT EXISTS "${LLVM_MINGW_ROOT}/bin/${BB_TRIPLE}-${tool}")
        message(FATAL_ERROR "missing ${LLVM_MINGW_ROOT}/bin/${BB_TRIPLE}-${tool}")
    endif()
endforeach()
set(CMAKE_C_COMPILER   "${LLVM_MINGW_ROOT}/bin/${BB_TRIPLE}-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/${BB_TRIPLE}-clang++")
set(CMAKE_RC_COMPILER  "${LLVM_MINGW_ROOT}/bin/${BB_TRIPLE}-windres")
set(CMAKE_AR           "${LLVM_MINGW_ROOT}/bin/${BB_TRIPLE}-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB       "${LLVM_MINGW_ROOT}/bin/${BB_TRIPLE}-ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP        "${LLVM_MINGW_ROOT}/bin/${BB_TRIPLE}-strip" CACHE FILEPATH "")
# The compiler driver knows its own sysroot (the toolchain's x86_64-w64-mingw32 tree); it is not
# overridden. The target root holds what this project builds for Windows.
set(CMAKE_FIND_ROOT_PATH "${WINDOWS_TARGET_ROOT};${LLVM_MINGW_ROOT}/${BB_TRIPLE}")
set(CMAKE_PREFIX_PATH "${WINDOWS_TARGET_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# pkg-config: the sysroot's .pc files only, no host paths. pkg-config itself is a host program.
set(ENV{PKG_CONFIG_LIBDIR} "${WINDOWS_TARGET_ROOT}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
# Libraries some exported CMake packages name without a path (Zydis: -lZycore) come from the sysroot.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${WINDOWS_TARGET_ROOT}/lib")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L${WINDOWS_TARGET_ROOT}/lib")
# Cross builds cannot run what they compile: try_run() must not be reached without a declared result.
set(CMAKE_CROSSCOMPILING_EMULATOR "")
set(BB_WINDOWS_CROSS ON)
