/* Guest threads on host pthreads. Each guest thread owns a FreeBSD-style TCB
 * (variant II: static TLS below the TCB). The loader rewrites the eboot's
 * `mov rax, fs:[0]` into `mov rax, gs:[0]`: on Linux GS base = guest TCB while glibc keeps FS
 * (arch_prctl); on Windows GS stays the TEB and the load's displacement is patched to the TEB
 * TLS slot that holds the TCB (platform_thread_publish_tcb). Priorities/affinity are recorded,
 * not enforced by a PS4 scheduler. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <errno.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_MAGIC UINT32_C(0x41545452)
#define STACK_MARGIN (256*1024)
#define MIN_STACK (64*1024)
#define DEFAULT_STACK (1024*1024)
#define DEFAULT_PRIO 700

typedef void *(ABI *GuestEntry)(void *);
typedef struct ThreadAttr {
    uint32_t magic;
    int detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
    struct ThreadAttr *next;
} ThreadAttr;
typedef struct GuestThread {
    /* Guest-visible TCB is allocated separately; this is the ScePthread handle. */
    uint64_t *tcb;
    unsigned char *tls_block;
    pthread_t host;
    GuestEntry entry;
    void *argument, *result;
    ThreadAttr attr;
    char name[32];
    int detached, finished, joined, host_owned;
    jmp_buf exit_jump;
    struct GuestThread *next;
} GuestThread;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static ThreadAttr *attributes;
static GuestThread *threads;
static _Thread_local GuestThread *current;
static _Thread_local int32_t guest_errno;
static const unsigned char *tls_template;
static uint64_t tls_filesz, tls_memsz, tls_align=16;
static size_t created, joined_count, exited;

void runtime_set_main_tls(const void *data,uint64_t filesz,uint64_t memsz,uint64_t align) {
    tls_template=data; tls_filesz=filesz; tls_memsz=memsz; tls_align=align ? align : 16;
}
static uint64_t tls_offset(void) { return (tls_memsz+tls_align-1)&~(tls_align-1); }
/* Build TCB/static TLS for the calling host thread and publish it as the guest TCB. */
static void attach(GuestThread *t) {
    uint64_t offset=tls_offset();
    size_t total=offset+256;
    unsigned char *block=platform_aligned_alloc(64,(total+63)&~(size_t)63);
    if (!block) { fputs("Cannot allocate guest TLS\n",stderr); exit(1); }
    memset(block,0,total);
    if (tls_filesz) memcpy(block,tls_template,tls_filesz);
    uint64_t *tcb=(uint64_t *)(block+offset);
    static uint64_t dtv[3];
    tcb[0]=(uint64_t)(uintptr_t)tcb;         /* tcb_self */
    tcb[1]=(uint64_t)(uintptr_t)dtv;         /* tcb_dtv (static module only) */
    tcb[2]=(uint64_t)(uintptr_t)t;           /* tcb_thread */
    t->tls_block=block; t->tcb=tcb;
    platform_thread_publish_tcb(tcb);
    current=t;
    runtime_fp_thread_setup(); /* BB_DETERMINISTIC_FP: Orbis' MXCSR and x87 control word */
}
static GuestThread *new_thread(void) {
    GuestThread *t=calloc(1,sizeof(*t));
    if (!t) return NULL;
    t->attr=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.stack=DEFAULT_STACK,.affinity=0x7f};
    return t;
}
static void publish(GuestThread *t) {
    pthread_mutex_lock(&lock); t->next=threads; threads=t; pthread_mutex_unlock(&lock);
}
/* Host threads that call into guest code without being created by the guest
 * (the main thread, test threads) get a record on first use. */
GuestThread *runtime_thread_current(void) {
    if (current) return current;
    GuestThread *t=new_thread();
    if (!t) { fputs("Cannot allocate guest thread\n",stderr); exit(1); }
    t->host=pthread_self(); t->host_owned=1;
    snprintf(t->name,sizeof(t->name),"host");
    attach(t); publish(t);
    return t;
}
/* Host threads that call guest code (HLE decoders invoking guest callbacks) need
 * a TCB/TLS like guest threads; the record lives until process exit. */
