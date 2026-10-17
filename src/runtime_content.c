/* Offline AppContent provider for the configured base-game profile.
 * No package/DLC mounting, downloads, entitlement or license emulation. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
/* SacredFire: sceSysmoduleLoadModuleInternalWithArg: internal module ids are 0x8000xxxx; the
 * module's start arguments are ignored and the module's start result is 0. */
static ABI int32_t module_load_internal(uint32_t id,uint64_t argc,const void *argv,uint32_t flags,const void *option,int32_t *result) {
    (void)argc; (void)argv; (void)flags; (void)option;
    if ((id&0xffff0000)!=0x80000000) return INVALID_ID;
    int32_t r=module_load((uint16_t)id);
    if (!r && result) *result=0;
    return r;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    puts("Runtime: AppContent initialized; offline base-game profile, no mounted add-ons");
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    if ((!capacity || !list) && !hits) return PARAMETER;
    /* No add-ons are mounted by this provider. Never touch unused list slots. */
    if (hits) *hits=0;
    ++lists; return 0;
}
/* UFC 3: temporary data lives in /temp0 (a writable user directory, see runtime_file.c); no
 * add-ons are mounted, so an add-on mount reports NOT_FOUND. */
static ABI int32_t temp_mount2(uint32_t option,char *mount_point) {
    require_initialized();
    if (!mount_point) return PARAMETER;
    memset(mount_point,0,16); memcpy(mount_point,"/temp0",6);
    printf("Runtime: AppContent temporary data mounted at /temp0 (option %u)\n",option);
    return 0;
}
static ABI int32_t temp_format(const char *mount_point) {
    require_initialized();
    return mount_point ? 0 : PARAMETER;
}
static ABI int32_t temp_space(const char *mount_point,uint64_t *kb) {
    require_initialized();
    if (!mount_point || !kb) return PARAMETER;
    *kb=1024*1024; return 0; /* 1 GiB free */
}
static ABI int32_t addcont_mount(uint32_t service,const void *entitlement,char *mount_point) {
    require_initialized(); (void)service; (void)entitlement; (void)mount_point;
    return (int32_t)0x80D90005; /* SCE_APP_CONTENT_ERROR_NOT_FOUND */
}
static ABI int32_t addcont_unmount(const char *mount_point) { (void)mount_point; return 0; }
uintptr_t runtime_content_resolve(const char *name) {
    if (runtime_nid_equals(name,"buYbeLOGWmA#libSceAppContent")) return (uintptr_t)temp_mount2;
    if (runtime_nid_equals(name,"a5N7lAG0y2Q#libSceAppContent")) return (uintptr_t)temp_format;
    if (runtime_nid_equals(name,"SaKib2Ug0yI#libSceAppContent")) return (uintptr_t)temp_space;
    if (runtime_nid_equals(name,"VANhIWcqYak#libSceAppContent")) return (uintptr_t)addcont_mount;
    if (runtime_nid_equals(name,"3rHWaV-1KC4#libSceAppContent")) return (uintptr_t)addcont_unmount;
    if (runtime_nid_equals(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (runtime_nid_equals(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (runtime_nid_equals(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (runtime_nid_equals(name,"hHrGoGoNf+s#libSceSysmodule")) return (uintptr_t)module_load_internal;
    if (runtime_nid_equals(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (runtime_nid_equals(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (runtime_nid_equals(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
