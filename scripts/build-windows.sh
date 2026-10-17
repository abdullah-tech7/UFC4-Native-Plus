#!/usr/bin/env bash
# SacredFire Windows x86-64 build, made on a Linux host (docs/sacredfire/13-windows-port.md).
# Stages (each idempotent, run in order unless one is named):
#   --fetch    scripts/windows/fetch-deps.sh: the pinned toolchain, sources and runtime inputs
#   --deps     scripts/windows/build-deps.sh: the target libraries into out/windows/sysroot
#   --runtime  cmake -S gpu -B out/windows/build with cmake/windows-x86_64-llvm-mingw.cmake, then
#              ninja: sacredfire-game.exe, SacredFire.exe, bb-gpu-capabilities.exe -> out/windows/bin
#   --tests    the Windows probes and runtime tests (ninja windows-tests) -> out/windows/tests
# Then scripts/package-windows.sh makes the release archive. Options: BB_JOBS, BB_LTO (ON; ufc3: the GPU thread is CPU-bound on laptops),
# BB_WINDOWS_ROOT (out/windows). Nothing here needs Windows or Wine; nothing is downloaded
# outside --fetch.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
root=${BB_WINDOWS_ROOT:-$PWD/out/windows}
stage=${1:-all}
toolchain_tar=$(sed -n 's/^llvm-mingw archive [^ ]*\/\([^ ]*\) .*/\1/p' packaging/windows/deps.lock)
export LLVM_MINGW_ROOT=${LLVM_MINGW_ROOT:-$root/toolchain/${toolchain_tar%.tar.xz}}
export WINDOWS_TARGET_ROOT=$root/sysroot
if [[ $stage == all || $stage == --fetch ]]; then bash scripts/windows/fetch-deps.sh; fi
if [[ $stage == all || $stage == --deps ]]; then bash scripts/windows/build-deps.sh; fi
if [[ $stage == all || $stage == --runtime || $stage == --tests ]]; then
    for tool in cmake ninja glslang python3; do
        command -v $tool >/dev/null || { echo "build-windows: need $tool on the host (host shaders are compiled with glslang)" >&2; exit 1; }
    done
    [[ -x $LLVM_MINGW_ROOT/bin/x86_64-w64-mingw32-clang ]] || { echo "build-windows: no toolchain at $LLVM_MINGW_ROOT (run with --fetch)" >&2; exit 1; }
    [[ -f $WINDOWS_TARGET_ROOT/lib/pkgconfig/sdl3.pc ]] || { echo "build-windows: no sysroot at $WINDOWS_TARGET_ROOT (run with --deps)" >&2; exit 1; }
    # Submodules and the FSR-Vulkan patch, as build.sh does for Linux.
    if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then git submodule update --init --recursive; fi
    for patch in gpu/patches/fsr-vulkan/*.patch; do
        if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
            git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
        fi
    done
    cmake -S gpu -B "$root/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/windows-x86_64-llvm-mingw.cmake" \
        -DLLVM_MINGW_ROOT="$LLVM_MINGW_ROOT" -DWINDOWS_TARGET_ROOT="$WINDOWS_TARGET_ROOT" \
        -DBB_PGO=off -DBB_LTO="${BB_LTO:-ON}" -DBB_AVPLAYER=OFF > "$root/configure.log" 2>&1 || { cat "$root/configure.log" >&2; exit 1; }
    echo "build-windows: configured (LTO ${BB_LTO:-ON}, no AvPlayer/FFmpeg)"
    if [[ $stage == --tests ]]; then
        ninja -C "$root/build" -j "${BB_JOBS:-$(nproc)}" windows-tests > "$root/tests-build.log" 2>&1 || { grep -v '^\[' "$root/tests-build.log" | tail -40 >&2; echo "build-windows: test build failed (log: $root/tests-build.log)" >&2; exit 1; }
        mkdir -p "$root/tests"
        for exe in abi_probe tls_probe memory_probe file_probe runtime-test sema-test eventflag-test content-test deterministic-fp-test; do cp "$root/build/$exe.exe" "$root/tests/"; done
        echo "build-windows: tests in $root/tests"
        exit 0
    fi
    if ! ninja -C "$root/build" -j "${BB_JOBS:-$(nproc)}" sacredfire-game sacredfire bb-gpu-capabilities > "$root/build.log" 2>&1; then
        grep -v '^\[' "$root/build.log" | tail -60 >&2
        echo "build-windows: build failed (full log: $root/build.log)" >&2; exit 1
    fi
    mkdir -p "$root/bin"
    cp "$root/build/sacredfire-game.exe" "$root/build/SacredFire.exe" "$root/build/bb-gpu-capabilities.exe" "$root/bin/"
    # Every executable must be a Windows x86-64 PE.
    for exe in "$root"/bin/*.exe; do
        "$LLVM_MINGW_ROOT/bin/llvm-readobj" --file-headers "$exe" | grep -q 'IMAGE_FILE_MACHINE_AMD64' || { echo "build-windows: $exe is not an AMD64 PE" >&2; exit 1; }
    done
    version=$(git describe --tags --match 'ufc3-v*' --always 2>/dev/null || echo unknown)
    git update-index -q --refresh 2>/dev/null || true
    git diff-index --quiet --ignore-submodules=dirty HEAD -- 2>/dev/null || version="$version-dirty"
    printf '%s\n' "$version" > "$root/bin/VERSION"
    echo "build-windows: $root/bin/{sacredfire-game.exe,SacredFire.exe,bb-gpu-capabilities.exe} ($version)"
fi
