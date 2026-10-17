/* Guest file system: PS4 mount points mapped onto host directories.
 *   /app0, /hostapp  -> game package root (read-only by convention)
 *   /temp0, /download0, /data, and mounts added by SaveData -> user directory
 * Guest descriptors are small integers in our own table; stdio 0-2 pass through.
 * Paths containing ".." components are rejected rather than normalized. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define MAX_FILES 1024
#define MAX_MOUNTS 16

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    GuestTimespec atime, mtime, ctime;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    GuestTimespec birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat)==120,"FreeBSD stat layout");

typedef PlatformListing Listing;
/* commit: a save file opened for writing is written to a temporary copy (temp), which replaces
 * the file (commit) in one rename when closed: a crash or a kill mid-save leaves the old file. */
typedef struct { int used, host, dirty, save_logged; unsigned t4_logged; Listing *dir; size_t position; char path[512]; char *commit, *temp; } File;
typedef struct { char guest[64]; char host[512]; } Mount;
static File files[MAX_FILES];
static Mount mounts[MAX_MOUNTS];
static size_t mount_count, opens, reads, writes, missing;
static uint64_t bytes_read;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;

/* UFC-13: trace capture production and consumption, alongside GPU [T4] layout diagnostics. */
static int t4_path(const char *p) {
    const char *on=getenv("BB_T4_TRACE");
    return on && on[0]=='1' && p &&
        (!strncmp(p,"/temp0/createplayer",19) || !strncmp(p,"/temp0/images",13));
}

static int save_path(const char *p) { return p && !strncmp(p,"/savedata",9); }
static int temp_name(const char *name) {
    size_t n=strlen(name);
    return n>6 && !strcmp(name+n-6,".bbtmp");
}
/* Copies left by a crash (none of them open): removed when their directory is mounted. */
static void remove_stale_temps(const char *dir,int depth) {
    PlatformListing *d=platform_list_directory(dir);
    if (!d) return;
    for (size_t i=0;i<d->count;++i) {
        const char *name=d->names+d->offsets[i];
        if (name[0]=='.') continue;
        char path[1024];
        if ((size_t)snprintf(path,sizeof(path),"%s/%s",dir,name)>=sizeof(path)) continue;
        if (temp_name(name)) {
            int open_now=0;
            for (int i=3;i<MAX_FILES;++i) if (files[i].used && files[i].temp && !strcmp(files[i].temp,path)) open_now=1;
            if (!open_now && !platform_unlink(path)) printf("Runtime: removed %s (a save interrupted earlier; the file it would have replaced is intact)\n",path);
        } else if (depth<4 && d->types[i]==4) remove_stale_temps(path,depth+1);
    }
    platform_free_listing(d);
}

