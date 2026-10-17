// SPDX-License-Identifier: GPL-2.0-or-later
/* Compatibility implementation for the SHA-256-identified ufc3-v0.1.2 donor.
 * Layouts below are checked against its retained symbols/instructions by the
 * linker. A full renderer build uses TextureCache::InvalidateMemoryFromGPU.
 * No game assets, shaders, or saved characters are changed.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef U4_TEXTURE_TEST
extern void u4_texture_lock(void *) __asm__("_ZNSt3__15mutex4lockEv");
extern void u4_texture_unlock(void *) __asm__("_ZNSt3__15mutex6unlockEv");
extern void u4_texture_refresh(void *,void *)
    __asm__("_ZN9VideoCore12TextureCache12RefreshImageERNS_5ImageE");
extern void u4_texture_copy(void *,void *,void *)
    __asm__("_ZN6Vulkan7Runtime9CopyImageEPN9VideoCore5ImageES3_");
extern void u4_texture_copy_mip(void *,void *,void *,uint32_t,uint32_t)
    __asm__("_ZN6Vulkan7Runtime7CopyMipEPN9VideoCore5ImageES3_jj");
#else
extern void u4_texture_lock(void *);
extern void u4_texture_unlock(void *);
extern void u4_texture_refresh(void *,void *);
extern void u4_texture_copy(void *,void *,void *);
extern void u4_texture_copy_mip(void *,void *,void *,uint32_t,uint32_t);
#endif

static uintptr_t u4_texture_pointer(const unsigned char *p) {
    uintptr_t value;
    memcpy(&value,p,sizeof(value));
    return value;
}

/* ExpandImage and ResolveOverlap already hold the cache mutex. Refreshing the
 * destination alone lets an old CPU/GPU-dirty source overwrite its fresh upload.
 * Refresh both BEFORE the copy, preserving GPU-only writes via RefreshImage's
 * normal dirty/hash rules. No unconditional upload or post-copy CPU overwrite.
 */
void ufc4_texture_expand_copy(void *cache,void *source,void *destination) {
    u4_texture_refresh(cache,source);
    u4_texture_refresh(cache,destination);
    void *runtime=(void *)u4_texture_pointer((unsigned char *)cache+0x10);
    u4_texture_copy(runtime,source,destination);
}

void ufc4_texture_merge_mip(void *cache,void *source,void *destination,
                           uint32_t mip,uint32_t slice) {
    u4_texture_refresh(cache,source);
    u4_texture_refresh(cache,destination);
    void *runtime=(void *)u4_texture_pointer((unsigned char *)cache+0x10);
    u4_texture_copy_mip(runtime,source,destination,mip,slice);
}

void ufc4_texture_invalidate_gpu(void *cache, uint64_t address, uint64_t size) {
    const uint64_t limit=UINT64_C(1)<<40;
    if (!size || address>=limit) return;
    /* Clamp before adding to avoid wrapping the guest's 40-bit address space. */
    if (size>limit-address) size=limit-address;
    const uint64_t end=address+size;
    unsigned char *bytes=cache;
    u4_texture_lock(bytes+0x5e0);
    const unsigned char *images=(void *)u4_texture_pointer(bytes+0x158);
    const unsigned char *first=(void *)u4_texture_pointer(bytes+0x5a0);
    for (uint64_t page=address>>20;page<=(end-1)>>20;++page) {
        const unsigned char *second=(void *)u4_texture_pointer(first+(page>>10)*8);
        if (!second) continue;
        const unsigned char *entry=second+(page&1023)*0x58;
        const unsigned char *ids=(void *)u4_texture_pointer(entry);
        const uint64_t count=u4_texture_pointer(entry+8);
        for (uint64_t i=0;i<count;++i) {
            uint32_t id,flags,length;
            memcpy(&id,ids+i*4,4);
            unsigned char *image=(unsigned char *)images+(size_t)id*0x2d8;
            memcpy(&flags,image,4);
            const uint64_t begin=u4_texture_pointer(image+0x1c8);
            memcpy(&length,image+0x1d0,4);
            if (begin<end && address<begin+(uint64_t)length) {
                flags|=4; /* ImageFlagBits::GpuDirty; repeated visits are harmless. */
                memcpy(image,&flags,4);
            }
        }
    }
    u4_texture_unlock(bytes+0x5e0);
}
