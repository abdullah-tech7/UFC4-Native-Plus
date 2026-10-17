#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
if [[ -z ${CC:-} ]]; then
    CC=$(command -v cc || command -v gcc || true)
    if [[ -z $CC ]]; then
        for candidate in /nix/store/*-gcc-wrapper-*/bin/gcc; do
            if [[ -x $candidate ]]; then CC=$candidate; break; fi
        done
    fi
fi
if [[ -z ${CC:-} ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3). On NixOS the
# environment is provided by shell.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${BB_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
        exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
    fi
    echo 'Need pkg-config with vulkan and sdl3, cmake and ninja (see shell.nix).' >&2; exit 1
fi
# SacredFire: header-only libraries the host lacks (magic_enum, VulkanMemoryAllocator, xbyak) come
# from tools/deps.sh into out/deps/prefix; CMake finds them through CMAKE_PREFIX_PATH.
if [[ ! -f out/deps/prefix/include/vk_mem_alloc.h ]]; then bash tools/deps.sh; fi
export CMAKE_PREFIX_PATH="$PWD/out/deps/prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libbbgpu.so.
# BB_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. BB_LTO=OFF disables link-time optimization.
pgo=${BB_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
# Submodules (git clone --recursive, or: git submodule update --init) and this port's changes
# to FSR-Vulkan (gpu/patches/fsr-vulkan), applied to its working tree once.
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
# bbport 0.51 (issue 124): a checkout updated from an earlier version keeps that version's patch
# applied, and the new one applies neither way ("patch does not apply"): the submodule's tree
# holds nothing but these patches, so it is reset and they are applied again.
fsr_patches_apply() {
    for patch in gpu/patches/fsr-vulkan/*.patch; do
        if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
            git -C gpu/third_party/fsr-vulkan apply "$@" "$PWD/$patch" || return 1
        fi
    done
}
if ! fsr_patches_apply --check 2>/dev/null; then
    echo 'FSR-Vulkan: an earlier version of the port changed it; resetting it and patching again'
    git -C gpu/third_party/fsr-vulkan checkout -- .
fi
fsr_patches_apply
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" \
    -DBB_LTO="${BB_LTO:-ON}" -DBB_PGO_DIR="$PWD/pgo" >/dev/null
echo "GPU library: PGO $pgo, LTO ${BB_LTO:-ON}"
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
if ! ninja -C out/gpu bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic)
# The runtime and its Linux host services (src/platform; the Windows ones are platform_win.c).
runtime=(src/runtime*.c src/platform/platform_posix.c)
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" src/vulkan_smoke.c out/libatrac9.a -lm -lZydis "${gpu[@]}" "${libraries[@]}" -o out/sacredfire
echo "Built $PWD/out/sacredfire"
# out/VERSION: the source revision this build came from (the nearest v* tag, else the commit;
# "-dirty" when tracked files differ from it). run.sh prints it, the launcher shows it and the
# package carries it, so a published binary can be matched to its source. The FSR-Vulkan patch
# build.sh applies inside its submodule does not count as a change.
version=$(git describe --tags --match 'ufc3-v*' --always 2>/dev/null || echo unknown)
git update-index -q --refresh 2>/dev/null || true
if ! git diff-index --quiet --ignore-submodules=dirty HEAD -- 2>/dev/null; then version="$version-dirty"; fi
printf '%s\n' "$version" > out/VERSION
echo "Version: $version (out/VERSION)"
# GPU check for run.sh (live_resolution=auto) and the launcher's gamepad list (--gamepads).
"$CC" -std=c11 -O2 -Wall -Wextra -Werror "${includes[@]}" tools/gpu_capabilities.c "${libraries[@]}" -o out/bb-gpu-capabilities
if [[ ${1:-} == --test ]]; then
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c src/platform/platform_posix.c "${libraries[@]}" -o out/pad-test
    out/pad-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm -lZydis "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_file_mods.c src/platform/platform_posix.c -o out/file-mods-test
    out/file-mods-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm -lZydis "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    out/sema-test
    # ufc3 online (UFC-25): the deterministic rcp/rsqrt patch against its rule (--all: every pattern).
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_deterministic_fp.c src/runtime_fp.c src/platform/platform_posix.c -lZydis -lm -o out/deterministic-fp-test
    out/deterministic-fp-test
    "$CC" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    out/content-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_eventflag.c "${runtime[@]}" out/libatrac9.a -lm -lZydis "${gpu[@]}" "${libraries[@]}" -o out/eventflag-test
    out/eventflag-test
    # ufc3 online: the PS4 network ABI (without the local server) and the Blaze/EASO server.
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_online_net.c src/runtime_net.c -o out/online-net-test
    out/online-net-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc -Wl,--gc-sections -ffunction-sections tests/test_time_conversion.c -o out/time-conversion-test
    out/time-conversion-test
    python3 -m unittest discover -s tests -p test_online_server.py
fi
