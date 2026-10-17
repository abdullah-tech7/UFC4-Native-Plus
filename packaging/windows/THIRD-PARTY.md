# Third-party components in the SacredFire Windows package

SacredFire is licensed under the GNU GPL v2 or later (LICENSE). The corresponding source of
the binaries in this archive is the SacredFire repository at the commit named in VERSION and
in `SacredFire-windows-x86_64.zip.build.txt`, together with the inputs pinned in
`packaging/windows/deps.lock` of that commit (exact commits or SHA-256 of every third-party
source and tool). This file lists what the archive contains besides SacredFire's own code.

| component | where in the archive | licence | source |
|---|---|---|---|
| bbport (deadinside28/bloodborne_pc) and its Windows adaptations from Supermedo/bloodborne_pc | bin/sacredfire-game.exe (runtime, loader, GPU shims) | GPL-2.0-or-later | github.com/deadinside28/bloodborne_pc, github.com/Supermedo/bloodborne_pc |
| shadPS4 video core and shader recompiler (vendored, gpu/VENDOR.txt) | bin/sacredfire-game.exe | GPL-2.0-or-later | github.com/shadps4-emu/shadPS4 |
| shadPS4 aerolib.inl (NID to symbol names) | share/aerolib.inl | GPL-2.0-or-later | github.com/shadps4-emu/shadPS4 |
| sirit (SPIR-V emitter) | bin/sacredfire-game.exe | BSD-3-Clause | sirit-LICENSE.txt |
| half (half-precision floats) | bin/sacredfire-game.exe | MIT | half-LICENSE.txt |
| Dear ImGui | bin/sacredfire-game.exe | MIT | Dear-ImGui-LICENSE.txt |
| FSR-Vulkan (FireBurn) and AMD FidelityFX SDK | bin/sacredfire-game.exe (upscalers, off by default) | MIT | fsr-vulkan LICENSE |
| LibAtrac9 | bin/sacredfire-game.exe (audio decoding) | MIT | LibAtrac9-LICENSE |
| DejaVu fonts | bin/sacredfire-game.exe (in-game menu font) | Bitstream Vera / public domain | dejavu-fonts.github.io |
| SDL3 | bin/SDL3.dll | Zlib | SDL3-LICENSE.txt |
| fmt | bin/sacredfire-game.exe | MIT | fmt-LICENSE.txt |
| xxHash | bin/sacredfire-game.exe | BSD-2-Clause | xxHash-LICENSE.txt |
| Zydis and Zycore | bin/sacredfire-game.exe (TLS site decoding, shader recompiler) | MIT | Zydis-LICENSE.txt, Zycore-LICENSE.txt |
| miniz | bin/sacredfire-game.exe (window icon PNG) | MIT | miniz-LICENSE.txt |
| Vulkan-Headers | compiled in | Apache-2.0 / MIT | Vulkan-Headers-LICENSE.md |
| VulkanMemoryAllocator | compiled in | MIT | VulkanMemoryAllocator-LICENSE.txt |
| magic_enum | compiled in | MIT | magic_enum-LICENSE.txt |
| xbyak | compiled in | BSD-3-Clause | xbyak-COPYRIGHT.txt |
| robin-map | compiled in | MIT | robin-map-LICENSE.txt |
| Boost (header-only parts) | compiled in | BSL-1.0 | Boost-LICENSE_1_0.txt |
| libc++, libc++abi, libunwind (LLVM), mingw-w64 CRT and winpthreads | bin/libc++.dll, bin/libunwind.dll, bin/libwinpthread-1.dll, startup code in every .exe | Apache-2.0 with LLVM exception; mingw-w64: ZPL/public domain, winpthreads: MIT | llvm-mingw-LICENSE.txt |
| Python (embeddable package) | python/ | PSF-2.0 | Python-LICENSE.txt |
| Game patches by illusion and TroyWarez (illusionyy/PS-Game-Patch) | patches/CrashTeamRacing.xml | community patches, attribution kept | github.com/illusionyy/PS-Game-Patch |

Not in the archive: vulkan-1.dll (part of your graphics driver), the Universal C Runtime
(part of Windows 10), any game file. SacredFire is not affiliated with Sony Interactive
Entertainment, Activision or Beenox.
