// SPDX-License-Identifier: GPL-2.0-or-later
// UFC-48: a tracked page never asks for write-only protection (PageWatch::FromWatchers). The
// Windows log UFC48-test-win-20261016_170301 stopped entering Career on page_manager.cpp's
// "Attempted to protect region as write-only" assert: a page whose GPU bytes a write window's
// close had all taken for the CPU kept its GPU-modified bit (read watcher), the next write fault's
// readback found nothing to download and left it so, and the fault marked it CPU-modified (no
// write watcher).
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/page_watch_perms.h"

using VideoCore::PageWatch::FromWatchers;

int main() {
    for (unsigned read = 0; read <= 1; ++read) {
        for (unsigned write = 0; write <= 3; ++write) {
            const auto access = FromWatchers(read, write);
            assert(!(access.write && !access.read)); // never write-only
            assert(access.read == (read == 0));
            assert(access.write == (read == 0 && write == 0));
        }
    }
    // The Career page: GPU-modified (read watcher), CPU-modified (no write watcher): no access,
    // so the CPU's next access faults and its readback clears the GPU bit first.
    static_assert(!FromWatchers(1, 0).read && !FromWatchers(1, 0).write);
    static_assert(FromWatchers(0, 0).read && FromWatchers(0, 0).write);
    static_assert(FromWatchers(0, 2).read && !FromWatchers(0, 2).write);
    std::puts("page watch perms: ok");
    return 0;
}
