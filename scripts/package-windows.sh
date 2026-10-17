#!/usr/bin/env bash
# SacredFire Windows release archive, made on Linux after scripts/build-windows.sh:
#   dist/UFC3Native-windows-x86_64.zip  with .sha256 and .build.txt (source, donor, toolchain, lock)
# Staged: SacredFire.exe (launcher), bin/ (sacredfire-game.exe, bb-gpu-capabilities.exe, the
# DLLs they import that are not part of Windows: SDL3, libc++, libunwind, libwinpthread), python/
# (the official embeddable package, pinned), scripts/ and tools/gen_import_names.py (preparation),
# patches/ (community patches), share/aerolib.inl, licenses/, README-Windows.md, LICENSE, VERSION.
# Never: game files, generated images, caches, saves, graphics drivers, build-host paths.
# The DLL closure is resolved by reading each PE's import table recursively (llvm-readobj); every
# staged binary must be an AMD64 PE; a dirty or untagged checkout is refused unless BB_ALLOW_DIRTY=1.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
root=${BB_WINDOWS_ROOT:-$PWD/out/windows}
toolchain_tar=$(sed -n 's/^llvm-mingw archive [^ ]*\/\([^ ]*\) .*/\1/p' packaging/windows/deps.lock)
tc=${LLVM_MINGW_ROOT:-$root/toolchain/${toolchain_tar%.tar.xz}}
readobj=$tc/bin/llvm-readobj
[[ -f $root/bin/sacredfire-game.exe && -f $root/bin/SacredFire.exe && -f $root/bin/VERSION ]] || { echo 'Build first: scripts/build-windows.sh' >&2; exit 1; }
built_from=$(cat "$root/bin/VERSION")
checkout=$(git describe --tags --match 'ufc3-v*' --always 2>/dev/null || echo unknown)
git update-index -q --refresh 2>/dev/null || true
git diff-index --quiet --ignore-submodules=dirty HEAD -- 2>/dev/null || checkout="$checkout-dirty"
if [[ -z ${BB_ALLOW_DIRTY:-} ]]; then
    [[ $checkout != *-dirty ]] || { echo "The checkout has uncommitted changes ($checkout): commit and tag first (BB_ALLOW_DIRTY=1 for a test archive)." >&2; exit 1; }
    [[ $built_from == "$checkout" ]] || { echo "out/windows/bin was built from $built_from, the checkout is at $checkout: build again." >&2; exit 1; }
fi
stage=$root/stage/UFC3Native
rm -rf "$root/stage"
mkdir -p "$stage/bin" "$stage/python" "$stage/share" "$stage/licenses" "$stage/tools"
# Programs, stripped of debug information (out/windows/bin keeps the symbols for crash reports).
"$tc/bin/x86_64-w64-mingw32-strip" --strip-debug -o "$stage/UFC3.exe" "$root/bin/SacredFire.exe"
for exe in sacredfire-game.exe bb-gpu-capabilities.exe; do "$tc/bin/x86_64-w64-mingw32-strip" --strip-debug -o "$stage/bin/$exe" "$root/bin/$exe"; done
# DLL closure: imports of each PE, resolved against the sysroot and the toolchain; Windows' own
# DLLs (kernel32, user32, the api-ms-win-* sets, vulkan-1 from the graphics driver) are not shipped.
declare -A seen
resolve() {
    local pe=$1
    "$readobj" --coff-imports "$pe" | sed -n 's/^ *Name: \(.*\)$/\1/p' | while read -r dll; do
        local lower=${dll,,}
        case $lower in
        kernel32.dll|user32.dll|gdi32.dll|comctl32.dll|advapi32.dll|shell32.dll|ole32.dll|oleaut32.dll|shlwapi.dll|ws2_32.dll|bcrypt.dll|ntdll.dll|\
        winmm.dll|imm32.dll|version.dll|setupapi.dll|cfgmgr32.dll|dwmapi.dll|uxtheme.dll|crypt32.dll|msvcrt.dll|vulkan-1.dll|\
        api-ms-win-*|ext-ms-*|dbghelp.dll|synchronization.dll|onecore.dll|comdlg32.dll|hid.dll|xinput*.dll|dinput8.dll|\
        d3d11.dll|d3d12.dll|dxgi.dll|opengl32.dll|wininet.dll|urlmon.dll|iphlpapi.dll|dxcore.dll|windows.*.dll|avrt.dll|mmdevapi.dll|ole32.dll)
            ;;
        *)
            [[ -n ${seen[$lower]:-} ]] && continue
            seen[$lower]=1
            local found=
            for dir in "$root/sysroot/bin" "$tc/x86_64-w64-mingw32/bin"; do
                if [[ -f $dir/$dll ]]; then found=$dir/$dll; break; fi
            done
            [[ -n $found ]] || { echo "package-windows: $pe imports $dll, which is neither a Windows DLL nor in the sysroot/toolchain" >&2; exit 1; }
            "$readobj" --file-headers "$found" | grep -q IMAGE_FILE_MACHINE_AMD64 || { echo "package-windows: $found is not an AMD64 DLL" >&2; exit 1; }
            cp "$found" "$stage/bin/$dll"
            echo "  $dll <- $found"
            resolve "$found"
            ;;
        esac
    done
}
echo 'package-windows: DLL closure'
resolve "$stage/bin/sacredfire-game.exe"; resolve "$stage/bin/bb-gpu-capabilities.exe"; resolve "$stage/UFC3.exe"
# The Python embeddable package (stdlib in python313.zip; no Tk, no pip) and the preparation
# scripts. python -I (the launcher's invocation) imports only from there and the script's folder.
python_zip=$(sed -n 's/^python-embed archive [^ ]*\/\([^ ]*\) .*/\1/p' packaging/windows/deps.lock)
python3 - "$root/downloads/$python_zip" "$stage/python" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for member in z.infolist():
        if '..' in member.filename or member.filename.startswith('/'): raise SystemExit('unsafe zip member ' + member.filename)
    z.extractall(sys.argv[2])