void runtime_thread_attach_host(const char *name) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "host");
}
void runtime_thread_attach_main(void) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"main");
}
static GuestThread *find_thread(void *handle) {
    GuestThread *found=NULL;
    pthread_mutex_lock(&lock);
    for (GuestThread *t=threads;t;t=t->next) if (t==handle) { found=t; break; }
    pthread_mutex_unlock(&lock);
    return found;
}
static ThreadAttr *find_attr(ThreadAttr **slot) {
    if (!slot || !*slot) return NULL;
    ThreadAttr *found=NULL;
    pthread_mutex_lock(&lock);
    for (ThreadAttr *a=attributes;a;a=a->next) if (a==*slot && a->magic==ATTR_MAGIC) { found=a; break; }
    pthread_mutex_unlock(&lock);
    return found;
}

static ABI void *thread_self(void) { return runtime_thread_current(); }
static ABI int32_t *guest_error(void) { return &guest_errno; }
int32_t *runtime_errno(void) { return &guest_errno; }

static ABI int32_t attr_init(ThreadAttr **out) {
    if (!out) return ERR(22);
    ThreadAttr *a=calloc(1,sizeof(*a));
    if (!a) return ERR(12);
    *a=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.inherit=4,.stack=DEFAULT_STACK,.guard=4096,.affinity=0x7f};
    pthread_mutex_lock(&lock); a->next=attributes; attributes=a; pthread_mutex_unlock(&lock);
    *out=a; return 0;
}
static ABI int32_t attr_destroy(ThreadAttr **slot) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    pthread_mutex_lock(&lock);
    ThreadAttr **link=&attributes;
    while (*link!=a) link=&(*link)->next;
    *link=a->next; a->magic=0;
    pthread_mutex_unlock(&lock);
    free(a); *slot=NULL; return 0;
}
static ABI int32_t attr_get(void *thread,ThreadAttr **out) {
    ThreadAttr *a=find_attr(out);
    if (!a || !thread) return ERR(22);
    GuestThread *t=find_thread(thread);
    if (!t) return ERR(3);
    ThreadAttr *next=a->next;
    *a=t->attr; a->magic=ATTR_MAGIC; a->next=next;
    a->detached=t->detached;
    return 0;
}
static ABI int32_t attr_set_stack(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || size<16384) return ERR(22);
    a->stack=size; return 0;
}
static ABI int32_t attr_get_stack(ThreadAttr **slot,uint64_t *size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !size) return ERR(22);
    *size=a->stack; return 0;
}
static ABI int32_t attr_set_detach(ThreadAttr **slot,int state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (state!=0 && state!=1)) return ERR(22);
    a->detached=state; return 0;
}
static ABI int32_t attr_get_detach(ThreadAttr **slot,int *state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !state) return ERR(22);
    *state=a->detached; return 0;
}
static ABI int32_t attr_set_policy(ThreadAttr **slot,int policy) {
    ThreadAttr *a=find_attr(slot);
    if (!a || policy<1 || policy>3) return ERR(22);
    a->policy=policy; return 0;
}
static ABI int32_t attr_set_inherit(ThreadAttr **slot,int inherit) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (inherit!=0 && inherit!=4)) return ERR(22);
    a->inherit=inherit; return 0;
}
static ABI int32_t attr_set_param(ThreadAttr **slot,const int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    a->prio=*param; return 0;
}
static ABI int32_t attr_get_param(ThreadAttr **slot,int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    *param=a->prio; return 0;
}
static ABI int32_t attr_set_affinity(ThreadAttr **slot,uint64_t mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->affinity=mask; return 0;
}
static ABI int32_t attr_affinity(ThreadAttr **slot,uint64_t *mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !mask) return ERR(22);
    *mask=a->affinity; return 0;
}
static ABI int32_t attr_set_guard(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->guard=size; return 0;
}

