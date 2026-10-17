// SPDX-License-Identifier: GPL-2.0-or-later
// sacredfire (Windows): headers the FidelityFX SDK sources (gpu/third_party/fsr-vulkan) rely on
// MSVC's standard library including transitively: _countof and swprintf_s live in mingw-w64's
// stdlib.h and wchar.h, std::lock_guard, placement new and log2 in <mutex>, <new>, <cmath>.
// Force-included into every FSR translation unit by gpu/CMakeLists.txt; nothing else uses it.
#pragma once
#include <stdlib.h>
#include <wchar.h>
#ifdef __cplusplus
#include <cmath>
#include <mutex>
#include <new>
#endif
