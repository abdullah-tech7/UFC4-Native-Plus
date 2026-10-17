// SPDX-License-Identifier: GPL-2.0-or-later
/* The donor's late-label guard mistakes its own earlier fence writes for a
 * rebuilt command buffer. NoteWrite is optional diagnostics, runs BEFORE the
 * guard, and therefore cannot serve as a last-successful-write registry.
 * Remember accepted writes independently, scoped to the exact submission and
 * submitted slot. Unknown values still go through the original corruption
 * guard. This never acknowledges GPU work early or changes the fence value.
 */
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

typedef struct { uint64_t seq,base,bytes,ns; } U4LabelRange;

#ifndef U4_LABEL_TEST
extern uint8_t u4_label_original(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,const uint32_t *)
 __asm__("_ZN9BbClobber10GuardLabelEyyjNS_6SourceEyPKj");
#define U4_LABEL_GLOBAL(function,symbol) \
 static const unsigned char *function(void) { const unsigned char *p; \
  __asm__("leaq " symbol "(%%rip), %0":"=r"(p));return p; }
U4_LABEL_GLOBAL(u4_label_ranges,"_ZN9BbClobber12_GLOBAL__N_16rangesE.llvm.17993348240306159657")
U4_LABEL_GLOBAL(u4_label_head,"_ZN9BbClobber12_GLOBAL__N_111ranges_headE.llvm.17993348240306159657")
#else
extern uint8_t u4_label_original(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,const uint32_t *);
extern U4LabelRange u4_label_test_ranges[256];
extern _Atomic uint64_t u4_label_test_head;
#define u4_label_ranges() ((const unsigned char *)u4_label_test_ranges)
#define u4_label_head() ((const unsigned char *)&u4_label_test_head)
#endif

typedef struct {
 uint64_t address,value,packet,range_seq,range_ns,submitted;
 uint32_t bytes;
} U4OwnLabel;
static U4OwnLabel u4_own_labels[8192];
static atomic_flag u4_label_lock=ATOMIC_FLAG_INIT;
static unsigned u4_label_preserved;

static U4LabelRange u4_label_newest(uint64_t address,uint32_t bytes) {
 U4LabelRange newest={0};
 const uint64_t head=atomic_load_explicit((const _Atomic uint64_t *)u4_label_head(),memory_order_acquire);
 const unsigned count=head<256?(unsigned)head:256;
 for(unsigned i=0;i<count;++i) {
  U4LabelRange range;
  memcpy(&range,u4_label_ranges()+((head-1-i)%256)*sizeof(range),sizeof(range));
  if(range.bytes && address>=range.base && address-range.base<=range.bytes &&
     bytes<=range.bytes-(address-range.base) &&
     (!newest.bytes || range.seq>newest.seq)) newest=range;
 }
 return newest;
}

uint8_t ufc4_gpu_guard_label(uint64_t address,uint64_t value,uint32_t bytes,uint32_t source,
                        uint64_t packet,const uint32_t *at_submit) {
 /* Only the exact, snapshotted in-buffer label can need the correction. The
  * original handles external labels and invalid sizes. Sequence zero is a
  * valid first graphics submission; the redirected callers identify the queue.
  */
 if(!address || address&3 || (bytes!=4 && bytes!=8) || !at_submit)
  return u4_label_original(address,value,bytes,source,packet,at_submit);
 const U4LabelRange range=u4_label_newest(address,bytes);
 if(!range.bytes || range.seq!=packet)
  return u4_label_original(address,value,bytes,source,packet,at_submit);
 uint64_t current=0,submitted=0;
 memcpy(&current,(const void *)(uintptr_t)address,bytes);
 memcpy(&submitted,at_submit,bytes);
 if(bytes==4)value&=UINT32_MAX;
 while(atomic_flag_test_and_set_explicit(&u4_label_lock,memory_order_acquire)){}
 U4OwnLabel *last=&u4_own_labels[((address>>2)^(address>>17))%8192];
 const int ours=last->bytes==bytes && last->address==address &&
  last->packet==packet && last->range_seq==range.seq && last->range_ns==range.ns &&
  last->submitted==submitted && last->value==current;
 /* NoteWrite has already run at this call site. It may contain the upcoming
  * value; only our independent registry can identify the previous accepted one.
  */
 const int blocked=ours?0:u4_label_original(address,value,bytes,source,packet,at_submit);
 if(!blocked) {
  *last=(U4OwnLabel){address,value,packet,range.seq,range.ns,submitted,bytes};
  if(ours && current!=submitted && u4_label_preserved++<4)
   printf("UFC4 GPU labels: preserved completed counter progression %llu -> %llu (submission %llu)\n",
    (unsigned long long)current,(unsigned long long)value,(unsigned long long)packet);
 }
 atomic_flag_clear_explicit(&u4_label_lock,memory_order_release);
 return blocked;
}
