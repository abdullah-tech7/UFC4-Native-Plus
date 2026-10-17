// SPDX-License-Identifier: GPL-2.0-or-later
#include <assert.h>
#include <stdio.h>
#include "../src/ufc4_texture_probe_abi.h"
/* The exact C declarations used by the extension call independent C++ methods. */
int main(void) {
    U4ProbeStaging ref=u4_probe_request((void*)0x12345678,688128,2,16,0);
    assert(ref.buffer==(void*)0x76543210 && ref.offset==32 && ref.size==688128);
    assert(ref.mapped==(void*)0x45678900 && ref.type==2);
    U4ProbeCopy copies[2]={{.offset=32,.width=1024},
                           {.offset=524320,.mip=1,.height=256}};
    u4_probe_download((void*)0x11111111,(void*)0x22222222,ref.buffer,(U4ProbeSpan){copies,2});
    puts("PASS: actual Clang Windows C-to-C++ member ABI for 40-byte sret and 16-byte std::span");
    return 0;
}