int runtime_file_mount(const char *guest,const char *host) {
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        snprintf(mounts[i].host,sizeof(mounts[i].host),"%s",host);
        pthread_mutex_unlock(&lock); return 0;
    }
    if (mount_count==MAX_MOUNTS || strlen(guest)>=64 || strlen(host)>=512) { pthread_mutex_unlock(&lock); return -1; }
    snprintf(mounts[mount_count].guest,64,"%s",guest);
    snprintf(mounts[mount_count].host,512,"%s",host);
    ++mount_count;
    if (save_path(guest)) remove_stale_temps(host,0);
    pthread_mutex_unlock(&lock);
    return 0;
}
void runtime_file_unmount(const char *guest) {
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        mounts[i]=mounts[--mount_count]; break;
    }
    pthread_mutex_unlock(&lock);
}
static char user_root[512]="user";
const char *runtime_file_user_dir(void) { return user_root; }
void runtime_file_configure(const char *app0,const char *user) {
    char path[600];
    snprintf(user_root,sizeof(user_root),"%s",user);
    runtime_file_mount("/app0",app0);
    runtime_file_mount("/hostapp",app0);
    const char *writable[]={"temp0","download0","data"};
    platform_mkdir(user,0755);
    for (int i=0;i<3;++i) {
        snprintf(path,sizeof(path),"%s/%s",user,writable[i]);
        platform_mkdir(path,0755);
        char guest[32]; snprintf(guest,sizeof(guest),"/%s",writable[i]);
        runtime_file_mount(guest,path);
    }
    /* ufc3: folders the game reads its fighter portraits and created-player data from
     * (/temp0/images/career, /temp0/images/match, /temp0/createplayer); it only opens them, so
     * missing folders are a separate failure from blank/corrupt capture contents. */
    {
        const char *ufc_dirs[]={"temp0/images","temp0/images/career","temp0/images/match","temp0/createplayer"};
        for (int i=0;i<4;++i) {
            snprintf(path,sizeof(path),"%s/%s",user,ufc_dirs[i]);
            int r=platform_mkdir(path,0755);
            if (t4_path("/temp0/images")) printf("[T4] configure mkdir %s -> %d\n",path,r);
        }
    }
}
/* Resolve a guest path to a host path; returns 0 or a host errno. */
static int translate(const char *guest,char *out,size_t size) {
    if (!guest || !*guest) return ENOENT;
    char buffer[1024];
    if (guest[0]!='/') snprintf(buffer,sizeof(buffer),"/app0/%s",guest);
    else snprintf(buffer,sizeof(buffer),"%s",guest);
    for (const char *p=buffer;(p=strstr(p,".."));p+=2)
        if ((p==buffer || p[-1]=='/') && (p[2]==0 || p[2]=='/')) return EACCES;
    pthread_mutex_lock(&lock);
    size_t best=0; const Mount *m=NULL;
    for (size_t i=0;i<mount_count;++i) {
        size_t n=strlen(mounts[i].guest);
        if (!strncmp(buffer,mounts[i].guest,n) && (buffer[n]=='/' || !buffer[n]) && n>best) { best=n; m=&mounts[i]; }
    }
    int result=0;
    if (!m) result=ENOENT;
    else if ((size_t)snprintf(out,size,"%s%s",m->host,buffer+best)>=size) result=ENAMETOOLONG;
    pthread_mutex_unlock(&lock);
    if (result==ENOENT) fprintf(stderr,"Runtime: no mount for guest path %s\n",guest);
    /* ufc3 (BB_SKIP_MOVIES=1, BB_SKIP_MOVIE_FILE=<clip>): the EA intro and the first-start video
     * (movie/bootflow/, movie/ftue/; BB_SKIP_MOVIES_MATCH: other comma-separated path parts)
     * open a fraction-of-a-second black clip of the same format instead. */
    if (!result) {
        static int skip=-1; static char clip[1024], match[512];
        if (skip<0) {
            const char *on=getenv("BB_SKIP_MOVIES"), *file=getenv("BB_SKIP_MOVIE_FILE"), *m=getenv("BB_SKIP_MOVIES_MATCH");
            skip=on && on[0]=='1' && file && *file;
            if (skip) {
                snprintf(clip,sizeof(clip),"%s",file);
                snprintf(match,sizeof(match),"%s",m && *m ? m : "/movie/bootflow/,/movie/ftue/");
                printf("Runtime: intro and first-start videos skipped (BB_SKIP_MOVIES: %s)\n",match);
            }
        }
        if (skip && strstr(buffer,".mkv")) {
            char parts[512]; snprintf(parts,sizeof(parts),"%s",match);
            for (char *part=parts, *next; part; part=next) {
                next=strchr(part,','); if (next) *next++=0;
                if (*part && strstr(buffer,part)) {
                    if ((size_t)snprintf(out,size,"%s",clip)>=size) result=ENAMETOOLONG;
                    printf("Runtime: video %s skipped\n",buffer);
                    break;
                }
            }
        }
    }
    /* BB_FILE_TRACE=1: every guest path the game opens, stats or checks (mods, missing files). */
    static int trace=-1;
    if (trace<0) { const char *t=getenv("BB_FILE_TRACE"); trace=t && t[0]=='1'; }
    if (trace) printf("File trace: %s -> %s\n",guest,result ? "(no mount)" : out);
    return result;
}
/* The PS4's file system ignores case, and the game asks for lower-case names
 * (adhoc/font/dbgfont14h.ccm): files from mods or dumps made elsewhere keep their own case.
 * After a miss, the host path is matched component by component ignoring case; true when it was
 * corrected (same length: only case differs). Only on misses: no cost for found files. */
