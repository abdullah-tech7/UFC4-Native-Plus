/* libSceSaveData on host directories:
 *   <user>/savedata/<user id>/<title id>/<dir name>/        files the game writes
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_sys/ param.bin, icon0.png
 *   <user>/savedata/<user id>/<title id>.memory/memory.dat  SaveDataMemory
 * A mounted directory appears to the guest as /savedata0../savedata15.
 * Metadata lives beside the directory so the guest's own files are untouched. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_INTERNAL ((int32_t)0x809F000B)
#define ERR_MOUNT_FULL ((int32_t)0x809F000C)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define ERR_INVALID_USER ((int32_t)0x809F0011)
#define ERR_MEMORY_NOT_READY ((int32_t)0x809F0012)
#define MODE_RDONLY 1
#define MODE_RDWR 2
#define MODE_CREATE 4
#define MODE_COPY_ICON 16
#define MODE_CREATE2 32
#define SLOTS 16

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param; int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} Param;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { int32_t user; int32_t pad; const DirName *dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                 uint8_t reserved[32]; } SearchCond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SearchInfo;
typedef struct { uint32_t hits; int32_t pad; DirName *names; uint32_t names_capacity, set_count;
                 Param *params; SearchInfo *infos; uint8_t reserved[12]; int32_t pad2; } SearchResult;
typedef struct { const void *buffer; uint64_t buffer_size, data_size; uint8_t reserved[32]; } Icon;
_Static_assert(sizeof(Param)==1328,"OrbisSaveDataParam layout");
_Static_assert(sizeof(Mount1)==80 && sizeof(Mount2)==64,"OrbisSaveDataMount layouts");
_Static_assert(sizeof(MountResult)==64,"OrbisSaveDataMountResult layout");
_Static_assert(sizeof(SearchCond)==64 && sizeof(SearchResult)==56,"dir name search layouts");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static char title_id[16]="UNKNOWN";
static struct { int used; char host[700], meta[700]; } slots[SLOTS];
static char memory_path[720];
static uint64_t memory_size;
static size_t mounts_done, memory_writes;

void runtime_savedata_configure(const char *title) {
    if (title && *title) snprintf(title_id,sizeof(title_id),"%s",title);
}

static int make_dirs(const char *path) {
    char buffer[700];
    snprintf(buffer,sizeof(buffer),"%s",path);
    for (char *p=buffer+1;*p;++p) if (*p=='/' || *p=='\\') { char c=*p; *p=0; platform_mkdir(buffer,0755); *p=c; }
    const int e=platform_mkdir(buffer,0755);
    return e && e!=-EEXIST ? -1 : 0;
}
static void root(int32_t user, const char *title, char *out, size_t size) {
    snprintf(out,size,"%s/savedata/%d/%s",runtime_file_user_dir(),user,title && *title ? title : title_id);
}
static int valid_name(const char *name, size_t max) {
    size_t n=strnlen(name,max);
    if (!n || n==max) return 0;
    for (size_t i=0;i<n;++i) if (name[i]=='/' || name[i]=='\\' || (name[i]=='.' && (i==0 || name[i-1]=='.'))) return 0;
    return 1;
}
/* A whole file replaced in one rename (a crash mid-write keeps the old one; bbport 0.5, #64). */
static int write_atomic(const char *path, const void *data, size_t size) {
    char temp[760]; snprintf(temp,sizeof(temp),"%s.bbtmp",path);
    FILE *f=fopen(temp,"wb");
    if (!f) return -1;
    int ok=fwrite(data,1,size,f)==size && !fflush(f) && !platform_fsync(fileno(f));
    ok=!fclose(f) && ok;
    if (!ok || platform_rename(temp,path)) { platform_unlink(temp); return -1; }
    return 0;
}
static void write_param(const char *meta, const Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    Param copy=*p; copy.mtime=time(NULL);
    write_atomic(path,&copy,sizeof(copy));
}
static int read_param(const char *meta, Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    memset(p,0,sizeof(*p));
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    size_t n=fread(p,sizeof(*p),1,f); fclose(f);
    PlatformStat st;
    if (!platform_stat(path,&st)) p->mtime=st.mtim.tv_sec;
    return n==1 ? 0 : -1;
}
/* Removes a save directory: only inside this title's save root (platform_remove_tree refuses
 * anything else), so a bad directory name can never reach the user's other files. */
