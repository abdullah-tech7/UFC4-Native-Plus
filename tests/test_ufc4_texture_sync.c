// SPDX-License-Identifier: GPL-2.0-or-later
/* CPU-only regression tests: no Vulkan, game process, assets, or saves. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define U4_TEXTURE_TEST 1
#include "../src/ufc4_texture_sync.c"

static unsigned locked,unlocked;
static void *mutex_address;
typedef struct { unsigned dirty;int memory[2],gpu[2]; } CopyImage;
static void *copy_cache,*copy_runtime;
static CopyImage *copy_source,*copy_destination;
static unsigned copy_step;
void u4_texture_lock(void *mutex) { assert(mutex==mutex_address);assert(locked==unlocked);++locked; }
void u4_texture_unlock(void *mutex) { assert(mutex==mutex_address);assert(locked==unlocked+1);++unlocked; }
void u4_texture_refresh(void *cache,void *object) {
    assert(cache==copy_cache&&locked==unlocked+1);
    assert(object==(copy_step==0 ? copy_source : copy_destination));
    assert(copy_step<2);++copy_step;
    CopyImage *image=object;
    if (image->dirty) { memcpy(image->gpu,image->memory,sizeof(image->gpu));image->dirty=0; }
}
void u4_texture_copy(void *runtime,void *source,void *destination) {
    assert(runtime==copy_runtime&&source==copy_source&&destination==copy_destination&&copy_step==2);
    ++copy_step;
    copy_destination->gpu[0]=copy_source->gpu[0]; // expanded image has another layer
}
void u4_texture_copy_mip(void *runtime,void *source,void *destination,uint32_t mip,uint32_t slice) {
    assert(runtime==copy_runtime&&source==copy_source&&destination==copy_destination&&copy_step==2);
    assert(mip==7&&slice==3);++copy_step;
    copy_destination->gpu[1]=copy_source->gpu[0];
}
static void ptr(unsigned char *p,uintptr_t value) { memcpy(p,&value,8); }
static void word(unsigned char *p,uint32_t value) { memcpy(p,&value,4); }
static uint32_t flags(unsigned char *images,unsigned id) {
    uint32_t value;memcpy(&value,images+id*0x2d8,4);return value;
}
static void image(unsigned char *images,unsigned id,uint64_t start,uint32_t size,uint32_t flag) {
    unsigned char *p=images+id*0x2d8;word(p,flag);ptr(p+0x1c8,start);word(p+0x1d0,size);
}
static void page(unsigned char *table,unsigned index,uint32_t *ids,size_t count) {
    ptr(table+index*0x58,(uintptr_t)ids);ptr(table+index*0x58+8,count);
}
int main(void) {
    unsigned char cache[0x600]={0};
    unsigned char *first=calloc(1024,8),*table=calloc(1024,0x58),*next=calloc(1024,0x58);
    unsigned char images[8*0x2d8]={0};
    assert(first&&table&&next);
    ptr(cache+0x158,(uintptr_t)images);ptr(cache+0x5a0,(uintptr_t)first);mutex_address=cache+0x5e0;
    const uint64_t base=UINT64_C(3)<<30;
    ptr(first+3*8,(uintptr_t)table);ptr(first+4*8,(uintptr_t)next);
    uint32_t aliases[]={0,1,2,3,4};
    page(table,0,aliases,5);page(table,1,aliases,5);
    image(images,0,base+0x100,0x200,0x48); // image contains a write starting inside it
    image(images,1,base+0x200,0x80,0x40);  // overlapping subresource/alias
    image(images,2,base+0x400,0x80,0x40);  // disjoint, same page
    image(images,3,base+0x0ffff0,0x80,0x40); // crosses a page boundary
    image(images,4,base+0x100040,0x80,0x40); // begins in the next page
    ufc4_texture_invalidate_gpu(cache,base+0x220,0x10);
    assert(flags(images,0)==0x4c&&flags(images,1)==0x44&&flags(images,2)==0x40);
    image(images,0,base+0x100,0x200,0x48);image(images,1,base+0x200,0x80,0x40);
    ufc4_texture_invalidate_gpu(cache,base+0x280,0x80);
    assert(flags(images,0)==0x4c&&flags(images,1)==0x40); // half-open ends
    ufc4_texture_invalidate_gpu(cache,base+0x0ffff8,0x80);
    assert(flags(images,3)==0x44&&flags(images,4)==0x44); // full range, both pages
    assert(flags(images,2)==0x40); // no unrelated invalidation
    uint32_t boundary[]={5};page(table,1023,boundary,1);page(next,0,boundary,1);
    image(images,5,(UINT64_C(4)<<30)-8,32,0x41);
    ufc4_texture_invalidate_gpu(cache,(UINT64_C(4)<<30)-4,16);
    assert(flags(images,5)==0x45); // spans first-level page table entries, retains MaybeCpuDirty
    unsigned calls=locked;
    ufc4_texture_invalidate_gpu(cache,base,0);
    ufc4_texture_invalidate_gpu(cache,UINT64_C(1)<<40,8);
    assert(calls==locked); // no zero-size underflow or out-of-bounds page lookup
    ptr(first+1023*8,(uintptr_t)next);uint32_t last[]={6};page(next,1023,last,1);
    image(images,6,(UINT64_C(1)<<40)-16,16,0x40);
    ufc4_texture_invalidate_gpu(cache,(UINT64_C(1)<<40)-8,UINT64_MAX);
    assert(flags(images,6)==0x44); // overflow clamped at the guest address limit
    ufc4_texture_invalidate_gpu(cache,UINT64_C(20)<<30,0x1000); // absent page table
    assert(locked==unlocked);
    CopyImage source={.dirty=2,.memory={23,24},.gpu={1,2}};
    CopyImage destination={.dirty=2,.memory={11,77},.gpu={0,0}};
    copy_cache=cache;copy_runtime=&copy_step;copy_source=&source;copy_destination=&destination;
    ptr(cache+0x10,(uintptr_t)copy_runtime);
    u4_texture_lock(mutex_address);
    ufc4_texture_expand_copy(cache,&source,&destination);
    u4_texture_unlock(mutex_address);
    assert(copy_step==3&&destination.gpu[0]==23&&destination.gpu[1]==77);
    // A clean GPU-authored source must survive even if its CPU memory is old.
    source.dirty=0;source.memory[0]=-1;source.gpu[0]=99;
    destination.dirty=1;destination.memory[0]=45;destination.memory[1]=46;copy_step=0;
    u4_texture_lock(mutex_address);
    ufc4_texture_merge_mip(cache,&source,&destination,7,3);
    u4_texture_unlock(mutex_address);
    assert(copy_step==3&&destination.gpu[0]==45&&destination.gpu[1]==99);
    assert(locked==unlocked);
    free(first);free(table);free(next);
    puts("PASS: GPU writes inside images, aliases/mips, full ranges, page boundaries, unchanged neighbors, dirty flags, zero and overflow");
    puts("PASS: refresh source and destination before image expansion/mip merge; preserve untouched layers and GPU-authored content; forward mip/slice correctly");
    return 0;
}