static int equal_ignoring_case(const char *a,const char *b) {
    for (;;++a,++b) {
        const unsigned char x=(unsigned char)*a, y=(unsigned char)*b;
        if ((x|0x20)!=(y|0x20) || (x>='A' && x<='Z' ? x+32 : x)!=(y>='A' && y<='Z' ? y+32 : y)) return 0;
        if (!x) return 1;
    }
}
/* A root ("/" or "C:/") stays as it is; the components after it are matched one by one. */
static int fix_case(char *path) {
    const char *p=path;
    char out[1024]="", trial[1024], part[256];
    if (path[0]=='/') ++p;
    else if (((path[0]>='A' && path[0]<='Z') || (path[0]>='a' && path[0]<='z')) && path[1]==':' && (path[2]=='/' || path[2]=='\\')) { snprintf(out,sizeof(out),"%.2s",path); p=path+3; }
    else return 0;
    while (*p) {
        while (*p=='/' || *p=='\\') ++p;
        if (!*p) break;
        size_t len=strcspn(p,"/\\");
        if (len>=sizeof(part)) return 0;
        memcpy(part,p,len); part[len]=0;
        if ((size_t)snprintf(trial,sizeof(trial),"%s/%s",out,part)>=sizeof(trial)) return 0;
        if (platform_access(trial,0)) {
            PlatformListing *d=platform_list_directory(out[0] ? out : "/");
            if (!d) return 0;
            int found=0;
            for (size_t i=0;i<d->count && !found;++i) {
                const char *name=d->names+d->offsets[i];
                if (equal_ignoring_case(name,part)) { snprintf(trial,sizeof(trial),"%s/%s",out,name); found=1; }
            }
            platform_free_listing(d);
            if (!found) return 0;
        }
        memcpy(out,trial,strlen(trial)+1);
        p+=len;
    }
    if (!strcmp(out,path) || strlen(out)!=strlen(path)) return 0;
    memcpy(path,out,strlen(out)+1);
    return 1;
}
static void free_listing(Listing *l) { platform_free_listing(l); }
/* The directory's entries without the save copies in progress (name.N.bbtmp). */
static Listing *list_directory(const char *path) {
    Listing *l=platform_list_directory(path);
    if (!l) return NULL;
    size_t kept=0;
    for (size_t i=0;i<l->count;++i) {
        if (temp_name(l->names+l->offsets[i])) continue;
        l->offsets[kept]=l->offsets[i]; l->types[kept]=l->types[i]; ++kept;
    }
    l->count=kept;
    return l;
}
static void convert_stat(const PlatformStat *s,GuestStat *g) {
    memset(g,0,sizeof(*g));
    g->dev=s->dev; g->ino=s->ino;
    g->mode=(uint16_t)s->mode; g->nlink=(uint16_t)s->nlink;
    g->size=s->size; g->blocks=s->blocks; g->blksize=s->blksize;
    g->atime=(GuestTimespec){s->atim.tv_sec,s->atim.tv_nsec};
    g->mtime=(GuestTimespec){s->mtim.tv_sec,s->mtim.tv_nsec};
    g->ctime=(GuestTimespec){s->ctim.tv_sec,s->ctim.tv_nsec};
    g->birthtime=g->ctime;
}
static File *get(int fd) {
    if (fd<3 || fd>=MAX_FILES || !files[fd].used) return NULL;
    return &files[fd];
}
/* BB_AUDIO_TRACE=1: sound file opens and failed reads (missing game sounds). */
static int audio_trace(void) { static int v=-1; if (v<0) { const char *e=getenv("BB_AUDIO_TRACE"); v=e && e[0]=='1'; } return v; }
/* BB_SAVE_TRACE=1: every operation on save files (/savedataN). */
static int save_trace(void) { static int v=-1; if (v<0) { const char *e=getenv("BB_SAVE_TRACE"); v=e && e[0]=='1'; } return v; }
/* Game mounts (including linked mod overlays) are read-only. Saves use other mounts. */
static int game_path(const char *p) {
    if (!p || !*p) return 0;
    if (*p!='/') return 1;
    return (!strncmp(p,"/app0",5) && (!p[5] || p[5]=='/')) ||
           (!strncmp(p,"/hostapp",8) && (!p[8] || p[8]=='/'));
}
/* Saves (/savedataN): see File. The copy starts as the old file unless the open truncates. */
static unsigned temp_serial;
static int copy_contents(int from,int to) {
    char buffer[65536];
    for (;;) {
        ssize_t n=platform_read(from,buffer,sizeof(buffer));
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return n<0 ? -1 : 0;
        for (ssize_t done=0; done<n;) {
            ssize_t w=platform_write(to,buffer+done,(size_t)(n-done));
            if (w<0 && errno==EINTR) continue;
            if (w<0) return -1;
            done+=w;
        }
    }
}
/* A save file open for writing is its copy to the operations by path too (the game stats the
 * file it is writing, which may not exist yet: it renames the old one to its backup first). */