static void remove_save_tree(const char *base, const char *path) {
    if (platform_remove_tree(base,path)) fprintf(stderr,"Runtime: save data %s not removed (outside %s or in use)\n",path,base);
}

static ABI int32_t save_initialize(const void *param) { (void)param; initialized=1; return 0; }
static ABI int32_t save_terminate(void) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    initialized=0; return 0;
}
static int32_t mount_impl(int32_t user, const char *title, const DirName *dir, uint32_t mode, MountResult *result);
/* ufc3: every failed save-data call is logged (career saves failed with no trace). */
static int32_t logged(const char *what, const char *name, int32_t r) {
    if (r) printf("Runtime: %s%s%s failed: 0x%08X\n", what, name ? " " : "", name ? name : "", (uint32_t)r);
    return r;
}
static int32_t mount(int32_t user, const char *title, const DirName *dir, uint32_t mode, MountResult *result) {
    char name[64]; snprintf(name,sizeof(name),"'%.32s' mode 0x%x",dir ? dir->data : "?",mode);
    return logged("save mount",name,mount_impl(user,title,dir,mode,result));
}
static int32_t mount_impl(int32_t user, const char *title, const DirName *dir, uint32_t mode, MountResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<0) return ERR_INVALID_USER;
    if (!dir || !result || !valid_name(dir->data,sizeof(dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(user,title,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir->data);
    PlatformStat st;
    int exists=!platform_stat(host,&st) && st.is_dir;
    if ((mode & MODE_CREATE) && exists) return ERR_EXISTS;
    /* ufc3: the career save (CAREERSAVE0) is first mounted read-write without a create flag
     * when the game saves; on the console the save was created earlier (career start), which
     * never happens here, so the game showed "An error has occurred while saving". A read-write
     * mount of a missing save creates it; read-only probes still find nothing. */
    if (!exists && (mode & MODE_RDWR) && !(mode & (MODE_CREATE|MODE_CREATE2))) {
        printf("Runtime: save data '%s' missing for a read-write mount: created\n",dir->data);
        mode |= MODE_CREATE2;
    }
    if (!(mode & (MODE_CREATE|MODE_CREATE2)) && !exists) return ERR_NOT_FOUND;
    pthread_mutex_lock(&lock);
    int slot=-1;
    for (int i=0;i<SLOTS;++i) {
        if (slots[i].used && !strcmp(slots[i].host,host)) { pthread_mutex_unlock(&lock); return ERR_BAD_MOUNTED; }
        if (!slots[i].used && slot<0) slot=i;
    }
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_MOUNT_FULL; }
    if (!exists && (make_dirs(host) || make_dirs(meta))) { pthread_mutex_unlock(&lock); return ERR_INTERNAL; }
    if (!exists) { Param empty={0}; write_param(meta,&empty); }
    slots[slot].used=1;
    snprintf(slots[slot].host,sizeof(slots[slot].host),"%s",host);
    snprintf(slots[slot].meta,sizeof(slots[slot].meta),"%s",meta);
    memset(result,0,sizeof(*result));
    char point[32];
    snprintf(point,sizeof(point),"/savedata%d",slot&15);
    memcpy(result->point.data,point,strlen(point)+1);
    result->status=exists ? 0 : 1; /* CREATED */
    runtime_file_mount(result->point.data,host);
    ++mounts_done;
    pthread_mutex_unlock(&lock);
    printf("Runtime: save data '%s' mounted at %s (%s%s)\n",dir->data,result->point.data,
           exists ? "existing" : "created",(mode & MODE_RDONLY) ? ", read-only" : "");
    return 0;
}
static ABI int32_t save_mount(const Mount1 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount(m->user,m->title ? m->title->data : NULL,m->dir,m->mode,result);
}
static ABI int32_t save_mount2(const Mount2 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount(m->user,NULL,m->dir,m->mode,result);
}
static int slot_of(const MountPoint *point) {
    if (!point || strncmp(point->data,"/savedata",9)) return -1;
    int slot=atoi(point->data+9);
    return slot>=0 && slot<SLOTS && slots[slot].used ? slot : -1;
}
static ABI int32_t save_umount(const MountPoint *point) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot>=0) { runtime_file_unmount(point->data); slots[slot].used=0; }
    pthread_mutex_unlock(&lock);
    return slot>=0 ? 0 : ERR_NOT_FOUND;
}
/* UFC 3: SceSaveDataMountInfo {blocks, freeBlocks, reserved[32]}; 32 KiB blocks, 1 GiB per save. */
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } MountInfo;
static ABI int32_t save_mount_info(const MountPoint *point, MountInfo *info) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!info) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    pthread_mutex_unlock(&lock);
    if (slot<0) return ERR_NOT_MOUNTED;
    memset(info,0,sizeof(*info)); info->blocks=32768; info->free_blocks=32000;
    return 0;
}
/* No backups, icons or transfer data exist: the queries find nothing; an unmount with backup is
 * a plain unmount. */
