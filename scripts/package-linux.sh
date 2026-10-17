#!/usr/bin/env bash
# UFC 3: dist/UFC3Native-linux-x86_64.tar.gz from the current Linux build (bash build.sh first).
# Portable to glibc 2.38+: the program's libraries (a SDL3 built here from the Windows deps'
# source, fmt, miniz, xxhash, Zydis, libstdc++, libgcc_s) go to bin/lib; glibc, Vulkan and the
# window system come from the system. Every ELF is checked for its newest glibc symbol version.
# A dirty or untagged checkout is refused unless BB_ALLOW_DIRTY=1 (as package-windows.sh).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
[[ -f out/sacredfire && -f out/gpu/libbbgpu.so && -f out/VERSION ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
built_from=$(cat out/VERSION)
checkout=$(git describe --tags --match 'ufc3-v*' --always 2>/dev/null || echo unknown)
git update-index -q --refresh 2>/dev/null || true
git diff-index --quiet --ignore-submodules=dirty HEAD -- 2>/dev/null || checkout="$checkout-dirty"
if [[ -z ${BB_ALLOW_DIRTY:-} ]]; then
    [[ $checkout != *-dirty ]] || { echo "package-linux: uncommitted changes ($checkout): commit and tag first" >&2; exit 1; }
    [[ $built_from == "$checkout" ]] || { echo "package-linux: out/ was built from $built_from, the checkout is at $checkout: run build.sh again" >&2; exit 1; }
fi
set_rpath() { if type -P patchelf >/dev/null; then patchelf --set-rpath "$@"; else uvx --quiet --from patchelf patchelf --set-rpath "$@"; fi; }
work=out/linux-release
mkdir -p "$work"
# SDL3 for glibc 2.38: its float math bound to the old symbol versions (packaging/linux/glibc_compat.c).
sdl_src=out/windows/deps/src/SDL
if [[ ! -f $work/sdl-build/libSDL3.so.0 ]]; then
    [[ -d $sdl_src ]] || { echo "package-linux: no SDL source in $sdl_src (scripts/build-windows.sh fetches it)" >&2; exit 1; }
    gcc -O2 -fPIC -c packaging/linux/glibc_compat.c -o "$work/glibc_compat.o"
    wrap="-Wl,--wrap=acosf -Wl,--wrap=asinf -Wl,--wrap=sqrtf -Wl,--wrap=log10f -Wl,--wrap=atan2f $PWD/$work/glibc_compat.o"
    cmake -S "$sdl_src" -B "$work/sdl-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL=OFF "-DCMAKE_SHARED_LINKER_FLAGS=$wrap" > "$work/sdl-cmake.log"
    ninja -C "$work/sdl-build" > "$work/sdl-build.log"
fi
stage=$work/stage/UFC3Native
rm -rf "$work/stage"
mkdir -p "$stage/bin/gpu" "$stage/bin/lib" "$stage/scripts" "$stage/tools" "$stage/share" "$stage/patches" "$stage/licenses" "$stage/launcher"
install -m755 out/sacredfire out/bb-gpu-capabilities "$stage/bin/"
install -m755 out/gpu/libbbgpu.so "$stage/bin/gpu/"
strip --strip-debug "$stage/bin/sacredfire" "$stage/bin/bb-gpu-capabilities" "$stage/bin/gpu/libbbgpu.so"
cp -L "$work/sdl-build/libSDL3.so.0" "$stage/bin/lib/"
ldd out/sacredfire out/gpu/libbbgpu.so > "$work/ldd.txt"
for lib in libfmt.so libminiz.so libxxhash.so libZycore.so libZydis.so libstdc++.so.6 libgcc_s.so.1; do
    path=$(awk -v l="$lib" 'index($1, l) == 1 && !found {print $3; found = 1}' "$work/ldd.txt")
    [[ -n $path ]] || { echo "package-linux: $lib not found" >&2; exit 1; }
    cp -L "$path" "$stage/bin/lib/$(basename "$path")"
done
# Relative library paths (no build-machine path in the package).
set_rpath '$ORIGIN/gpu:$ORIGIN/lib' "$stage/bin/sacredfire"
set_rpath '$ORIGIN/lib' "$stage/bin/bb-gpu-capabilities"
set_rpath '$ORIGIN/../lib' "$stage/bin/gpu/libbbgpu.so"
for f in "$stage"/bin/lib/*.so*; do set_rpath '$ORIGIN' "$f"; done
install -m755 run.sh packaging/linux/ufc3.sh packaging/linux/UFC3.sh "$stage/"
cp launcher/ufc3_launcher.py launcher/ufc3_online.py "$stage/launcher/"
cp packaging/linux/ufc3.conf.example packaging/linux/README-Linux.md LICENSE "$stage/"
cp out/VERSION "$stage/VERSION"
cp scripts/*.py "$stage/scripts/"; cp tools/gen_import_names.py tools/ufc3_server.py tools/blaze_tdf.py tools/ufc3_auth.py tools/ufc3_games.py "$stage/tools/"
find patches -maxdepth 1 -type f -name '*.xml' -exec cp {} "$stage/patches/" \;
[ ! -d patches/experiments ] || cp -r patches/experiments "$stage/patches/"
cp share/skip_movie.mkv "$stage/share/"
cp out/windows/downloads/aerolib.inl "$stage/share/aerolib.inl" 2>/dev/null || cp out/aerolib.inl "$stage/share/aerolib.inl"
cp gpu/third_party/imgui/LICENSE.txt "$stage/licenses/Dear-ImGui-LICENSE.txt"
cp third_party/LibAtrac9/LICENSE* "$stage/licenses/" 2>/dev/null || true
cp third_party/minimp3/LICENSE "$stage/licenses/minimp3-LICENSE"
cp "$sdl_src/LICENSE.txt" "$stage/licenses/SDL3-LICENSE.txt" 2>/dev/null || true
cp packaging/windows/THIRD-PARTY.md "$stage/licenses/THIRD-PARTY.md"
# Checks: glibc 2.38 at most; no build-machine paths; no game data.
newest=$(find "$stage/bin" -type f \( -name '*.so*' -o -perm -u+x \) -exec objdump -T {} \; 2>/dev/null |
         grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)
[[ $(printf '%s\n' "$newest" GLIBC_2.38 | sort -V | tail -1) == GLIBC_2.38 ]] || { echo "package-linux: needs $newest (> 2.38)" >&2; exit 1; }
if grep -rlq "$HOME" "$stage/bin" "$stage"/*.sh 2>/dev/null; then echo "package-linux: a build-machine path is in the package:" >&2; grep -rl "$HOME" "$stage/bin" >&2; fi
mkdir -p dist
tar -C "$work/stage" -czf dist/UFC3Native-linux-x86_64.tar.gz UFC3Native
(cd dist && sha256sum UFC3Native-linux-x86_64.tar.gz > UFC3Native-linux-x86_64.tar.gz.sha256)
echo "package-linux: dist/UFC3Native-linux-x86_64.tar.gz ($built_from; newest glibc symbol $newest)"
ls -lh dist/UFC3Native-linux-x86_64.tar.gz
