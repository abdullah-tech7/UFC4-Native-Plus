// SPDX-License-Identifier: GPL-2.0-or-later
/* Regression tests for the two observed UFC4 initialization failures. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <direct.h>
#define UFC4_DONOR_EXTENSION 1
#define UFC4_EXTENSION_TEST 1
#include "../src/runtime_ufc4.c"

static _Thread_local int32_t test_errno;
static long test_offset=-10800;
static int test_dst;
static int test_listing_failure;
static int64_t test_save_size=65537;
static const char *test_save_host="work/ufc4-icon-test";
PlatformListing *platform_list_directory(const char *path) {
    (void)path;if(test_listing_failure)return NULL;
    static char names[]="DATA\0INDEX\0staging.bbtmp";
    static size_t offsets[]={0,5,11};
    static PlatformListing result={.names=names,.count=3,.offsets=offsets};return &result;
}
void platform_free_listing(PlatformListing *listing) { (void)listing; }
int platform_stat(const char *path,PlatformStat *info) {
    memset(info,0,sizeof(*info));info->is_reg=1;
    info->size=strstr(path,"/DATA") ? test_save_size : 0;return 0;
}
long platform_utc_offset(time_t t) { (void)t;return test_offset; }
int platform_is_dst(time_t t) { (void)t;return test_dst; }
uintptr_t runtime_kernel_resolve(const char *name) { (void)name;return 789; }
static GUEST int32_t test_mount_info(const void *point,void *info) {
    memset(info,0,48);return strcmp((const char *)point,"/savedata0") ? (int32_t)0x809f0004 : 0;
}
uintptr_t runtime_savedata_resolve(const char *name) {
    if (!strcmp(name,"65VH0Qaaz6s#libSceSaveData"))return (uintptr_t)test_mount_info;
    return 987;
}
int runtime_file_translate(const char *guest,char *out,size_t size) {
    (void)guest;snprintf(out,size,"%s",test_save_host);return 0;
}
const char *runtime_file_user_dir(void) { return "work/mock-profile"; }
uintptr_t runtime_file_resolve(const char *name) { (void)name;return 654; }
int32_t *runtime_errno(void) { return &test_errno; }
uintptr_t runtime_services_resolve(const char *name) { (void)name; return 123; }
const char *runtime_symbol(const char *name) {
    if (!strncmp(name,"sceNetSocket#",13)) return "sceNetSocket";
    return NULL;
}
uintptr_t bbgpu_resolve(const char *name) { (void)name; return 456; }
uint32_t platform_thread_id(void) { return GetCurrentThreadId(); }
uint64_t platform_monotonic_ns(void) {
    LARGE_INTEGER count,frequency; QueryPerformanceCounter(&count);QueryPerformanceFrequency(&frequency);
    return (uint64_t)(count.QuadPart/frequency.QuadPart)*UINT64_C(1000000000)+
           (uint64_t)(count.QuadPart%frequency.QuadPart)*UINT64_C(1000000000)/(uint64_t)frequency.QuadPart;
}
int platform_sleep_ns(uint64_t ns) { (void)ns; SwitchToThread();return 0; }
void original_attach(void *context) {
    Sleep(2); /* Make the asynchronous attachment race deterministic. */
    *(uint64_t *)context=UINT64_C(0x12345000); /* donor's temporary TCB value */
}
static void *test_thread(void *context) { ufc4_thread_attach(context);return NULL; }
static void *errno_thread(void *context) {
    (void)context; assert(*u4_net_errno_location()==0);
    assert(u4_net_close(-1)==(int32_t)0x80410109);
    assert(*u4_net_errno_location()==9); return NULL;
}
int main(void) {
#ifdef UFC4_DONOR_V015
    assert(!ufc4_offline_resolver("sceNetConnect#libSceNet"));
    assert(!ufc4_offline_resolver("any-online-export"));
#endif
    uint64_t mount_info[6]={0};u4_set_quota("/savedata0",576);
    assert(!u4_save_info("/savedata0",mount_info) && mount_info[0]==576 && mount_info[1]==573);
    test_save_size=0;assert(!u4_save_info("/savedata0",mount_info) && mount_info[1]==576);
    test_save_size=INT64_C(32768)*600;
    assert(!u4_save_info("/savedata0",mount_info) && !mount_info[1]);
    test_listing_failure=1;assert(u4_save_info("/savedata0",mount_info)==(int32_t)0x809f000b);
    test_listing_failure=0;memset(u4_quotas,0,sizeof(u4_quotas));
    test_save_size=65537;
    test_save_host="work/mock-profile/savedata/0/CUSA14209/PROFILE";
    u4_set_quota("/savedata0",576);
    test_save_host="work/mock-profile/savedata/0/CUSA14209/EXTRADATA0";
    u4_set_quota("/savedata0",768);
    test_save_host="work/mock-profile/savedata/0/CUSA14209/PROFILE";
    assert(!u4_save_info("/savedata0",mount_info) && mount_info[0]==576 && mount_info[1]==573);
    unsigned char search_condition[64]={0},search_result[56]={0};char name[32]="PROFILE";
    void *names=name,*infos=mount_info;uint32_t count=1;
    mount_info[0]=32768;mount_info[1]=16384;
    memcpy(search_result+8,&names,8);memcpy(search_result+20,&count,4);memcpy(search_result+32,&infos,8);
    assert(!u4_save_search_space(search_condition,search_result));
    assert(mount_info[0]==576 && mount_info[1]==573);
    strcpy(name,"EXTRADATA0");mount_info[0]=32768;mount_info[1]=16384;
    assert(!u4_save_search_space(search_condition,search_result));
    assert(mount_info[0]==768 && mount_info[1]==765);
    test_save_host="work/ufc4-icon-test";
    puts("PASS: requested save quota, measured usage, full/empty bounds and query errors");
    _mkdir("work/ufc4-icon-test.sce_sys");
    const char *icon_path="work/ufc4-icon-test.sce_sys/icon0.png";
    FILE *icon_file=fopen(icon_path,"wb");assert(icon_file);
    const char icon_bytes[]="fixture-icon-content";
    assert(fwrite(icon_bytes,1,sizeof(icon_bytes),icon_file)==sizeof(icon_bytes));fclose(icon_file);
    char icon_data[64]={0};U4SaveIcon icon={.buffer=icon_data,.buffer_size=sizeof(icon_data)};
    assert(!u4_save_load_icon("/savedata0",&icon));
    assert(icon.data_size==sizeof(icon_bytes) && !memcmp(icon_data,icon_bytes,sizeof(icon_bytes)));
    icon.buffer_size=3;memset(icon_data,0xcc,sizeof(icon_data));
    assert(!u4_save_load_icon("/savedata0",&icon));
    assert(icon.data_size==sizeof(icon_bytes) && !memcmp(icon_data,icon_bytes,3) && (unsigned char)icon_data[3]==0xcc);
    assert(u4_save_load_icon("/savedata9",&icon)==(int32_t)0x809f0004);
    assert(u4_save_load_icon(NULL,&icon)==(int32_t)0x809f0000);
    assert(!remove(icon_path));_rmdir("work/ufc4-icon-test.sce_sys");
    assert(u4_save_load_icon("/savedata0",&icon)==(int32_t)0x809f0008);
    puts("PASS: saved icon bytes, size reporting, partial buffer and missing/unmounted/invalid errors");
    U4Timezone zone={0};int64_t utc=0,local=0;int32_t dst=-1;uint64_t dst64=UINT64_MAX;
    U4Timesec detail={0};
    assert(!u4_localtime_to_utc(1700000000,1,&utc,&zone,&dst));
    assert(utc==1700010800 && zone.offset_minutes==-180 && !zone.dst_minutes && !dst);
    assert(!u4_utc_to_localtime(utc,&local,&detail,&dst64));
    assert(local==1700000000 && detail.time==utc && detail.offset_seconds==-10800 && !dst64);
    assert(!u4_localtime_to_utc(1,1,NULL,&zone,NULL));
    assert(u4_localtime_to_utc(1,1,&utc,NULL,&dst)==(int32_t)0x80020016);
    test_offset=7200;test_dst=1;
    assert(!u4_localtime_to_utc(100000,1,&utc,&zone,&dst));
    assert(utc==92800 && zone.offset_minutes==60 && zone.dst_minutes==60 && dst==3600);
    assert(!u4_utc_to_localtime(utc,&local,&detail,&dst64));
    assert(local==100000 && detail.offset_seconds==3600 && detail.dst_seconds==3600 && dst64==3600);
    assert(ufc4_kernel_resolve("0NTHN1NKONI#libkernel")== (uintptr_t)u4_localtime_to_utc);
    assert(ufc4_kernel_resolve("0NTHN1NKONI#other")==789);
    puts("PASS: five-argument local-to-UTC ABI, UTC detail layout, optional outputs and timezone offsets");
    uint64_t words[64]={0}; pthread_t threads[64];
    for (unsigned i=0;i<64;i++) {
        assert(!ufc4_host_pthread_create(&threads[i],NULL,test_thread,&words[i]));
        assert(words[i] && words[i]!=UINT64_C(0x12345000));
    }
    for (unsigned i=0;i<64;i++) assert(!pthread_join(threads[i],NULL));
    for (unsigned i=0;i<128;i++) {
        words[0]=0;
        assert(!ufc4_host_pthread_create(&threads[0],NULL,test_thread,&words[0]));
        assert(words[0] && words[0]!=UINT64_C(0x12345000));
        assert(!pthread_join(threads[0],NULL));
    }
    for (unsigned i=0;i<4096;i++)assert(!atomic_load(&ready_threads[i].context));
    puts("PASS: delayed thread attachment publishes an ID before creation returns");
    uint32_t flag=999;
    assert(!ufc4_pft_flag(&flag) && flag==0);
    assert(ufc4_pft_flag(NULL)==(int32_t)0x80d90002);
    assert(!ufc4_is_prospero());
    assert(ufc4_services_resolve("sceNetSocket#libSceNet")== (uintptr_t)u4_net_socket);
    assert(ufc4_services_resolve("sceNetSocket#anotherLibrary")==123);
    assert(ufc4_bbgpu_fallback("unknown#libkernel")==456);
    assert(ufc4_bbgpu_fallback("xmhnAoxN3Wk#libSceAppContentPft")== (uintptr_t)ufc4_pft_flag);
    assert(u4_net_socket("test",28,2,0)==(int32_t)0x8041012f);
    int id=u4_net_socket("regression-test",2,2,17); assert(id>=10000);
    int yes=1; assert(!u4_net_set_option(id,0xffff,0x1200,&yes,4));
    U4Address address={.length=16,.family=2,.address=0x0100007f};
    assert(!u4_net_bind(id,&address,sizeof(address)));
    uint32_t length=sizeof(address); assert(!u4_net_name(id,&address,&length));
    assert(length==16 && address.length==16 && address.family==2 && address.port!=0);
    char buffer[32];
    assert(u4_net_recv(id,buffer,sizeof(buffer),0)==(int32_t)0x80410123);
    assert(*u4_net_errno_location()==35);
    assert(u4_net_offline(id)==(int32_t)0x80410133);
    assert(*u4_net_errno_location()==51);
    pthread_t other;assert(!pthread_create(&other,NULL,errno_thread,NULL));assert(!pthread_join(other,NULL));
    assert(*u4_net_errno_location()==51);
    int size=8192;assert(!u4_net_set_option(id,0xffff,0x1002,&size,4));
    uint32_t size_length=4;size=0;assert(!u4_net_get_option(id,0xffff,0x1002,&size,&size_length));assert(size>=8192);
    assert(u4_net_set_option(id,0xffff,0x7777,&size,4)==(int32_t)0x8041012a);
    assert(u4_net_bind(id,NULL,16)==(int32_t)0x80410116);
    assert(!u4_net_close(id));assert(u4_net_close(id)==(int32_t)0x80410109);
    puts("PASS: UDP lifecycle, BSD address translation, nonblocking receive, offline sends, TLS errno and resolver isolation");
    return 0;
}
