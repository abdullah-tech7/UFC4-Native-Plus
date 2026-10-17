// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef U4_TEXTURE_PROBE_ABI_H
#define U4_TEXTURE_PROBE_ABI_H
#include <stdint.h>
#include <stddef.h>
typedef struct {
    void *buffer;
    uint64_t offset,size;
    unsigned char *mapped;
    uint8_t type;
} U4ProbeStaging;
typedef struct {
    uint64_t offset;
    uint32_t row_length,image_height,aspect,mip,base_layer,layers;
    int32_t x,y,z;
    uint32_t width,height,depth;
} U4ProbeCopy;
typedef struct { const U4ProbeCopy *data; uint64_t count; } U4ProbeSpan;
_Static_assert(sizeof(U4ProbeStaging)==40,"donor StagingBufferRef ABI");
_Static_assert(sizeof(U4ProbeCopy)==56,"Vulkan BufferImageCopy ABI");
_Static_assert(sizeof(U4ProbeSpan)==16,"libc++ span ABI");
#ifndef U4_PROBE_TEST
extern U4ProbeStaging u4_probe_request(void *,uint64_t,uint8_t,uint64_t,uint8_t)
 __asm__("_ZN6Vulkan17StagingBufferPool7RequestEyN9VideoCore10MemoryTypeEyb");
extern void u4_probe_download(void *,void *,const void *,U4ProbeSpan)
 __asm__("_ZN6Vulkan7Runtime13DownloadImageEPN9VideoCore5ImageEPKNS1_6BufferENSt3__14spanIKN2vk15BufferImageCopyELy18446744073709551615EEE");
#else
extern U4ProbeStaging u4_probe_request(void *,uint64_t,uint8_t,uint64_t,uint8_t);
extern void u4_probe_download(void *,void *,const void *,U4ProbeSpan);
#endif
#endif