static void current_copy(char *path,size_t size,int write) {
    pthread_mutex_lock(&lock);
    for (int i=3;i<MAX_FILES;++i)
        if (files[i].used && files[i].commit && !strcmp(files[i].commit,path)) { snprintf(path,size,"%s",files[i].temp); files[i].dirty|=write; break; }
    pthread_mutex_unlock(&lock);
}
/* Returns the descriptor of the copy, or -(errno). Guest flag bits: see platform_open. */
enum { GF_CREAT=0x200, GF_TRUNC=0x400, GF_EXCL=0x800 };
static int open_for_commit(const char *path,const char *source,int flags,int mode,char **commit,char **temp) {
    PlatformStat s;
    int exists=!platform_stat(source,&s);
    if (exists && !s.is_reg) return platform_open(path,flags,mode ? mode : 0644);
    if (exists && (flags&GF_CREAT) && (flags&GF_EXCL)) return -EEXIST;
    if (!exists && !(flags&GF_CREAT)) return -ENOENT;
    size_t n=strlen(path)+32;
    char *t=malloc(n), *c=strdup(path);
    if (!t || !c) { free(t); free(c); return -ENOMEM; }
    snprintf(t,n,"%s.%u.bbtmp",path,__atomic_add_fetch(&temp_serial,1,__ATOMIC_RELAXED));
    int out=platform_open(t,1|GF_CREAT|GF_TRUNC,exists ? (int)(s.mode&07777) : mode ? mode : 0644);
    int e=out<0 ? -out : 0;
    if (!e && exists && !(flags&GF_TRUNC)) {
        int in=platform_open(source,0,0);
        if (in<0 || copy_contents(in,out)) e=in<0 ? -in : (errno ? errno : EIO);
        if (in>=0) platform_close(in);
    }
    if (out>=0) platform_close(out);
    int host=e ? -1 : platform_open(t,flags&~(GF_CREAT|GF_EXCL|GF_TRUNC),0);
    if (host<0) {
        if (!e) e=-host;
        platform_unlink(t); free(t); free(c);
        return -e;
    }
    *commit=c; *temp=t;
    return host;
}
static void sync_directory(const char *file) {
    char dir[1024];
    snprintf(dir,sizeof(dir),"%s",file);
    char *slash=strrchr(dir,'/');
    if (!slash) return;
    *slash=0;
    int d=platform_open(dir,0x20000,0);
    if (d>=0) { platform_fsync(d); platform_close(d); }
}
static int commit_file(const File *f) {
    if (!f->dirty) { platform_close(f->host); platform_unlink(f->temp); return 0; } /* opened for writing, not written */
    int e=-platform_fsync(f->host);
    platform_close(f->host);
    if (!e) e=-platform_rename(f->temp,f->commit);
    if (e) {
        fprintf(stderr,"Runtime: save file %s not replaced (%s); the old one is kept\n",f->path,strerror(e));
        platform_unlink(f->temp);
        return -e;
    }
    sync_directory(f->commit);
    return 0;
}
/* All operations return >=0 or -(host errno); wrappers adapt the convention. */
static int64_t do_open(const char *guest,int flags,int mode) {
    if (game_path(guest) && (flags & (3|0x8|0x200|0x400|0x800))) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    char *commit=NULL, *temp=NULL, current[1024];
    snprintf(current,sizeof(current),"%s",path);
    if (save_path(guest)) current_copy(current,sizeof(current),0);
    int host;
    if (save_path(guest) && (flags&3)) {
        host=open_for_commit(path,current,flags,mode,&commit,&temp);
    } else {
        host=platform_open(current,flags,mode ? mode : 0644);
        if (host==-ENOENT && !(flags&0x200) && !save_path(guest) && fix_case(current)) {
            host=platform_open(current,flags,mode ? mode : 0644);
            snprintf(path,sizeof(path),"%s",current); // the directory listing below uses it
        }
    }
    if (save_trace() && save_path(guest))
        printf("Save trace: open(%s, flags 0x%x) -> host %d%s%s\n",guest,flags,host,temp ? ", copy " : "",temp ? temp : "");
    if (t4_path(guest)) printf("[T4] open %s flags=%#x host=%d\n",guest,flags,host);
    if (host<0) {
        e=-host;
        if (e==ENOENT) { ++missing; printf("Runtime: open(%s) -> not found\n",guest); }
        return -e;
    }
    PlatformStat s;
    Listing *dir=NULL;
    if (!platform_fstat(host,&s) && s.is_dir) dir=list_directory(path);
    pthread_mutex_lock(&lock);
    int fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!files[i].used) { fd=i; break; }
    if (fd<0) {
        pthread_mutex_unlock(&lock); platform_close(host); free_listing(dir);
        if (temp) platform_unlink(temp);
        free(commit); free(temp); return -EMFILE;
    }
    files[fd]=(File){.used=1,.host=host,.dir=dir,.commit=commit,.temp=temp};
    snprintf(files[fd].path,sizeof(files[fd].path),"%s",guest);
    ++opens;
    pthread_mutex_unlock(&lock);
    if (audio_trace() && strstr(guest,"sound/")) printf("Audio trace: open(%s) -> fd %d, %lld bytes\n",guest,fd,(long long)s.size);
    const char *mod_trace=getenv("BB_MOD_TRACE"), *mod_root=getenv("BB_MODS_DIR");
    if (mod_trace && mod_trace[0]=='1' && mod_root) {
        char actual[PATH_MAX],root[PATH_MAX];
        static unsigned traced;
        if (platform_realpath(path,actual) && platform_realpath(mod_root,root)) {
            size_t n=strlen(root);
            if (!strncmp(actual,root,n) && actual[n]=='/' &&
                __atomic_fetch_add(&traced,1,__ATOMIC_RELAXED)<32)
                printf("Mods: open %s -> %s\n",guest,actual);
        }
    }
    if (save_trace() && save_path(guest)) printf("Save trace: open(%s) -> fd %d\n",guest,fd);
    return fd;
}
static int64_t do_close(int fd) {
    if (fd>=0 && fd<3) return 0;
    pthread_mutex_lock(&lock);
    File *f=get(fd);
    if (!f) { pthread_mutex_unlock(&lock); return -EBADF; }
    File closed=*f;
    *f=(File){0};
    pthread_mutex_unlock(&lock);
    if (t4_path(closed.path)) {
        PlatformStat s={0};
        int r=platform_fstat(closed.host,&s);
        printf("[T4] close %s fd=%d stat=%d bytes=%lld directory=%d\n",
               closed.path,fd,r,(long long)s.size,s.is_dir);
    }
    free_listing(closed.dir);
    int result=0;
    if (closed.temp) result=commit_file(&closed);
    else platform_close(closed.host);
    if (save_trace() && save_path(closed.path))
        printf("Save trace: close(fd %d, %s)%s -> %d\n",fd,closed.path,closed.temp ? closed.dirty ? " commit" : " unwritten" : "",result);
    free(closed.commit); free(closed.temp);
    return result;
}
static int host_fd(int fd) {
    if (fd>=0 && fd<3) return fd;
    File *f=get(fd);
    return f ? f->host : -1;
}
static int host_fd_written(int fd) {
    if (fd>=0 && fd<3) return fd;
    File *f=get(fd);
    if (!f) return -1;
    if (!f->dirty && save_trace() && save_path(f->path)) printf("Save trace: first write to fd %d (%s)\n",fd,f->path);
    f->dirty=1;
    return f->host;
}
/* First successful transfer in each direction, including the guest buffer address. Do not
 * dereference it here: GPU pages may be protected, and tracing must not cause readbacks. */
