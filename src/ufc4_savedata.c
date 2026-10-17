// SPDX-License-Identifier: GPL-2.0-or-later
/* Save diagnostics plus real loading of the icon stored by the original provider. */
extern uintptr_t runtime_savedata_resolve(const char *name);
#if defined(UFC4_DONOR_EXTENSION) && !defined(UFC4_EXTENSION_TEST) && !defined(UFC4_DONOR_V015)
/* The donor's public wrapper was inlined; its unique internal implementation
 * is retained and verified by name by the extension linker. */
extern int runtime_file_translate(const char *guest,char *out,size_t size) __asm__("translate");
#else
extern int runtime_file_translate(const char *guest,char *out,size_t size);
#endif
#include "ufc4_save_space.h"
typedef struct { void *buffer; uint64_t buffer_size,data_size; uint8_t reserved[32]; } U4SaveIcon;
typedef int32_t (GUEST *U4SaveMount)(const void *,void *);
typedef int32_t (GUEST *U4SavePoint)(const void *);
typedef int32_t (GUEST *U4SaveEvent)(const void *,void *);
typedef int32_t (GUEST *U4SaveParam)(const void *,uint32_t,const void *,uint64_t);
static GUEST int32_t u4_save_search(const void *condition,void *result) {
    U4SaveMount original=(U4SaveMount)runtime_savedata_resolve("dyIhnXq-0SM#libSceSaveData");
    int32_t code=original(condition,result);
    if(!code && u4_save_search_space(condition,result))code=(int32_t)0x809f000b;
    return code;
}
static GUEST int32_t u4_save_info(const void *point,void *info) {
    U4SaveMount original=(U4SaveMount)runtime_savedata_resolve("65VH0Qaaz6s#libSceSaveData");
    int32_t code=original(point,info);
    if(!code && u4_save_capacity(point,info))code=(int32_t)0x809f000b;
    printf("UFC4 save: GetMountInfo code=%08x blocks=%llu free=%llu caller=%p\n",(unsigned)code,
        (unsigned long long)(!code ? ((uint64_t *)info)[0] : 0),
        (unsigned long long)(!code ? ((uint64_t *)info)[1] : 0),__builtin_return_address(0));return code;
}
static GUEST int32_t u4_save_umount(const void *point) {
    U4SavePoint original=(U4SavePoint)runtime_savedata_resolve("BMR4F-Uek3E#libSceSaveData");
    int32_t code=original(point);
    printf("UFC4 save: Umount %s returned %08x\n",point ? (const char *)point : "(null)",(unsigned)code);return code;
}
static GUEST int32_t u4_save_load_icon(const void *point,void *output) {
    U4SaveIcon *icon=output;
    if (!point || !icon || !icon->buffer) return (int32_t)0x809f0000;
    /* GetMountInfo validates initialization and membership in the save slots.
     * Translate only that mount root, then read its sibling metadata icon. */
    U4SaveMount mounted=(U4SaveMount)runtime_savedata_resolve("65VH0Qaaz6s#libSceSaveData");
    uint8_t info[48];int32_t code=mounted(point,info);
    if (code) return code;
    if (strnlen((const char *)point,16)>=16) return (int32_t)0x809f0000;
    char host[1024],path[1100];
    if (runtime_file_translate((const char *)point,host,sizeof(host))) return (int32_t)0x809f0004;
    if (snprintf(path,sizeof(path),"%s.sce_sys/icon0.png",host)>=(int)sizeof(path)) return (int32_t)0x809f000b;
    FILE *file=fopen(path,"rb");
    if (!file) return (int32_t)0x809f0008;
    if (fseek(file,0,SEEK_END)) { fclose(file);return (int32_t)0x809f000b; }
    long size=ftell(file);
    if (size<0 || fseek(file,0,SEEK_SET)) { fclose(file);return (int32_t)0x809f000b; }
    uint64_t count=(uint64_t)size<icon->buffer_size ? (uint64_t)size : icon->buffer_size;
    void *copy=malloc(count ? (size_t)count : 1);
    if (!copy) { fclose(file);return (int32_t)0x809f000b; }
    size_t read=fread(copy,1,(size_t)count,file);fclose(file);
    if (read!=(size_t)count) { free(copy);return (int32_t)0x809f000b; }
    memcpy(icon->buffer,copy,(size_t)count);free(copy);
    icon->data_size=(uint64_t)size;
    printf("UFC4 save: LoadIcon read %llu of %ld bytes\n",(unsigned long long)count,size);
    return 0;
}
static GUEST int32_t u4_save_mount(const void *param,void *result) {
    U4SaveMount original=(U4SaveMount)runtime_savedata_resolve("0z45PIH+SNI#libSceSaveData");
    int32_t code=original(param,result);
    int32_t user=0;uint64_t blocks=0;uint32_t mode=0;
    if(param){memcpy(&user,param,4);memcpy(&blocks,(const char *)param+16,8);memcpy(&mode,(const char *)param+24,4);}
    if(!code && result && blocks && blocks<=32768)u4_set_quota(result,blocks);
    printf("UFC4 save: Mount2 user=%d blocks=%llu mode=%x returned %08x\n",user,(unsigned long long)blocks,mode,(unsigned)code);return code;
}
static GUEST int32_t u4_save_umount_backup(const void *point) {
    U4SavePoint original=(U4SavePoint)runtime_savedata_resolve("VwadwBBBJ80#libSceSaveData");
    int32_t code=original(point);
    printf("UFC4 save: UmountWithBackup returned %08x\n",(unsigned)code);return code;
}
static GUEST int32_t u4_save_event(const void *param,void *event) {
    U4SaveEvent original=(U4SaveEvent)runtime_savedata_resolve("j8xKtiFj0SY#libSceSaveData");
    int32_t code=original(param,event);
    static _Atomic unsigned count;
    if (atomic_fetch_add(&count,1)<16) printf("UFC4 save: GetEventResult returned %08x\n",(unsigned)code);
    return code;
}
static GUEST int32_t u4_save_param(const void *point,uint32_t type,const void *data,uint64_t size) {
    U4SaveParam original=(U4SaveParam)runtime_savedata_resolve("85zul--eGXs#libSceSaveData");
    int32_t code=original(point,type,data,size);
    printf("UFC4 save: SetParam type=%u size=%llu returned %08x\n",type,(unsigned long long)size,(unsigned)code);return code;
}
uintptr_t ufc4_savedata_resolve(const char *name) {
    if (!strcmp(name,"dyIhnXq-0SM#libSceSaveData"))return (uintptr_t)u4_save_search;
    if (!strcmp(name,"65VH0Qaaz6s#libSceSaveData"))return (uintptr_t)u4_save_info;
    if (!strcmp(name,"BMR4F-Uek3E#libSceSaveData"))return (uintptr_t)u4_save_umount;
    if (!strcmp(name,"cGjO3wM3V28#libSceSaveData"))return (uintptr_t)u4_save_load_icon;
    if (!strcmp(name,"0z45PIH+SNI#libSceSaveData"))return (uintptr_t)u4_save_mount;
    if (!strcmp(name,"VwadwBBBJ80#libSceSaveData"))return (uintptr_t)u4_save_umount_backup;
    if (!strcmp(name,"j8xKtiFj0SY#libSceSaveData"))return (uintptr_t)u4_save_event;
    if (!strcmp(name,"85zul--eGXs#libSceSaveData"))return (uintptr_t)u4_save_param;
    return runtime_savedata_resolve(name);
}
uintptr_t ufc4_savedata_lookup(const void *table,size_t count,const char *name) {
    (void)table;(void)count;return ufc4_savedata_resolve(name);
}
