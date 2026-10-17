// SPDX-License-Identifier: GPL-2.0-or-later
/* Optional diagnostics for the observed save failure; no result is changed. */
extern uintptr_t runtime_file_resolve(const char *name);
typedef int64_t (GUEST *U4FileIO)(int,void *,uint64_t);
typedef int32_t (GUEST *U4FileSync)(int);
typedef int64_t (GUEST *U4FilePositioned)(int,void *,uint64_t,int64_t);
static GUEST int64_t u4_file_pwrite(int fd,void *data,uint64_t length,int64_t offset) {
    U4FilePositioned original=(U4FilePositioned)runtime_file_resolve("nKWi-N2HBV4#libkernel");
    int64_t code=original(fd,data,length,offset);
    printf("UFC4 IO: pwrite fd=%d bytes=%llu offset=%lld result=%lld\n",fd,
        (unsigned long long)length,(long long)offset,(long long)code);
    return code;
}
static GUEST int64_t u4_file_pread(int fd,void *data,uint64_t length,int64_t offset) {
    U4FilePositioned original=(U4FilePositioned)runtime_file_resolve("+r3rMFwItV4#libkernel");
    int64_t code=original(fd,data,length,offset);
    if (code<0) printf("UFC4 IO: pread fd=%d bytes=%llu offset=%lld result=%lld\n",fd,
        (unsigned long long)length,(long long)offset,(long long)code);
    return code;
}
static GUEST int64_t u4_file_read(int fd,void *data,uint64_t length) {
    U4FileIO original=(U4FileIO)runtime_file_resolve("Cg4srZ6TKbU#libkernel");
    int64_t code=original(fd,data,length);
    if (length<=64 || code<0) {
        uint64_t first=0;if (code>=8)memcpy(&first,data,8);
        printf("UFC4 IO: read fd=%d bytes=%llu result=%lld first=%016llx caller=%p\n",fd,
            (unsigned long long)length,(long long)code,(unsigned long long)first,__builtin_return_address(0));
    }
    return code;
}
static GUEST int32_t u4_file_close(int fd) {
    U4FileSync original=(U4FileSync)runtime_file_resolve("UK2Tl2DWUns#libkernel");
    int32_t code=original(fd);
    printf("UFC4 IO: close fd=%d result=%08x caller=%p\n",fd,(unsigned)code,__builtin_return_address(0));
    return code;
}
static GUEST int64_t u4_file_write(int fd,void *data,uint64_t length) {
    U4FileIO original=(U4FileIO)runtime_file_resolve("4wSze92BhLI#libkernel");
    int64_t code=original(fd,data,length);
    printf("UFC4 IO: write fd=%d bytes=%llu result=%lld\n",fd,(unsigned long long)length,(long long)code);
    return code;
}
static GUEST int32_t u4_file_sync(int fd) {
    U4FileSync original=(U4FileSync)runtime_file_resolve("fTx66l5iWIA#libkernel");
    int32_t code=original(fd);
    printf("UFC4 IO: fsync fd=%d result=%08x\n",fd,(unsigned)code);
    return code;
}
static uintptr_t u4_file_diagnostic(const char *name) {
    const char *trace=getenv("BB_SAVE_TRACE");
    if (!trace || trace[0]!='1') return 0;
    if (!strcmp(name,"4wSze92BhLI#libkernel"))return (uintptr_t)u4_file_write;
    if (!strcmp(name,"fTx66l5iWIA#libkernel"))return (uintptr_t)u4_file_sync;
    if (!strcmp(name,"UK2Tl2DWUns#libkernel"))return (uintptr_t)u4_file_close;
    if (!strcmp(name,"Cg4srZ6TKbU#libkernel"))return (uintptr_t)u4_file_read;
    if (!strcmp(name,"nKWi-N2HBV4#libkernel"))return (uintptr_t)u4_file_pwrite;
    if (!strcmp(name,"+r3rMFwItV4#libkernel"))return (uintptr_t)u4_file_pread;
    return 0;
}
