// SPDX-License-Identifier: GPL-2.0-or-later
/* Optional, bounded GPU readback for the SHA-identified donor. Called only by
 * its existing frame recorder, on the serial GPU command thread. No guest
 * memory is read or written. This is diagnostic code, not a visual workaround.
 * All private offsets used below have instruction witnesses in the linker.
 */
#include "ufc4_texture_probe_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef U4_PROBE_TEST
extern void u4_probe_finish(void *) __asm__("_ZN6Vulkan9Scheduler6FinishEv");
extern void u4_probe_invalidate(void *,uint64_t,uint64_t)
 __asm__("_ZN9VideoCore6Buffer10InvalidateEyy");
extern void u4_probe_sampled_original(const void *,uint8_t)
 __asm__("_ZN6Vulkan12FrameCapture7SampledERKN9VideoCore9ImageInfoEb");
extern void u4_probe_draw_original(uint64_t,uint64_t,uint32_t,uint32_t)
 __asm__("_ZN6Vulkan12FrameCapture4DrawEyyjj");
/* Direct RIP-relative references avoid absolute .refptr tables in the COFF
 * extension; the linker resolves these three identified donor data symbols. */
#define U4_PROBE_GLOBAL(function,symbol) \
    static const unsigned char *function(void) { const unsigned char *p; \
      __asm__("leaq " symbol "(%%rip), %0":"=r"(p));return p; }
U4_PROBE_GLOBAL(u4_probe_get_pending,"_ZN6Vulkan12_GLOBAL__N_115pending_sampledE")
U4_PROBE_GLOBAL(u4_probe_get_flips,"_ZN6Vulkan12FrameCapture5flipsE")
U4_PROBE_GLOBAL(u4_probe_get_state,"_ZN6Vulkan12FrameCapture5stateE")
#else
extern void u4_probe_finish(void *);
extern void u4_probe_invalidate(void *,uint64_t,uint64_t);
extern void u4_probe_sampled_original(const void *,uint8_t);
extern void u4_probe_draw_original(uint64_t,uint64_t,uint32_t,uint32_t);
extern unsigned char u4_probe_pending[],u4_probe_flips[],u4_probe_state[];
#define u4_probe_get_pending() u4_probe_pending
#define u4_probe_get_flips() u4_probe_flips
#define u4_probe_get_state() u4_probe_state
#endif

static uint32_t u4_probe_u32(const void *p) { uint32_t n;memcpy(&n,p,4);return n; }
static uint64_t u4_probe_u64(const void *p) { uint64_t n;memcpy(&n,p,8);return n; }
static void *u4_probe_ptr(const void *p) { void *n;memcpy(&n,p,sizeof(n));return n; }

enum { U4_PROBE_MAX_IMAGES=48, U4_PROBE_PENDING=64 };
static unsigned char *u4_probe_images[U4_PROBE_PENDING];
static unsigned u4_probe_image_count,u4_probe_done_count;
static uint64_t u4_probe_bytes;
static struct { uint64_t uid,flip; } u4_probe_done[U4_PROBE_MAX_IMAGES];

/* BC1/BC2/BC3 colour/alpha maps. Do not infer pixels from the CPU backing of
 * generated maps: read the actual selected VkImage after its prior commands.
 * Ignore storage images and reject volumes, MSAA, huge or dirty images. */
static unsigned u4_probe_block_bytes(uint32_t format) {
    if (format>=131 && format<=134) return 8;
    if (format>=135 && format<=138) return 16;
    return 0;
}
static int u4_probe_target(uint64_t ps) {
    const char *selected=getenv("BB_UFC4_TEXTURE_PROBE_PS");
    if(selected && *selected) return ps==strtoull(selected,0,16);
    switch (ps) {
    case 0x7388baf7:case 0x7f438824: /* skin and CAF skin */
    case 0x269ff1bd:case 0x9d6e4947: /* hair */
    case 0x1ad1784d:case 0xc7a8f70f: /* hair cards */
    case 0xc7ebda56:case 0xe442b032: /* hair caps */
    case 0xb4496c55:case 0x19de3644: /* hair strands */
        return 1;
    default:return 0;
    }
}

void ufc4_probe_sampled(const void *info,uint8_t storage) {
    /* The original vector is cleared by Draw, Dispatch and pass boundaries.
     * Its empty state bounds the pointers to the current binding only. */
    const unsigned char *pending=u4_probe_get_pending();
    if (u4_probe_u64(pending)==u4_probe_u64(pending+8))
        u4_probe_image_count=0;
    if (!storage && u4_probe_image_count<U4_PROBE_PENDING)
        u4_probe_images[u4_probe_image_count++]=(unsigned char *)info-0x68;
    u4_probe_sampled_original(info,storage);
}