static void set_host_name(const char *name) { platform_thread_set_name(name); }
static void *host_start(void *p) {
    GuestThread *t=p;
    attach(t);
    set_host_name(t->name);
    if (!setjmp(t->exit_jump)) t->result=t->entry(t->argument);
    runtime_kernel_thread_exit();
    runtime_thread_keys_cleanup();
    pthread_mutex_lock(&lock); t->finished=1; ++exited; pthread_mutex_unlock(&lock);
    return t->result;
}
static int32_t create(GuestThread **out,ThreadAttr **attr_slot,GuestEntry entry,void *argument,const char *name) {
    if (!out || !entry) return ERR(22);
    ThreadAttr *a=NULL;
    if (attr_slot && !(a=find_attr(attr_slot))) return ERR(22);
    GuestThread *t=new_thread();
    if (!t) return ERR(12);
    if (a) { t->attr=*a; t->attr.next=NULL; }
    t->entry=entry; t->argument=argument; t->detached=t->attr.detached;
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "guest");
    pthread_attr_t host;
    pthread_attr_init(&host);
    uint64_t stack=t->attr.stack<MIN_STACK ? MIN_STACK : t->attr.stack;
    /* Stacks below 1 TiB as on PS4, fully committed (guest code packs stack addresses and has no
     * stack probes): platform_thread_stack. */
    size_t stack_bytes=(size_t)stack+STACK_MARGIN;
    platform_thread_stack(&host,stack_bytes);
    publish(t);
    /* Publish the handle before the thread can run and inspect itself. */
    *out=t;
    int e=pthread_create(&t->host,&host,host_start,t);
    pthread_attr_destroy(&host);
    if (e) { fprintf(stderr,"STOP: host pthread_create failed: %d\n",e); exit(21); }
    if (t->detached) pthread_detach(t->host);
    pthread_mutex_lock(&lock); ++created; pthread_mutex_unlock(&lock);
    printf("Runtime: guest thread '%s' created (stack=%llu, prio=%d)\n",t->name,(unsigned long long)stack,t->attr.prio);
    return 0;
}
static ABI int32_t thread_create(GuestThread **out,ThreadAttr **attr,GuestEntry entry,void *argument,const char *name) {
    return create(out,attr,entry,argument,name);
}
static ABI int32_t thread_join(GuestThread *t,void **result) {
    if (!find_thread(t) || t->host_owned) return ERR(3);
    if (t==current) return ERR(11);
    if (t->detached || t->joined) return ERR(22);
    t->joined=1;
    void *value=NULL;
    int e=pthread_join(t->host,&value);
    if (e) { fprintf(stderr,"STOP: host pthread_join failed: %d\n",e); exit(21); }
    if (result) *result=t->result;
    pthread_mutex_lock(&lock); ++joined_count; pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t thread_detach(GuestThread *t) {
    if (!find_thread(t)) return ERR(3);
    if (t->detached) return ERR(22);
    t->detached=1;
    if (!t->host_owned) pthread_detach(t->host);
    return 0;
}
static ABI __attribute__((noreturn)) void thread_exit(void *value) {
    GuestThread *t=runtime_thread_current();
    if (t->host_owned) { fputs("STOP: pthread_exit on host-owned/main thread\n",stderr); exit(21); }
    t->result=value;
    /* No host unwinder: guest frames have no registered FDEs. */
    longjmp(t->exit_jump,1);
}
static ABI int32_t thread_yield(void) { sched_yield(); return 0; }
static ABI int32_t thread_get_prio(GuestThread *t,int *prio) {
    if (!find_thread(t)) return ERR(3);
    if (!prio) return ERR(22);
    *prio=t->attr.prio; return 0;
}
static ABI int32_t thread_set_prio(GuestThread *t,int prio) {
    if (!find_thread(t)) return ERR(3);
    t->attr.prio=prio; return 0;
}
static ABI int32_t thread_set_affinity(GuestThread *t,uint64_t mask) {
    if (!find_thread(t)) return ERR(3);
    t->attr.affinity=mask; return 0;
}
static ABI int32_t thread_get_affinity(GuestThread *t,uint64_t *mask) {
    if (!find_thread(t)) return ERR(3);
    if (!mask) return ERR(22);
    *mask=t->attr.affinity; return 0;
}
static ABI int32_t thread_rename(GuestThread *t,const char *name) {
    if (!find_thread(t)) return ERR(3);
    if (!name) return ERR(22);
    snprintf(t->name,sizeof(t->name),"%s",name);
    if (t==current) set_host_name(t->name);
    return 0;
}
static ABI int32_t thread_equal(GuestThread *a,GuestThread *b) { return a==b; }
/* SacredFire: the thread's name into a 32-byte buffer, and its kernel thread id. */
static ABI int32_t thread_getname(GuestThread *t,char *name) {
    if (!find_thread(t)) return ERR(3);
    if (!name) return ERR(22);
    memcpy(name,t->name,sizeof(t->name)); return 0;
}
static ABI int32_t thread_getthreadid(void) { return (int32_t)platform_thread_id(); }
/* Deferred cancellation: a thread without cancellation points keeps running, as on the console; the
 * request is noted so the caller sees success. No guest thread of this game is expected to reach a
 * cancellation point; a cancel that must take effect would need the point implemented. */
static _Atomic unsigned cancels;
static ABI int32_t thread_cancel(GuestThread *t) {
    if (!find_thread(t)) return ERR(3);
    if (atomic_fetch_add_explicit(&cancels,1,memory_order_relaxed)<4) printf("Runtime: scePthreadCancel(%s) noted; deferred cancellation never fires here\n",t->name);
    return 0;
}
/* POSIX scheduling parameters: the policy and priority recorded for the thread (SCHED_OTHER 2,
 * FIFO 1, RR 3 as on FreeBSD); the host scheduler is not changed. */
typedef struct { int32_t priority; } GuestSchedParam;
static ABI int32_t posix_getschedparam(GuestThread *t,int32_t *policy,GuestSchedParam *param) {
    if (!find_thread(t)) return 3;
    if (!policy || !param) return 22;
    *policy=t->attr.policy; param->priority=t->attr.prio; return 0;
}
static ABI int32_t posix_setschedparam(GuestThread *t,int32_t policy,const GuestSchedParam *param) {
    if (!find_thread(t)) return 3;
    if (!param || policy<1 || policy>3) return 22;
    t->attr.policy=policy; t->attr.prio=param->priority; return 0;
}

/* UFC 3: stack address queries (the runtime allocates every stack itself, so a caller-provided
 * address is ignored and its size kept), sce* scheduling parameters and cancel state. */
static ABI int32_t attr_get_stack_full(ThreadAttr **slot,void **addr,uint64_t *size) {
    if (!addr) return ERR(22);
    *addr=NULL; return attr_get_stack(slot,size);
}
static ABI int32_t attr_get_stack_addr(ThreadAttr **slot,void **addr) {
    if (!slot || !*slot || !addr) return ERR(22);
    *addr=NULL; return 0;
}
static ABI int32_t attr_set_stack_full(ThreadAttr **slot,void *addr,uint64_t size) {
    static _Atomic unsigned notes;
    if (addr && atomic_fetch_add_explicit(&notes,1,memory_order_relaxed)<4)
        printf("Runtime: scePthreadAttrSetstack address %p ignored, size %llu kept\n",addr,(unsigned long long)size);
    return attr_set_stack(slot,size);
}
static ABI int32_t sce_getschedparam(GuestThread *t,int32_t *policy,GuestSchedParam *param) {
    int32_t r=posix_getschedparam(t,policy,param); return r ? ERR(r) : 0;
}
static ABI int32_t sce_setschedparam(GuestThread *t,int32_t policy,const GuestSchedParam *param) {
    int32_t r=posix_setschedparam(t,policy,param); return r ? ERR(r) : 0;
}
static ABI int32_t thread_setcancelstate(int state,int *old) {
    if (old) *old=0; /* PTHREAD_CANCEL_ENABLE */
    (void)state; return 0;
}

/* POSIX (libScePosix) variants return positive errno values. */
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_attr_init(ThreadAttr **a) { return posix(attr_init(a)); }
static ABI int32_t posix_attr_destroy(ThreadAttr **a) { return posix(attr_destroy(a)); }
static ABI int32_t posix_attr_set_detach(ThreadAttr **a,int s) { return posix(attr_set_detach(a,s)); }
static ABI int32_t posix_attr_set_stack(ThreadAttr **a,uint64_t s) { return posix(attr_set_stack(a,s)); }
static ABI int32_t posix_attr_set_param(ThreadAttr **a,const int *p) { return posix(attr_set_param(a,p)); }
static ABI int32_t posix_create(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg) { return posix(create(t,a,e,arg,"posix")); }
static ABI int32_t posix_create_name(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg,const char *name) { return posix(create(t,a,e,arg,name)); }
static ABI int32_t posix_join(GuestThread *t,void **r) { return posix(thread_join(t,r)); }

uintptr_t runtime_thread_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"aI+OeCz8xrQ#libkernel",thread_self}, {"EotR8a3ASf4",thread_self},
        {"9BcDykPmo1I#libkernel",guest_error},
        {"nsYoNRywwNg#libkernel",attr_init}, {"62KCwEMmzcM#libkernel",attr_destroy},
        {"x1X76arYMxU#libkernel",attr_get}, {"8+s5BzZjxSg#libkernel",attr_affinity},
        {"UTXzJbWhhTE#libkernel",attr_set_stack}, {"-Wreprtu0Qs#libkernel",attr_set_detach},
        {"4+h9EzwKF4I#libkernel",attr_set_policy}, {"DzES9hQF4f4#libkernel",attr_set_param},
        {"3qxgM4ezETA#libkernel",attr_set_affinity}, {"eXbUSpEaTsA#libkernel",attr_set_inherit},
        {"JaRMy+QcpeU#libkernel",attr_get_detach}, {"-fA+7ZlGDQs#libkernel",attr_get_stack},
        {"FXPWHNk8Of0#libkernel",attr_get_param}, {"El+cQ20DynU#libkernel",attr_set_guard},
        {"6UgtwV+0zb4#libkernel",thread_create}, {"onNY9Byn-W8#libkernel",thread_join},
        {"4qGrR6eoP9Y#libkernel",thread_detach}, {"3kg7rT0NQIs#libkernel",thread_exit},
        {"T72hz6ffq08#libkernel",thread_yield}, {"1tKyG7RlMJo#libkernel",thread_get_prio},
        {"W0Hpm2X0uPE#libkernel",thread_set_prio}, {"bt3CTBKmGyI#libkernel",thread_set_affinity}, {"rcrVFJsQWRY#libkernel",thread_get_affinity},
        {"GBUY7ywdULE#libkernel",thread_rename}, {"3PtV6p3QNX4#libkernel",thread_equal},
        {"wtkt-teR1so",posix_attr_init}, {"zHchY8ft5pk",posix_attr_destroy},
        {"E+tyo3lp5Lw",posix_attr_set_detach}, {"2Q0z6rnBrTE",posix_attr_set_stack},
        {"euKRgm0Vn2M",posix_attr_set_param}, {"OxhIB8LB-PQ",posix_create},
        {"Jmi+9w9u0E4",posix_create_name}, {"h9CcP3J0oVM",posix_join},
        {"FJrT5LuUBAU",thread_exit},
        {"How7B8Oet6k#libkernel",thread_getname}, {"EI-5-jlq2dE#libkernel",thread_getthreadid}, {"qBDmpCyGssE#libkernel",thread_cancel},
        {"FIs3-UQT9sg",posix_getschedparam}, {"Xs9hdiD7sAA",posix_setschedparam},
        {"-quPa4SEJUw#libkernel",attr_get_stack_full}, {"Ru36fiTtJzA#libkernel",attr_get_stack_addr},
        {"Bvn74vj6oLo#libkernel",attr_set_stack_full}, {"P41kTWUS3EI#libkernel",sce_getschedparam},
        {"oIRFTjoILbg#libkernel",sce_setschedparam}, {"OAmWq+OHSjw#libkernel",thread_setcancelstate},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (runtime_nid_equals(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
#ifdef _WIN32
void runtime_thread_host_init(void) { platform_thread_tcb_displacement(); }
#else
void runtime_thread_host_init(void) {}
#endif
void runtime_thread_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: guest threads created=%zu, exited=%zu, joined=%zu\n",created,exited,joined_count);
    pthread_mutex_unlock(&lock);
}
