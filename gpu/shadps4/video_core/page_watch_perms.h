// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// ufc3 (UFC-48): the access a tracked page allows, from its watchers. Header-only
// (tests/test_page_watch_perms.cpp).
//
// A read watcher (precise readbacks) means the page's GPU data is only in VRAM: any CPU access
// must fault first so it is read back. A write watcher means the CPU's writes must be seen.
// A page with a read watcher and no write watcher (GPU- and CPU-modified at once) used to ask
// for write-only protection: the Windows page manager asserts on it (UFC48-test-win-20261016,
// entering Career), and on Linux the page was left with no access. Without read, write is
// denied too: the write faults, its readback runs first, and the page opens after it.

namespace VideoCore::PageWatch {

struct Access {
    bool read;
    bool write;
};

constexpr Access FromWatchers(unsigned read_watchers, unsigned write_watchers) {
    const bool read = read_watchers == 0;
    return {read, read && write_watchers == 0};
}

} // namespace VideoCore::PageWatch
