#!/usr/bin/env bash
# SacredFire Windows cross-build, stage 2: the target libraries and headers of
# packaging/windows/deps.lock, built with the pinned llvm-mingw toolchain into the Windows sysroot
# (out/windows/sysroot: include/, lib/, lib/pkgconfig, lib/cmake). Every package is configured
# through cmake/windows-x86_64-llvm-mingw.cmake, so headers, libraries and packages are searched in
# the sysroot and the toolchain only. Idempotent: a package whose marker file exists is skipped
# (delete out/windows/sysroot to rebuild everything). Also makes the toolchain's missing vulkan-1
# import library from the Vulkan-Loader export list (llvm-dlltool).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=${BB_WINDOWS_ROOT:-$PWD/out/windows}
sources=$root/deps/src
sysroot=$root/sysroot
builds=$root/deps/build
toolchain_tar=$(sed -n 's/^llvm-mingw archive [^ ]*\/\([^ ]*\) .*/\1/p' packaging/windows/deps.lock)
export LLVM_MINGW_ROOT=${LLVM_MINGW_ROOT:-$root/toolchain/${toolchain_tar%.tar.xz}}
export WINDOWS_TARGET_ROOT=$sysroot
toolchain_file=$PWD/cmake/windows-x86_64-llvm-mingw.cmake
if [[ ! -x $LLVM_MINGW_ROOT/bin/x86_64-w64-mingw32-clang ]]; then
    mkdir -p "$root/toolchain"
    echo "deps: unpacking $toolchain_tar"
    tar -C "$root/toolchain" -xJf "$root/downloads/$toolchain_tar"
fi
[[ -x $LLVM_MINGW_ROOT/bin/x86_64-w64-mingw32-clang ]] || { echo "deps: no toolchain at $LLVM_MINGW_ROOT (run scripts/windows/fetch-deps.sh)" >&2; exit 1; }
for tool in cmake ninja python3; do command -v $tool >/dev/null || { echo "deps: need $tool on the host" >&2; exit 1; }; done
mkdir -p "$sysroot/include" "$sysroot/lib/pkgconfig" "$builds"
jobs=${BB_JOBS:-$(nproc)}
log=$root/deps-build.log
: > "$log"
# cmake_install NAME MARKER [cmake options...]: configure, build and install one package.
cmake_install() {
    local name=$1 marker=$2; shift 2
    if [[ -e $sysroot/$marker ]]; then echo "deps: $name present ($marker)"; return; fi
    echo "deps: building $name"
    local src=$sources/$name/${CMAKE_SUBDIR:-} build=$builds/$name${CMAKE_SUBDIR:+-${CMAKE_SUBDIR//\//-}}
    if ! { cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_TOOLCHAIN_FILE="$toolchain_file" -DCMAKE_INSTALL_PREFIX="$sysroot" \
            -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTING=OFF "$@" &&
           cmake --build "$build" --parallel "$jobs" && cmake --install "$build"; } >> "$log" 2>&1; then
        tail -40 "$log" >&2; echo "deps: $name failed (log: $log)" >&2; exit 1
    fi
    [[ -e $sysroot/$marker ]] || { echo "deps: $name installed nothing at $marker" >&2; exit 1; }
    echo "deps: $name installed"
}
# Vulkan: headers from Vulkan-Headers; vulkan-1.dll comes with the player's graphics driver, the
# import library is generated here from the loader's export list (mingw-w64 a3d9399 has none).
cmake_install Vulkan-Headers include/vulkan/vulkan.h
if [[ ! -f $sysroot/lib/libvulkan-1.a ]]; then
    "$LLVM_MINGW_ROOT/bin/x86_64-w64-mingw32-dlltool" -m i386:x86-64 -d "$root/downloads/vulkan-1.def" -D vulkan-1.dll -l "$sysroot/lib/libvulkan-1.a"
    echo "deps: libvulkan-1.a generated from vulkan-1.def"
fi
cmake_install SDL lib/pkgconfig/sdl3.pc -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF
cmake_install fmt lib/cmake/fmt/fmt-config.cmake -DFMT_TEST=OFF -DFMT_DOC=OFF -DBUILD_SHARED_LIBS=OFF
CMAKE_SUBDIR=build/cmake cmake_install xxHash lib/pkgconfig/libxxhash.pc -DBUILD_SHARED_LIBS=OFF -DXXHASH_BUILD_XXHSUM=OFF
# Zydis decodes the guest's TLS access sites before they are patched (platform_win.c). Its Zycore
# dependency (a submodule of the pinned checkout) is installed first so Zydis links the sysroot copy.
CMAKE_SUBDIR=dependencies/zycore cmake_install zydis lib/cmake/zycore/zycore-config.cmake -DZYAN_DEV_MODE=OFF -DBUILD_SHARED_LIBS=OFF -DZYCORE_BUILD_EXAMPLES=OFF -DZYCORE_BUILD_TESTS=OFF
cmake_install zydis lib/cmake/zydis/zydis-config.cmake -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_DOXYGEN=OFF -DZYAN_SYSTEM_ZYCORE=ON -DBUILD_SHARED_LIBS=OFF "-DCMAKE_C_FLAGS=-I$sysroot/include"
cmake_install miniz lib/cmake/miniz/minizConfig.cmake -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_FUZZERS=OFF -DBUILD_TESTS=OFF
cmake_install VulkanMemoryAllocator include/vk_mem_alloc.h
cmake_install magic_enum include/magic_enum/magic_enum.hpp -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF -DMAGIC_ENUM_OPT_INSTALL=ON
cmake_install xbyak include/xbyak/xbyak.h
cmake_install robin-map share/cmake/tsl-robin-map/tsl-robin-mapConfig.cmake
# Boost: the header-only libraries the GPU code includes (container, icl, intrusive, pool,
# preprocessor) with their dependencies, from the CMake-enabled release archive.
if [[ ! -f $sysroot/include/boost/version.hpp ]]; then
    boost_tar=$(sed -n 's/^boost archive [^ ]*\/\([^ ]*\) .*/\1/p' packaging/windows/deps.lock)
    boost_dir=$sources/${boost_tar%-cmake.tar.xz} # the archive unpacks as boost-<version>/
    [[ -d $boost_dir ]] || { echo "deps: unpacking $boost_tar"; tar -C "$sources" -xJf "$root/downloads/$boost_tar"; }
    [[ -f $boost_dir/CMakeLists.txt ]] || { echo "deps: $boost_dir has no CMakeLists.txt" >&2; exit 1; }
    echo "deps: building boost headers"
    if ! { cmake -S "$boost_dir" -B "$builds/boost" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_TOOLCHAIN_FILE="$toolchain_file" -DCMAKE_INSTALL_PREFIX="$sysroot" -DCMAKE_INSTALL_LIBDIR=lib \
            -DBOOST_INCLUDE_LIBRARIES='container;icl;intrusive;pool;preprocessor;asio' -DBOOST_ENABLE_CMAKE=ON \
            -DBUILD_SHARED_LIBS=OFF -DBOOST_INSTALL_LAYOUT=system -DBOOST_SKIP_INSTALL_RULES=OFF &&
           cmake --build "$builds/boost" --parallel "$jobs" && cmake --install "$builds/boost"; } >> "$log" 2>&1; then
        tail -40 "$log" >&2; echo "deps: boost failed (log: $log)" >&2; exit 1
    fi
    echo "deps: boost installed"
fi
echo "deps: sysroot $sysroot ready"