static ABI int32_t save_not_found(void) { return initialized ? ERR_NOT_FOUND : ERR_NOT_INITIALIZED; }
#define NOT_FOUND_LOGGED(tag, fname) \
    static ABI int32_t tag(void) { return logged(fname, NULL, save_not_found()); }
NOT_FOUND_LOGGED(save_event_result, "sceSaveDataGetEventResult")
NOT_FOUND_LOGGED(save_transferring, "sceSaveDataTransferringMount")
NOT_FOUND_LOGGED(save_check_backup, "sceSaveDataCheckBackupData")
NOT_FOUND_LOGGED(save_restore_backup, "sceSaveDataRestoreBackupData")
static ABI int32_t save_umount_backup(const MountPoint *point) { return save_umount(point); }
static ABI int32_t save_set_param(const MountPoint *point, uint32_t type, const void *buffer, uint64_t size) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    Param p; read_param(slots[slot].meta,&p);
    switch (type) {
    case 0: if (size<sizeof(Param)) goto bad; memcpy(&p,buffer,sizeof(p)); break;     /* ALL */
    case 1: snprintf(p.title,sizeof(p.title),"%.*s",(int)size,(const char *)buffer); break;
    case 2: snprintf(p.subtitle,sizeof(p.subtitle),"%.*s",(int)size,(const char *)buffer); break;
    case 3: snprintf(p.detail,sizeof(p.detail),"%.*s",(int)size,(const char *)buffer); break;
    case 4: if (size<4) goto bad; memcpy(&p.user_param,buffer,4); break;
    default: goto bad;
    }
    write_param(slots[slot].meta,&p);
    pthread_mutex_unlock(&lock);
    return 0;