static void u4_probe_save(const char *directory,unsigned char *image,
                          uint64_t flip,uint64_t ps,uint32_t indices) {
    if (u4_probe_done_count>=U4_PROBE_MAX_IMAGES) return;
    const uint64_t uid=u4_probe_u64(image+0x60);
    for (unsigned i=0;i<u4_probe_done_count;++i)
        if(u4_probe_done[i].uid==uid && u4_probe_done[i].flip==flip)return;
    const uint32_t flags=u4_probe_u32(image);
    const uint32_t format=u4_probe_u32(image+0x8c);
    const uint32_t width=u4_probe_u32(image+0xa0),height=u4_probe_u32(image+0xa4);
    const unsigned block=u4_probe_block_bytes(format);
    const uint32_t levels=u4_probe_u32(image+0x98);
    if (!block || (flags&7) || width<256 || height<128 || width>4096 || height>4096 ||
        !levels || levels>13 || u4_probe_u32(image+0xa8)!=1 ||
        u4_probe_u32(image+0xb0)!=1 || !u4_probe_u32(image+0x9c)) return;
    U4ProbeCopy copies[13]={0};
    uint64_t total=0;
    for (uint32_t mip=0;mip<levels;++mip) {
        uint32_t w=width>>mip,h=height>>mip;if(!w)w=1;if(!h)h=1;
        copies[mip]=(U4ProbeCopy){.offset=total,.aspect=1,.mip=mip,
            .layers=1,.width=w,.height=h,.depth=1};
        total+=(uint64_t)((w+3)/4)*((h+3)/4)*block;
    }
    if (total>32*UINT64_C(1024)*1024 ||
        u4_probe_bytes+total>128*UINT64_C(1024)*1024) return;
    char path[1200];
    if (snprintf(path,sizeof(path),"%s/texture-%02u-%llx-%llx-fmt%u.raw",directory,
          u4_probe_done_count,(unsigned long long)flip,(unsigned long long)uid,format)
        >=(int)sizeof(path)) return;
    /* Verify the directory is writable before invoking any Vulkan operation. */
    FILE *file=fopen(path,"wb");if(!file)return;
    unsigned char *runtime=u4_probe_ptr(image+0x1e8);
    if (!runtime) { fclose(file);return; }
    U4ProbeStaging staging=u4_probe_request(runtime+0x30,total,2,16,0);
    if (!staging.buffer || !staging.mapped || staging.size<total) {
        fclose(file);puts("UFC4 GPU material capture: no mapped staging buffer");return;
    }
    for(uint32_t mip=0;mip<levels;++mip)copies[mip].offset+=staging.offset;
    u4_probe_download(runtime,image,staging.buffer,(U4ProbeSpan){copies,levels});
    u4_probe_finish(u4_probe_ptr(runtime+0x20));
    u4_probe_invalidate(staging.buffer,staging.offset,total);
    size_t written=fwrite(staging.mapped,1,(size_t)total,file);
    const int close_error=fclose(file);
    const unsigned number=u4_probe_done_count++;
    u4_probe_done[number].uid=uid;u4_probe_done[number].flip=flip;
    u4_probe_bytes+=total;
    printf("UFC4 GPU material capture: %s (%llu bytes, PS %08x, idx %u)\n",path,
           (unsigned long long)total,(unsigned)ps,indices);
    char manifest[1200];
    if(snprintf(manifest,sizeof(manifest),"%s/textures.jsonl",directory)>=(int)sizeof(manifest))return;
    file=fopen(manifest,"a");if(!file)return;
    fprintf(file,"{\"file\":\"texture-%02u-%llx-%llx-fmt%u.raw\",\"flip\":%llu,\"uid\":%llu,"
      "\"ps\":\"%08x\",\"indices\":%u,\"address\":\"0x%llx\",\"format\":%u,"
      "\"width\":%u,\"height\":%u,\"levels\":%u,\"layers\":%u,\"captured_layer\":0,"
      "\"flags\":%u,\"tile_mode\":%u,\"gpu_write_tick\":%llu,\"bytes\":%llu,"
      "\"written\":%llu,\"close_error\":%d,\"mips\":[",
      number,(unsigned long long)flip,(unsigned long long)uid,format,
      (unsigned long long)flip,(unsigned long long)uid,(unsigned)ps,indices,
      (unsigned long long)u4_probe_u64(image+0x1c8),format,width,height,levels,
      u4_probe_u32(image+0x9c),flags,u4_probe_u32(image+0xb8),
      (unsigned long long)u4_probe_u64(image+0x48),(unsigned long long)total,
      (unsigned long long)written,close_error);
    for(uint32_t mip=0;mip<levels;++mip)
        fprintf(file,"%s{\"level\":%u,\"offset\":%llu,\"width\":%u,\"height\":%u}",
          mip?",":"",mip,(unsigned long long)(copies[mip].offset-staging.offset),
          copies[mip].width,copies[mip].height);
    fputs("]}\n",file);fclose(file);
}

void ufc4_probe_draw(uint64_t vs,uint64_t ps,uint32_t indices,uint32_t instances) {
    const char *directory=getenv("BB_UFC4_TEXTURE_PROBE_DIR");
    const char *pipe=getenv("BB_DRAW_PIPE"),*workers=getenv("BB_PREP_WORKERS");
    /* Recording=2. Do not capture merely because the recorder is armed.
     * Serial draw preparation is required: Finish may not run on its helper. */
    if (directory && *directory && pipe && !strcmp(pipe,"0") &&
        workers && !strcmp(workers,"0") && u4_probe_u32(u4_probe_get_state())==2 &&
        u4_probe_target(ps) && instances && u4_probe_done_count<U4_PROBE_MAX_IMAGES) {
        const uint64_t flip=u4_probe_u64(u4_probe_get_flips());
        for(unsigned i=0;i<u4_probe_image_count;++i)
            u4_probe_save(directory,u4_probe_images[i],flip,ps,indices);
    }
    u4_probe_image_count=0;
    u4_probe_draw_original(vs,ps,indices,instances);
}