static void t4_io(int fd,const char *op,const void *buffer,ssize_t bytes,unsigned bit) {
    File *f=get(fd);
    if (bytes<=0 || !f || !t4_path(f->path) || (f->t4_logged&bit)) return;
    f->t4_logged|=bit;
    printf("[T4] %s %s fd=%d buffer=%p bytes=%lld\n",op,f->path,fd,buffer,(long long)bytes);
}
/* The GPU side is told of the write first (runtime_memory_note_write: its tracking unprotects the
 * range). Pages protected again meanwhile would make the kernel's copy fail with EFAULT instead
 * of faulting to our handler: a user-mode write to each page first goes through the handler. */
static void touch_for_write(void *buffer,uint64_t size) {
    if (!size) return;
    uintptr_t p=(uintptr_t)buffer & ~(uintptr_t)4095, end=(uintptr_t)buffer+size;
    for (; p<end; p+=4096) {
        volatile unsigned char *b=(volatile unsigned char *)(p<(uintptr_t)buffer ? (uintptr_t)buffer : p);
        *b=*b;
    }
}
/* A page protected again between the touch and the kernel's copy (the GPU side re-arms its write
 * traps): EFAULT with nothing read. Touched again, the read is tried again a few times. */
static ssize_t read_retrying(int h,void *buffer,uint64_t size,int64_t offset,int positional) {
    ssize_t n=-1;
    for (int attempt=0; attempt<8; ++attempt) {
        n=positional ? platform_pread(h,buffer,size,offset) : platform_read(h,buffer,size);
        if (n>=0 || errno!=EFAULT) break;
        touch_for_write(buffer,size);
    }
    return n;
}
static int64_t do_read(int fd,void *buffer,uint64_t size) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    runtime_memory_note_write((uintptr_t)buffer,size);
    touch_for_write(buffer,size);
    ssize_t n=read_retrying(h,buffer,size,0,0);
    if (n<0) { if (audio_trace()) printf("Audio trace: read(fd %d, %llu) failed, errno %d\n",fd,(unsigned long long)size,errno); return -errno; }
    if (n>0) runtime_memory_note_write((uintptr_t)buffer,(uint64_t)n); /* and once the data is there */
    t4_io(fd,"read",buffer,n,1);
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pread(int fd,void *buffer,uint64_t size,int64_t offset) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    runtime_memory_note_write((uintptr_t)buffer,size);
    touch_for_write(buffer,size);
    ssize_t n=read_retrying(h,buffer,size,offset,1);
    if (n<0) { if (audio_trace()) printf("Audio trace: pread(fd %d, %llu @%lld) failed, errno %d\n",fd,(unsigned long long)size,(long long)offset,errno); return -errno; }
    if (n>0) runtime_memory_note_write((uintptr_t)buffer,(uint64_t)n); /* and once the data is there */
    t4_io(fd,"pread",buffer,n,1);
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
/* Pages the GPU side keeps without access (data newer in VRAM, runtime_memory_trap reasons from
 * 16 up) would make the kernel's copy fail with EFAULT: a user-mode read of each page first goes
 * through the handler, which copies the data back. */
