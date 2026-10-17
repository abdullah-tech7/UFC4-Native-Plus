// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform/platform.h"
static pthread_mutex_t u4_quota_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { char point[16],host[1024];uint64_t blocks; } u4_quotas[16];
static void u4_set_quota(const void *point,uint64_t blocks) {
    char host[1024];
    if(runtime_file_translate(point,host,sizeof(host)))return;
    pthread_mutex_lock(&u4_quota_lock);
    unsigned selected=16;
    for(unsigned i=0;i<16;i++)if(!strcmp(u4_quotas[i].host,host)){selected=i;break;}
    if(selected==16)for(unsigned i=0;i<16;i++)if(!u4_quotas[i].host[0]){selected=i;break;}
    if(selected<16) {
        snprintf(u4_quotas[selected].point,sizeof(u4_quotas[selected].point),"%s",(const char *)point);
        snprintf(u4_quotas[selected].host,sizeof(u4_quotas[selected].host),"%s",host);
        u4_quotas[selected].blocks=blocks;
    }
    pthread_mutex_unlock(&u4_quota_lock);
}
/* Measure committed user files. Atomic-write staging copies are not additional
 * save content. Fail a query instead of claiming free space when it cannot be read. */
static int u4_save_bytes(const char *path,unsigned depth,uint64_t *bytes) {
    if(depth>16)return -1;
    PlatformListing *listing=platform_list_directory(path);
    if(!listing)return -1;
    int code=0;
    for(size_t i=0;i<listing->count;i++) {
        const char *name=listing->names+listing->offsets[i];size_t length=strlen(name);
        if(!strcmp(name,".") || !strcmp(name,"..") ||
           (length>=6 && !strcmp(name+length-6,".bbtmp")))continue;
        char child[1200];PlatformStat info;
        if(snprintf(child,sizeof(child),"%s/%s",path,name)>=(int)sizeof(child) || platform_stat(child,&info)) {code=-1;break;}
        if(info.is_dir) {if(u4_save_bytes(child,depth+1,bytes)){code=-1;break;}}
        else if(info.is_reg && info.size>=0) {
            if(UINT64_MAX-*bytes<(uint64_t)info.size){code=-1;break;}
            *bytes+=(uint64_t)info.size;
        }
    }
    platform_free_listing(listing);return code;
}
static int u4_save_capacity(const void *point,void *info) {
    char host[1024];
    if(runtime_file_translate(point,host,sizeof(host)))return -1;
    uint64_t quota=0;
    pthread_mutex_lock(&u4_quota_lock);
    for(unsigned i=0;i<16;i++)if(!strcmp(u4_quotas[i].host,host)){quota=u4_quotas[i].blocks;break;}
    pthread_mutex_unlock(&u4_quota_lock);
    if(!quota)return 0; /* A mount not created through this game's Mount2 path. */
    uint64_t bytes=0;
    if(u4_save_bytes(host,0,&bytes))return -1;
    uint64_t used=bytes/32768+(bytes%32768!=0);
    ((uint64_t *)info)[0]=quota;
    ((uint64_t *)info)[1]=used>=quota ? 0 : quota-used;
    return 0;
}
extern const char *runtime_file_user_dir(void);
static int u4_save_search_space(const void *condition,void *output) {
    const char *title="CUSA14209";const void *title_pointer=NULL;int32_t user;
    memcpy(&user,condition,4);memcpy(&title_pointer,(const char *)condition+8,8);
    if(title_pointer)title=title_pointer;
    if(strnlen(title,16)>=16 || strchr(title,'/') || strchr(title,'\\'))return -1;
    void *names=NULL,*infos=NULL;uint32_t count;
    memcpy(&names,(char *)output+8,8);memcpy(&count,(char *)output+20,4);memcpy(&infos,(char *)output+32,8);
    if(!names || !infos)return 0;
    for(uint32_t i=0;i<count;i++) {
        const char *name=(const char *)names+i*32;
        if(strnlen(name,32)>=32 || strchr(name,'/') || strchr(name,'\\') || strstr(name,".."))return -1;
        char path[1200];uint64_t bytes=0;
        if(snprintf(path,sizeof(path),"%s/savedata/%d/%s/%s",runtime_file_user_dir(),user,title,name)>=(int)sizeof(path) ||
           u4_save_bytes(path,0,&bytes))return -1;
        uint64_t *info=(uint64_t *)((char *)infos+i*48),quota=info[0];
        pthread_mutex_lock(&u4_quota_lock);
        for(unsigned j=0;j<16;j++)if(!strcmp(path,u4_quotas[j].host)){quota=u4_quotas[j].blocks;break;}
        pthread_mutex_unlock(&u4_quota_lock);
        uint64_t used=bytes/32768+(bytes%32768!=0);
        printf("UFC4 save: search %s old blocks=%llu free=%llu measured used=%llu\n",name,
            (unsigned long long)info[0],(unsigned long long)info[1],(unsigned long long)used);
        info[0]=quota;info[1]=used>=quota ? 0 : quota-used;
    }
    return 0;
}