PY
# The embeddable package's ._pth file is the whole import path (no script directory, no site):
# the standard library zip, the python folder, and the preparation scripts (they import each other).
pth=$(ls "$stage"/python/python*._pth | head -1)
printf 'python%s.zip\n.\n..\\scripts\n..\\tools\n' "$(basename "$pth" | sed 's/python\([0-9]*\)\._pth/\1/')" > "$pth"
mkdir -p "$stage/scripts"; cp scripts/*.py "$stage/scripts/"
cp tools/gen_import_names.py tools/ufc3_server.py tools/blaze_tdf.py tools/ufc3_auth.py tools/ufc3_games.py "$stage/tools/"
# Community patches only: not the disabled CTR file, not shader dumps taken from the game.
mkdir -p "$stage/patches"
find patches -maxdepth 1 -type f -name '*.xml' -exec cp {} "$stage/patches/" \;
# The launcher's 2K / 4K experiments (UFC-45/46): each folder's patch and its patches json.
find patches/experiments -mindepth 2 -maxdepth 2 -type f \( -name '*.xml' -o -name '*.json' \) | while read -r f; do
    mkdir -p "$stage/$(dirname "$f")"; cp "$f" "$stage/$f"
done
cp "$root/downloads/aerolib.inl" "$stage/share/aerolib.inl"
cp share/skip_movie.mkv "$stage/share/skip_movie.mkv"
cp LICENSE "$stage/LICENSE"; cp "$root/bin/VERSION" "$stage/VERSION"
cp packaging/windows/README-Windows.md "$stage/README-Windows.md"
# Licences of what is in the archive.
cp gpu/third_party/imgui/LICENSE.txt "$stage/licenses/Dear-ImGui-LICENSE.txt"
cp third_party/LibAtrac9/LICENSE "$stage/licenses/LibAtrac9-LICENSE.txt" 2>/dev/null || cp third_party/LibAtrac9/LICENSE.md "$stage/licenses/LibAtrac9-LICENSE.md" 2>/dev/null || true
cp gpu/third_party/fsr-vulkan/LICENSE* "$stage/licenses/" 2>/dev/null || true
cp gpu/third_party/sirit/LICENSE.txt "$stage/licenses/sirit-LICENSE.txt"
cp gpu/third_party/half/LICENSE.txt "$stage/licenses/half-LICENSE.txt" 2>/dev/null || true
for pair in "SDL:LICENSE.txt:SDL3-LICENSE.txt" "fmt:LICENSE:fmt-LICENSE.txt" "xxHash:LICENSE:xxHash-LICENSE.txt" "zydis:LICENSE:Zydis-LICENSE.txt" "zydis/dependencies/zycore:LICENSE:Zycore-LICENSE.txt" "miniz:LICENSE:miniz-LICENSE.txt" "VulkanMemoryAllocator:LICENSE.txt:VulkanMemoryAllocator-LICENSE.txt" "magic_enum:LICENSE:magic_enum-LICENSE.txt" "xbyak:COPYRIGHT:xbyak-COPYRIGHT.txt" "robin-map:LICENSE:robin-map-LICENSE.txt" "Vulkan-Headers:LICENSE.md:Vulkan-Headers-LICENSE.md"; do
    IFS=: read -r dir file target <<< "$pair"
    cp "$root/deps/src/$dir/$file" "$stage/licenses/$target" 2>/dev/null || echo "package-windows: no $dir/$file" >&2
done
cp "$root/deps/src/boost-1.92.0/LICENSE_1_0.txt" "$stage/licenses/Boost-LICENSE_1_0.txt" 2>/dev/null || true
cp "$tc/LICENSE.TXT" "$stage/licenses/llvm-mingw-LICENSE.txt" 2>/dev/null || cp "$tc"/LICENSE* "$stage/licenses/" 2>/dev/null || true
cp "$stage/python/LICENSE.txt" "$stage/licenses/Python-LICENSE.txt" 2>/dev/null || true
cp packaging/windows/THIRD-PARTY.md "$stage/licenses/THIRD-PARTY.md"
# Inspection: every PE is AMD64; no Linux ELF, no build-host paths, no game data.
while IFS= read -r -d '' f; do
    if head -c 4 "$f" | grep -q 'ELF'; then echo "package-windows: ELF object in the archive: $f" >&2; exit 1; fi
    case $f in *.exe|*.dll|*.pyd) "$readobj" --file-headers "$f" | grep -q IMAGE_FILE_MACHINE_AMD64 || { echo "package-windows: $f is not AMD64" >&2; exit 1; } ;; esac
done < <(find "$stage" -type f -print0)
if grep -rl -- "$PWD" "$stage/bin" "$stage/UFC3.exe" 2>/dev/null | head -1 | grep -q .; then echo "package-windows: a staged binary carries the build host path $PWD" >&2; exit 1; fi
for forbidden in eboot.bin '*.prx' '*.sav' 'boot*.bin' '*.spv'; do
    if find "$stage" -iname "$forbidden" | grep -q .; then echo "package-windows: $forbidden inside the archive" >&2; exit 1; fi
done
find "$stage" -name __pycache__ -prune -exec rm -r {} +
mkdir -p dist
archive=dist/UFC3Native-windows-x86_64.zip
archive_path=$PWD/$archive
rm -f "$archive"
(cd "$root/stage" && python3 -c "
import os, sys, zipfile, time
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for base, dirs, files in sorted(os.walk('UFC3Native')):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            info = zipfile.ZipInfo(path, date_time=(2026, 1, 1, 0, 0, 0))  # fixed timestamps: the archive hash depends on content only
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if name.endswith('.exe') else 0o644) << 16
            with open(path, 'rb') as f: z.writestr(info, f.read())
" "$archive_path")
(cd dist && sha256sum UFC3Native-windows-x86_64.zip > UFC3Native-windows-x86_64.zip.sha256)
donor=$(sed -n 's/^# donor Supermedo\/bloodborne_pc \([0-9a-f]*\).*/\1/p' packaging/windows/deps.lock | head -1)
{
    printf 'SacredFire %s (Windows x86-64)\nsource commit %s\ndonor Supermedo/bloodborne_pc %s\ntoolchain %s\nlock sha256 %s\nbuild options: BB_LTO=%s BB_PGO=off BB_AVPLAYER=OFF, --disable-high-entropy-va, stack 16 MiB committed\n' \
        "$built_from" "$(git rev-parse HEAD)" "${donor:-unknown}" "${toolchain_tar%.tar.xz}" "$(sha256sum packaging/windows/deps.lock | cut -d' ' -f1)" "${BB_LTO:-OFF}"
    echo 'files:'
    (cd "$root/stage" && find UFC3Native -type f | sort | while read -r f; do printf '  %s %s\n' "$(sha256sum "$f" | cut -c1-16)" "$f"; done)
} > dist/UFC3Native-windows-x86_64.zip.build.txt
ls -lh "$archive"; head -8 dist/UFC3Native-windows-x86_64.zip.build.txt