static void touch_for_read(const void *buffer,uint64_t size) {
    if (!size) return;
    uintptr_t p=(uintptr_t)buffer & ~(uintptr_t)4095, end=(uintptr_t)buffer+size;
    for (; p<end; p+=4096)
        (void)*(volatile const unsigned char *)(p<(uintptr_t)buffer ? (uintptr_t)buffer : p);
}
static int64_t do_write(int fd,const void *buffer,uint64_t size) {
    /* SacredFire: the first write into each opened save file is logged (plan milestone d). */
    { File *sf=get(fd); if (sf && !sf->save_logged && !strncmp(sf->path,"/savedata",9)) { sf->save_logged=1; printf("Runtime: save write: %s (%llu bytes)\n",sf->path,(unsigned long long)size); } }
    int h=host_fd_written(fd);
    if (h<0) return -EBADF;
    touch_for_read(buffer,size);
    ssize_t n=platform_write(h,buffer,size);
    if (n<0) return -errno;
    t4_io(fd,"write",buffer,n,2);
    __atomic_add_fetch(&writes,1,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pwrite(int fd,const void *buffer,uint64_t size,int64_t offset) {
    int h=host_fd_written(fd);
    if (h<0) return -EBADF;
    touch_for_read(buffer,size);
    ssize_t n=platform_pwrite(h,buffer,size,offset);
    t4_io(fd,"pwrite",buffer,n,2);
    return n<0 ? -errno : n;
}
static int64_t do_lseek(int fd,int64_t offset,int whence) {
    if (save_trace()) { File *t=get(fd); if (t && save_path(t->path)) printf("Save trace: lseek(fd %d, %lld, %d)\n",fd,(long long)offset,whence); }
    File *f=get(fd);
    if (!f) return -EBADF;
    if (whence<0 || whence>2) return -EINVAL;
    if (f->dir) {
        /* Directory offsets are entry indices for getdirentries. */
        if (whence==0 && offset>=0) { f->position=(size_t)offset; return offset; }
        return -EINVAL;
    }
    int64_t r=platform_lseek(f->host,offset,whence);
    return r<0 ? -errno : r;
}
static int64_t do_fstat(int fd,GuestStat *out) {
    if (save_trace()) { File *t=get(fd); if (t && save_path(t->path)) printf("Save trace: fstat(fd %d)\n",fd); }
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    if (!out) return -EFAULT;
    PlatformStat s;
    int r=platform_fstat(h,&s);
    if (r) return r;
    convert_stat(&s,out); return 0;
}
static int64_t do_stat(const char *guest,GuestStat *out) {
    char path[1024]; PlatformStat s;
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    if (save_path(guest)) current_copy(path,sizeof(path),0);
    if (!out) return -EFAULT;
    int r=platform_stat(path,&s);
    if (r==-ENOENT && !save_path(guest) && fix_case(path)) r=platform_stat(path,&s);
    if (save_trace() && save_path(guest)) printf("Save trace: stat(%s) -> %d, %lld bytes\n",guest,r,r ? -1LL : (long long)s.size);
    if (r) return r;
    convert_stat(&s,out); return 0;
}
static int64_t do_getdents(int fd,char *buffer,uint64_t size,int64_t *basep) {
    pthread_mutex_lock(&lock);
    File *f=get(fd);
    int64_t result=0;
    if (!f) result=-EBADF;
    else if (!f->dir) result=-EINVAL;
    else if (!buffer) result=-EFAULT;
    else if (size<512) result=-EINVAL;
    else {
        if (basep) *basep=(int64_t)f->position;
        uint64_t written=0;
        while (f->position<f->dir->count) {
            const char *name=f->dir->names+f->dir->offsets[f->position];
            size_t n=strlen(name); if (n>255) n=255;
            uint16_t reclen=(uint16_t)((8+n+1+7)&~(size_t)7);
            if (written+reclen>size) break;
            char *p=buffer+written;
            memset(p,0,reclen);
            uint32_t ino=(uint32_t)f->position+1;
            memcpy(p,&ino,4); memcpy(p+4,&reclen,2);
            p[6]=(char)f->dir->types[f->position]; p[7]=(char)n;
            memcpy(p+8,name,n);
            written+=reclen; ++f->position;
        }
        result=(int64_t)written;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static int64_t path_op(const char *guest,int op,int mode) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    int r= op==0 ? platform_mkdir(path,mode ? mode : 0755) : op==1 ? platform_rmdir(path) : platform_unlink(path);
    if (t4_path(guest)) printf("[T4] %s %s -> %d\n",op==0 ? "mkdir" : op==1 ? "rmdir" : "unlink",guest,r);
    if (save_trace() && save_path(guest)) printf("Save trace: %s(%s) -> %d\n",op==0 ? "mkdir" : op==1 ? "rmdir" : "unlink",guest,r);
    return r;
}
static int64_t do_rename(const char *from,const char *to) {
    if (game_path(from) || game_path(to)) return -EROFS;
    char a[1024],b[1024];
    int e=translate(from,a,sizeof(a));
    if (!e) e=translate(to,b,sizeof(b));
    if (e) return -e;
    int r=platform_rename(a,b);
    if (t4_path(from) || t4_path(to)) printf("[T4] rename %s -> %s result=%d\n",from,to,r);
    if (save_trace() && (save_path(from) || save_path(to))) printf("Save trace: rename(%s, %s) -> %d\n",from,to,r);
    return r;
}
static int64_t do_ftruncate(int fd,int64_t length) {
    int h=host_fd_written(fd);
    if (h<0) return -EBADF;
    return platform_ftruncate(h,length);
}
static int64_t do_truncate(const char *guest,int64_t length) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    if (save_path(guest)) current_copy(path,sizeof(path),1);
    if (save_trace() && save_path(guest)) printf("Save trace: truncate(%s, %lld)\n",guest,(long long)length);
    return platform_truncate(path,length);
}
static int64_t do_fsync(int fd) { int h=host_fd(fd); if (h<0) return -EBADF; return platform_fsync(h); }
static int64_t do_access(const char *guest,int mode) {
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    if (save_path(guest)) current_copy(path,sizeof(path),0);
    int r=platform_access(path,mode&7);
    if (r==-ENOENT && !save_path(guest) && fix_case(path)) r=platform_access(path,mode&7);
    return r;
}

/* Convention adapters: sceKernel* -> Orbis error codes, POSIX -> -1 + errno. */
static int64_t sce(int64_t r) { return r<0 ? ERR(runtime_guest_errno((int)-r)) : r; }
static int64_t posix(int64_t r) { if (r<0) { *runtime_errno()=runtime_guest_errno((int)-r); return -1; } return r; }
#define PAIR(name,params,args) \
    static ABI int64_t sce_##name params { return sce(do_##name args); } \
    static ABI int64_t posix_##name params { return posix(do_##name args); }
PAIR(open,(const char *p,int f,int m),(p,f,m))
PAIR(close,(int fd),(fd))
PAIR(read,(int fd,void *b,uint64_t n),(fd,b,n))
PAIR(pread,(int fd,void *b,uint64_t n,int64_t o),(fd,b,n,o))
PAIR(write,(int fd,const void *b,uint64_t n),(fd,b,n))
PAIR(pwrite,(int fd,const void *b,uint64_t n,int64_t o),(fd,b,n,o))
PAIR(lseek,(int fd,int64_t o,int w),(fd,o,w))
PAIR(fstat,(int fd,GuestStat *s),(fd,s))
PAIR(stat,(const char *p,GuestStat *s),(p,s))
PAIR(getdents,(int fd,char *b,uint64_t n,int64_t *base),(fd,b,n,base))
PAIR(rename,(const char *a,const char *b),(a,b))
PAIR(ftruncate,(int fd,int64_t l),(fd,l))
PAIR(truncate,(const char *p,int64_t l),(p,l))
PAIR(fsync,(int fd),(fd))
static ABI int64_t posix_access(const char *p,int m) { return posix(do_access(p,m)); }
static ABI int64_t sce_mkdir(const char *p,int m) { return sce(path_op(p,0,m)); }
static ABI int64_t posix_mkdir(const char *p,int m) { return posix(path_op(p,0,m)); }
static ABI int64_t sce_rmdir(const char *p) { return sce(path_op(p,1,0)); }
static ABI int64_t posix_rmdir(const char *p) { return posix(path_op(p,1,0)); }
static ABI int64_t sce_unlink(const char *p) { return sce(path_op(p,2,0)); }
static ABI int64_t posix_unlink(const char *p) { return posix(path_op(p,2,0)); }
/* SacredFire: sceKernelGetdents is getdirentries without the base pointer. */
static ABI int64_t sce_getdents_nobase(int fd,char *b,uint64_t n) { int64_t base=0; return sce(do_getdents(fd,b,n,&base)); }
static ABI int32_t sce_check_reachability(const char *p) {
    GuestStat s; return (int32_t)sce(do_stat(p,&s));
}

/* UFC 3: fcntl (F_GETFL/F_SETFL only: every file is blocking and stays that way), chmod and utimes
 * on existing paths (permission bits and times are not kept). */
static ABI int32_t sce_fcntl(int fd,int cmd,int64_t arg) {
    (void)arg;
    if (fd<0) return ERR(9);
    if (cmd==3) return 0x2;  /* F_GETFL: O_RDWR */
    return 0;                /* F_SETFL and the rest: accepted */
}
static ABI int32_t sce_chmod(const char *p,uint16_t mode) {
    (void)mode; GuestStat st; return (int32_t)sce(do_stat(p,&st));
}
static ABI int32_t sce_utimes(const char *p,const void *times) {
    (void)times; GuestStat st; return (int32_t)sce(do_stat(p,&st));
}

static const RuntimeExport exports[]={
    {"sceKernelOpen",sce_open}, {"open",posix_open}, {"_open",posix_open},
    {"sceKernelClose",sce_close}, {"close",posix_close}, {"_close",posix_close},
    {"sceKernelRead",sce_read}, {"read",posix_read}, {"_read",posix_read},
    {"sceKernelPread",sce_pread}, {"pread",posix_pread},
    {"sceKernelWrite",sce_write}, {"write",posix_write}, {"_write",posix_write},
    {"sceKernelPwrite",sce_pwrite}, {"pwrite",posix_pwrite},
    {"sceKernelLseek",sce_lseek}, {"lseek",posix_lseek},
    {"sceKernelFstat",sce_fstat}, {"fstat",posix_fstat},
    {"sceKernelStat",sce_stat}, {"stat",posix_stat},
    {"sceKernelGetdirentries",sce_getdents}, {"getdirentries",posix_getdents},
    {"sceKernelGetdents",sce_getdents_nobase},
    {"sceKernelRename",sce_rename}, {"rename",posix_rename},
    {"sceKernelFtruncate",sce_ftruncate}, {"ftruncate",posix_ftruncate},
    {"sceKernelTruncate",sce_truncate}, {"truncate",posix_truncate},
    {"sceKernelFsync",sce_fsync}, {"fsync",posix_fsync},
    {"access",posix_access}, {"sceKernelCheckReachability",sce_check_reachability},
    {"sceKernelMkdir",sce_mkdir}, {"mkdir",posix_mkdir},
    {"sceKernelRmdir",sce_rmdir}, {"rmdir",posix_rmdir},
    {"sceKernelUnlink",sce_unlink}, {"unlink",posix_unlink},
    {"sceKernelFcntl",sce_fcntl}, {"sceKernelChmod",sce_chmod}, {"sceKernelUtimes",sce_utimes},
};
uintptr_t runtime_file_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
/* Host-side helpers for other modules (e.g. SaveData, Fios). */
int64_t runtime_file_open(const char *p,int f,int m) { return do_open(p,f,m); }
int64_t runtime_file_close(int fd) { return do_close(fd); }
int64_t runtime_file_read(int fd,void *b,uint64_t n) { return do_read(fd,b,n); }
int64_t runtime_file_pread(int fd,void *b,uint64_t n,int64_t o) { return do_pread(fd,b,n,o); }
int64_t runtime_file_write(int fd,const void *b,uint64_t n) { return do_write(fd,b,n); }
int64_t runtime_file_lseek(int fd,int64_t o,int w) { return do_lseek(fd,o,w); }
int64_t runtime_file_stat(const char *p,void *s) { return do_stat(p,s); }
int64_t runtime_file_fstat(int fd,void *s) { return do_fstat(fd,s); }
int64_t runtime_file_getdents(int fd,char *b,uint64_t n,int64_t *base) { return do_getdents(fd,b,n,base); }
int runtime_file_translate(const char *guest,char *out,size_t size) { return translate(guest,out,size); }
void runtime_file_report(void) {
    printf("Runtime: files opened=%zu, reads=%zu (%llu bytes), writes=%zu, not found=%zu\n",
           opens,reads,(unsigned long long)bytes_read,writes,missing);
}
