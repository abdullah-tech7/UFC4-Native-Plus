// UFC-47: the late-label guard (BbClobber::GuardLabel) lets a label the port bumps inside a
// submitted command buffer go through (the game's counter at +0x510: 1 at submit, then 2, 3,
// ... from the buffer's own EOS packets), and still drops a label whose slot the game rebuilt.
#include "gpu/shim/bbport_clobber.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// The game executable provides these to the GPU library (the guard's report).
extern "C" {
void platform_describe_address(std::uintptr_t address, char* out, std::size_t size) {
    std::snprintf(out, size, "%#llx", (unsigned long long)address);
}
int platform_backtrace(void**, int) {
    return 0;
}
void platform_thread_get_name(char* out, std::size_t size) {
    std::snprintf(out, size, "test");
}
std::uint32_t platform_thread_id(void) {
    return 1;
}
}

namespace {
std::uint32_t buffer[0x400];

// What WriteEosLabel does with one label: note, guard, store.
bool Label(std::uint32_t value, const std::uint32_t* at_submit) {
    const auto address = reinterpret_cast<std::uint64_t>(&buffer[0x510 / 4]);
    BbClobber::NoteWrite(address, value, 4, BbClobber::Eos);
    if (BbClobber::GuardLabel(address, value, 4, BbClobber::Eos, 1, at_submit)) {
        return false;
    }
    buffer[0x510 / 4] = value;
    return true;
}
} // namespace

int main() {
    buffer[0x510 / 4] = 1;
    BbClobber::NoteSubmit(1, buffer, sizeof(buffer));
    const std::uint32_t at_submit[2] = {1, 0};
    for (std::uint32_t value = 2; value <= 11; ++value) {
        assert(Label(value, at_submit));
        assert(buffer[0x510 / 4] == value);
    }
    // A WRITE_DATA of the buffer's own into the slot is the port's too.
    BbClobber::NoteWrite(reinterpret_cast<std::uint64_t>(&buffer[0x510 / 4]), 20, 4,
                         BbClobber::WriteData);
    buffer[0x510 / 4] = 20;
    assert(Label(21, at_submit));
    // The game rebuilds the buffer: a packet header where the label was.
    buffer[0x510 / 4] = 0xc0012800;
    assert(!Label(22, at_submit));
    assert(buffer[0x510 / 4] == 0xc0012800);
    std::puts("label guard: ok");
    return 0;
}
