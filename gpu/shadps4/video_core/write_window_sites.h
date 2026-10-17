// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// ufc3 (UFC-48, BB_WRITE_WINDOW): write sites barred from windows. Header-only
// (tests/test_write_window_sites.cpp); the caller locks.
//
// A window decides who owns a byte by comparing the page with its copy at open. A CPU write that
// leaves the stale guest value over GPU-written bytes (the game rewriting memory the GPU wrote,
// e.g. a table of zeros over a freed GPU output) changes nothing there: the bytes stay
// GPU-owned and the GPU reads VRAM's old value. No diff can see it. What a window can see is the
// same site changing other GPU bytes: a site that writes over GPU data. Such a site reads the page
// back before writing from then on (exact, as with windows off). Sites that write beside GPU data
// only (every window in a fight: "CPU took over 0 GPU bytes") keep their windows.

#include <array>
#include <cstddef>
#include <cstdint>

namespace VideoCore::WindowSites {

/// A write site: the faulting instruction (guest offset, or a host address with its tag bit) and
/// the guest code that called it (host copies: memcpy's caller).
struct Site {
    std::uint64_t rip = 0;
    std::uint64_t caller = 0;
    bool operator==(const Site&) const = default;
};

template <std::size_t Capacity>
class Barred {
public:
    [[nodiscard]] bool Contains(const Site& site) const {
        for (std::size_t i = 0; i < count; ++i) {
            if (sites[i] == site) {
                return true;
            }
        }
        return false;
    }

    /// True when newly barred. A full table bars every window (`full`): safe, slower.
    bool Add(const Site& site) {
        if (Contains(site)) {
            return false;
        }
        if (count == Capacity) {
            full = true;
            return false;
        }
        sites[count++] = site;
        return true;
    }

    [[nodiscard]] bool Bars(const Site& site) const {
        return full || Contains(site);
    }

    [[nodiscard]] std::size_t Size() const {
        return count;
    }

    [[nodiscard]] bool Full() const {
        return full;
    }

private:
    std::array<Site, Capacity> sites{};
    std::size_t count = 0;
    bool full = false;
};

} // namespace VideoCore::WindowSites
