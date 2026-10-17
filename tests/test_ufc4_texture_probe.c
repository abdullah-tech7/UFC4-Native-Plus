// SPDX-License-Identifier: GPL-2.0-or-later
/* Isolated capture contract with fake GPU helpers, never starts Vulkan/game. */
#define U4_PROBE_TEST 1
#include <assert.h>
#include "../src/ufc4_texture_probe.c"

unsigned char u4_probe_pending[24],u4_probe_flips[8],u4_probe_state[4];
static unsigned char image[0x210],host_runtime[0x80],staging_bytes[8*1024*1024];
static unsigned sampled_calls,draw_calls,request_calls,download_calls,finish_calls,invalidate_calls;
static uint64_t expected_total;
static unsigned phase;
static void put32(void *p,uint32_t n){memcpy(p,&n,4);}
static void put64(void *p,uint64_t n){memcpy(p,&n,8);}
static void putptr(void *p,void *n){memcpy(p,&n,sizeof(n));}

U4ProbeStaging u4_probe_request(void *pool,uint64_t bytes,uint8_t type,uint64_t align,uint8_t deferred) {
    assert(pool==host_runtime+0x30 && type==2 && align==16 && !deferred);
    assert(phase==0 && bytes==expected_total);phase=1;++request_calls;
    return (U4ProbeStaging){.buffer=staging_bytes,.offset=32,.size=bytes,.mapped=staging_bytes+32,.type=2};
}
void u4_probe_download(void *runtime,void *selected,const void *buffer,U4ProbeSpan copies) {
    assert(runtime==host_runtime && selected==image && buffer==staging_bytes);
    assert(phase==1 && copies.count==3);phase=2;++download_calls;
    uint64_t at=32;
    for(unsigned i=0;i<copies.count;++i) {
        const U4ProbeCopy *c=copies.data+i;
        assert(c->offset==at && c->mip==i && c->aspect==1 && c->layers==1);
        assert(!c->row_length && !c->image_height && !c->base_layer && !c->x && !c->y && !c->z);
        assert(c->width==(1024u>>i) && c->height==(512u>>i) && c->depth==1);
        uint64_t size=(uint64_t)((c->width+3)/4)*((c->height+3)/4)*16;
        memset(staging_bytes+at,(int)i+1,(size_t)size);at+=size;
    }
    assert(at==32+expected_total);
}
void u4_probe_finish(void *scheduler) {
    assert(scheduler==image && phase==2);phase=3;++finish_calls;
}
void u4_probe_invalidate(void *buffer,uint64_t offset,uint64_t size) {
    assert(buffer==staging_bytes && offset==32 && size==expected_total && phase==3);
    phase=0;++invalidate_calls;
}
void u4_probe_sampled_original(const void *info,uint8_t storage) {
    assert(info==image+0x68);(void)storage;++sampled_calls;
    put64(u4_probe_pending+8,u4_probe_u64(u4_probe_pending+8)+32);
}
void u4_probe_draw_original(uint64_t vs,uint64_t ps,uint32_t indices,uint32_t instances) {
    assert(vs==0xdb607fb1 && indices==20220);(void)ps;(void)instances;++draw_calls;
    put64(u4_probe_pending+8,u4_probe_u64(u4_probe_pending));
}
static void draw(uint64_t ps,uint8_t storage,uint32_t instances) {
    ufc4_probe_sampled(image+0x68,storage);
    ufc4_probe_draw(0xdb607fb1,ps,20220,instances);
}
int main(int argc,char **argv) {
    assert(argc==2);
    put64(u4_probe_pending,0x1234);put64(u4_probe_pending+8,0x1234);
    put32(u4_probe_state,2);put64(u4_probe_flips,50);
    put64(image+0x60,7);put32(image+0x8c,138);put32(image+0x98,3);put32(image+0x9c,1);
    put32(image+0xa0,1024);put32(image+0xa4,512);put32(image+0xa8,1);put32(image+0xb0,1);
    put64(image+0x1c8,UINT64_C(0x1018b20600));putptr(image+0x1e8,host_runtime);
    putptr(host_runtime+0x20,image);expected_total=524288+131072+32768;
    _putenv_s("BB_UFC4_TEXTURE_PROBE_DIR","");draw(0x7388baf7,0,1);assert(!request_calls);
    _putenv_s("BB_UFC4_TEXTURE_PROBE_DIR",argv[1]);
    _putenv_s("BB_DRAW_PIPE","1");_putenv_s("BB_PREP_WORKERS","0");
    draw(0x7388baf7,0,1);assert(!request_calls);
    _putenv_s("BB_DRAW_PIPE","0");
    _putenv_s("BB_UFC4_TEXTURE_PROBE_PS","7388baf7");
    draw(0x7f438824,0,1);assert(!request_calls); /* CAF must not spend NPC budget */
    draw(0x7388baf7,1,1);draw(0x11111111,0,1);draw(0x7388baf7,0,0);assert(!request_calls);
    put32(u4_probe_state,1);draw(0x7388baf7,0,1);assert(!request_calls);put32(u4_probe_state,2);
    put32(image,4);draw(0x7388baf7,0,1);assert(!request_calls);put32(image,0x48);
    put32(image+0xb0,2);draw(0x7388baf7,0,1);assert(!request_calls);put32(image+0xb0,1);
    draw(0x7388baf7,0,1);assert(request_calls==1 && download_calls==1 && finish_calls==1 && invalidate_calls==1);
    draw(0x7388baf7,0,1);assert(request_calls==1); /* UID/frame dedup */
    put64(u4_probe_flips,51);draw(0x7388baf7,0,1);assert(request_calls==2);
    u4_probe_bytes=128*UINT64_C(1024)*1024;put64(u4_probe_flips,52);
    draw(0x7388baf7,0,1);assert(request_calls==2);u4_probe_bytes=0;
    u4_probe_done_count=U4_PROBE_MAX_IMAGES;draw(0x7388baf7,0,1);assert(request_calls==2);
    assert(sampled_calls==draw_calls && u4_probe_image_count==0);
    puts("PASS: GPU capture ordering, mip offsets, serial/recording/format guards, dedup, byte/image limits; original recorder preserved; no game or Vulkan started");
    return 0;
}