bad:
    pthread_mutex_unlock(&lock);
    return ERR_PARAMETER;
}
static ABI int32_t save_icon(const MountPoint *point, const Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    int32_t r=ERR_NOT_MOUNTED;
    if (slot>=0) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
        r=write_atomic(path,icon->buffer,icon->data_size) ? ERR_INTERNAL : 0;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
/* ufc3: the career save's slot shows its icon; on the console a save always has one (the
 * app's icon0.png unless the game set its own), and a NOT_FOUND here made the game report
 * "Your career data will not be saved due to an error". The save's own icon is returned,
 * else the app's, else a 1x1 PNG. */
static const uint8_t tiny_png[]={0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0,0,0,0x0D,0x49,0x48,0x44,0x52,
    0,0,0,1,0,0,0,1,8,6,0,0,0,0x1F,0x15,0xC4,0x89,0,0,0,0x0D,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,
    0xF8,0x0F,0x00,0x01,0x01,0x01,0x00,0x18,0xDD,0x8D,0xB0,0,0,0,0,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82};
static long read_whole(const char *path, void *buffer, uint64_t capacity) {
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    size_t n=fread(buffer,1,capacity,f); fclose(f);
    return (long)n;
}
static ABI int32_t save_load_icon_impl(const MountPoint *point, Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    char path[700]="";
    if (slot>=0) snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
    pthread_mutex_unlock(&lock);
    if (slot<0) return ERR_NOT_MOUNTED;
    long n=read_whole(path,(void *)icon->buffer,icon->buffer_size);
    if (n<=0 && !runtime_file_translate("/app0/sce_sys/icon0.png",path,sizeof(path)))
        n=read_whole(path,(void *)icon->buffer,icon->buffer_size);
    if (n<=0) {
        n=sizeof(tiny_png)<icon->buffer_size ? (long)sizeof(tiny_png) : (long)icon->buffer_size;
        memcpy((void *)icon->buffer,tiny_png,(size_t)n);
    }
    icon->data_size=(uint64_t)n;
    return 0;
}
static ABI int32_t save_load_icon(const MountPoint *p, Icon *i) { return logged("sceSaveDataLoadIcon",p?p->data:NULL,save_load_icon_impl(p,i)); }
static ABI int32_t save_delete(const Delete *d) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!d || !d->dir || !valid_name(d->dir->data,sizeof(d->dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(d->user,d->title ? d->title->data : NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,d->dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,d->dir->data);
    PlatformStat st;
    if (platform_stat(host,&st)) return ERR_NOT_FOUND;
    remove_save_tree(base,host);
    remove_save_tree(base,meta);
    printf("Runtime: save data '%s' deleted\n",d->dir->data);
    return 0;
}
/* SQL-LIKE pattern: % any run, _ one character. */
static int like(const char *s, const char *p) {
    if (!*p) return !*s;
    if (*p=='%') { for (;;++s) { if (like(s,p+1)) return 1; if (!*s) return 0; } }
    return *s && (*p=='_' || *p==*s) && like(s+1,p+1);
}
typedef struct { char name[32]; Param param; } Entry;
static uint32_t sort_key, sort_order;
static int compare(const void *a, const void *b) {
    const Entry *x=a, *y=b;
    int c = sort_key==1 ? (x->param.user_param>y->param.user_param)-(x->param.user_param<y->param.user_param)
          : sort_key==3 ? (x->param.mtime>y->param.mtime)-(x->param.mtime<y->param.mtime)
          : strcmp(x->name,y->name);
    return sort_order ? -c : c;
}
static ABI int32_t save_search(const SearchCond *cond, SearchResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!cond || !result) return ERR_PARAMETER;
    char base[600];
    root(cond->user,cond->title ? cond->title->data : NULL,base,sizeof(base));
    Entry *entries=NULL; size_t count=0, capacity=0;
    PlatformListing *d=platform_list_directory(base);
    for (size_t i=0; d && i<d->count; ++i) {
        const char *name=d->names+d->offsets[i];
        size_t n=strlen(name);
        if (name[0]=='.' || n>=32 || (n>8 && !strcmp(name+n-8,".sce_sys"))) continue;
        if (cond->dir && cond->dir->data[0] && !like(name,cond->dir->data)) continue;
        if (count==capacity) { capacity=capacity ? capacity*2 : 16; entries=realloc(entries,capacity*sizeof(*entries)); if (!entries) exit(1); }
        snprintf(entries[count].name,32,"%s",name);
        char meta[680]; snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,name);
        read_param(meta,&entries[count].param);
        ++count;
    }
    platform_free_listing(d);
    sort_key=cond->key; sort_order=cond->order;
    if (count) qsort(entries,count,sizeof(*entries),compare);
    result->hits=(uint32_t)count;
    uint32_t set=count<result->names_capacity ? (uint32_t)count : result->names_capacity;
    for (uint32_t i=0;i<set;++i) {
        if (result->names) memcpy(result->names[i].data,entries[i].name,32);
        if (result->params) result->params[i]=entries[i].param;
        if (result->infos) { memset(&result->infos[i],0,sizeof(SearchInfo)); result->infos[i].blocks=32768; result->infos[i].free_blocks=16384; }
    }
    result->set_count=set;
    free(entries);
    return 0;
}
/* SaveDataMemory: one fixed-size blob per user/title. */
static ABI int32_t memory_setup(int32_t user, uint64_t size, const Param *param) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!size) return ERR_PARAMETER;
    char base[600], dir[640];
    root(user,NULL,base,sizeof(base));
    snprintf(dir,sizeof(dir),"%s.memory",base);
    if (make_dirs(dir)) return ERR_INTERNAL;
    pthread_mutex_lock(&lock);
    snprintf(memory_path,sizeof(memory_path),"%s/memory.dat",dir);
    FILE *f=fopen(memory_path,"r+b");
    if (!f) f=fopen(memory_path,"w+b");
    int32_t r=ERR_INTERNAL;
    if (f) {
        fseek(f,0,SEEK_END);
        if ((uint64_t)ftell(f)<size && platform_ftruncate(fileno(f),(int64_t)size)) goto done;
        memory_size=size; r=0;
    done:
        fclose(f);
    }
    if (!r && param) write_param(dir,param);
    pthread_mutex_unlock(&lock);
    if (!r) printf("Runtime: save data memory ready (%llu bytes)\n",(unsigned long long)size);
    return r;
}
static int32_t memory_io(void *buffer, uint64_t size, int64_t offset, int write) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer || offset<0) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    if (!memory_size) { pthread_mutex_unlock(&lock); return ERR_MEMORY_NOT_READY; }
    if ((uint64_t)offset+size>memory_size) { pthread_mutex_unlock(&lock); return ERR_PARAMETER; }
    FILE *f=fopen(memory_path,"r+b");
    int32_t r=ERR_INTERNAL;
    if (f && !fseek(f,offset,SEEK_SET) &&
        (write ? fwrite(buffer,1,size,f) : fread(buffer,1,size,f))==size) r=0;
    if (f) fclose(f);
    if (!r && write) ++memory_writes;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t memory_get(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(buffer,size,offset,0); }
static ABI int32_t memory_set(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(buffer,size,offset,1); }

/* ufc3: logged entry points (failures only). */
static ABI int32_t l_umount(const MountPoint *p) { return logged("sceSaveDataUmount",p?p->data:NULL,save_umount(p)); }
static ABI int32_t l_set_param(const MountPoint *p, uint32_t t, const void *b, uint64_t n) { return logged("sceSaveDataSetParam",p?p->data:NULL,save_set_param(p,t,b,n)); }
static ABI int32_t l_icon(const MountPoint *p, const Icon *i) { return logged("sceSaveDataSaveIcon",p?p->data:NULL,save_icon(p,i)); }
static ABI int32_t l_delete(const Delete *d) { return logged("sceSaveDataDelete",d&&d->dir?d->dir->data:NULL,save_delete(d)); }
static ABI int32_t l_search(const SearchCond *c, SearchResult *r) { return logged("sceSaveDataDirNameSearch",NULL,save_search(c,r)); }
static ABI int32_t l_mount_info(const MountPoint *p, MountInfo *i) { return logged("sceSaveDataGetMountInfo",p?p->data:NULL,save_mount_info(p,i)); }
static ABI int32_t l_memory_setup(int32_t u, uint64_t n, const Param *pa) { return logged("sceSaveDataSetupSaveDataMemory",NULL,memory_setup(u,n,pa)); }
static ABI int32_t l_memory_get(int32_t u, void *b, uint64_t n, int64_t o) { return logged("sceSaveDataGetSaveDataMemory",NULL,memory_get(u,b,n,o)); }
static ABI int32_t l_memory_set(int32_t u, void *b, uint64_t n, int64_t o) { return logged("sceSaveDataSetSaveDataMemory",NULL,memory_set(u,b,n,o)); }
static const RuntimeExport exports[]={
    {"sceSaveDataInitialize",save_initialize}, {"sceSaveDataInitialize2",save_initialize},
    {"sceSaveDataInitialize3",save_initialize}, {"sceSaveDataTerminate",save_terminate},
    {"sceSaveDataMount",save_mount}, {"sceSaveDataMount2",save_mount2}, {"sceSaveDataUmount",l_umount},
    {"sceSaveDataSetParam",l_set_param}, {"sceSaveDataSaveIcon",l_icon}, {"sceSaveDataDelete",l_delete},
    {"sceSaveDataDirNameSearch",l_search},
    {"sceSaveDataGetMountInfo",l_mount_info}, {"sceSaveDataUmountWithBackup",save_umount_backup},
    {"sceSaveDataLoadIcon",save_load_icon}, {"sceSaveDataGetEventResult",save_event_result},
    {"sceSaveDataTransferringMount",save_transferring}, {"sceSaveDataCheckBackupData",save_check_backup},
    {"sceSaveDataRestoreBackupData",save_restore_backup},
    {"sceSaveDataSetupSaveDataMemory",l_memory_setup}, {"sceSaveDataGetSaveDataMemory",l_memory_get},
    {"sceSaveDataSetSaveDataMemory",l_memory_set},
};
uintptr_t runtime_savedata_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_savedata_report(void) { printf("Runtime: save data mounts=%zu, memory writes=%zu\n",mounts_done,memory_writes); }
