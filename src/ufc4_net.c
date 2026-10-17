// SPDX-License-Identifier: GPL-2.0-or-later
/* Windows local socket resources for UFC4's offline DirtySDK startup.
 * PS4 layouts/options checked against shadPS4 src/core/libraries/net/net_types.h.
 * No outbound connection or packet is sent: NetCtl remains disconnected.
 * Unsupported operations return an explicit network error.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <limits.h>

extern int32_t *runtime_errno(void);
extern uintptr_t runtime_services_resolve(const char *);
extern const char *runtime_symbol(const char *);

typedef struct { uint8_t length,family; uint16_t port; uint32_t address; uint8_t padding[8]; } U4Address;
typedef struct { SOCKET host; int active,type; } U4Socket;
static U4Socket u4_sockets[256];
static pthread_mutex_t u4_net_lock=PTHREAD_MUTEX_INITIALIZER;
static int u4_wsa_ready;
static HMODULE u4_wsa;
#define WSA_PROC(name) static __typeof__(&name) u4_##name
WSA_PROC(socket); WSA_PROC(bind); WSA_PROC(closesocket); WSA_PROC(ioctlsocket);
WSA_PROC(setsockopt); WSA_PROC(getsockopt); WSA_PROC(getsockname);
WSA_PROC(recv); WSA_PROC(recvfrom); WSA_PROC(WSAGetLastError);
#undef WSA_PROC
static int u4_initialize_wsa(void) {
    if (u4_wsa_ready) return u4_wsa_ready>0;
    u4_wsa_ready=-1; u4_wsa=LoadLibraryA("ws2_32.dll");
    if (!u4_wsa) return 0;
#define LOAD(name) u4_##name=(__typeof__(u4_##name))GetProcAddress(u4_wsa,#name); if (!u4_##name) return 0
    LOAD(socket); LOAD(bind); LOAD(closesocket); LOAD(ioctlsocket);
    LOAD(setsockopt); LOAD(getsockopt); LOAD(getsockname);
    LOAD(recv); LOAD(recvfrom); LOAD(WSAGetLastError);
#undef LOAD
    WSADATA data;
    if (WSAStartup(MAKEWORD(2,2),&data)) return 0;
    u4_wsa_ready=1; return 1;
}
static int32_t u4_net_error(int bsd) {
    *runtime_errno()=bsd;
    return (int32_t)(UINT32_C(0x80410100)|(unsigned)bsd);
}
static int32_t u4_last_error(void) {
    int error=u4_WSAGetLastError(),bsd=5;
    switch(error) {
    case WSAEWOULDBLOCK: bsd=35; break; case WSAEINPROGRESS: bsd=36; break;
    case WSAEBADF: case WSAENOTSOCK: bsd=9; break; case WSAEINVAL: bsd=22; break;
    case WSAEFAULT: bsd=14; break; case WSAEACCES: bsd=13; break;
    case WSAEMFILE: bsd=24; break; case WSAEADDRINUSE: bsd=48; break;
    case WSAEADDRNOTAVAIL: bsd=49; break; case WSAENETUNREACH: bsd=51; break;
    case WSAECONNRESET: bsd=54; break; case WSAENOBUFS: bsd=55; break;
    case WSAENOTCONN: bsd=57; break; case WSAETIMEDOUT: bsd=60; break;
    case WSAECONNREFUSED: bsd=61; break; case WSAENOPROTOOPT: bsd=42; break;
    case WSAEAFNOSUPPORT: bsd=47; break;
    }
    return u4_net_error(bsd);
}
static U4Socket *u4_get_socket(int id) {
    int i=id-10000;
    return i>=0 && i<256 && u4_sockets[i].active ? &u4_sockets[i] : NULL;
}
static GUEST int32_t *u4_net_errno_location(void) { return runtime_errno(); }
static GUEST int32_t u4_net_socket(const char *name,int af,int type,int protocol) {
    if (af!=2) return u4_net_error(47);
    if (type!=1 && type!=2) return u4_net_error(43);
    pthread_mutex_lock(&u4_net_lock);
    int32_t result=u4_net_error(24);
    if (!u4_initialize_wsa()) result=u4_net_error(50);
    else for (int i=0;i<256;i++) if (!u4_sockets[i].active) {
        SOCKET host=u4_socket(AF_INET,type,protocol);
        if (host==INVALID_SOCKET) {result=u4_last_error(); break;}
        u_long nonblocking=1;
        if (u4_ioctlsocket(host,FIONBIO,&nonblocking)) {result=u4_last_error();u4_closesocket(host);break;}
        u4_sockets[i]=(U4Socket){host,1,type}; result=10000+i;
        printf("UFC4: local offline socket %d (%s, type %d)\n",result,name?name:"",type);
        break;
    }
    pthread_mutex_unlock(&u4_net_lock); return result;
}
static GUEST int32_t u4_net_close(int id) {
    pthread_mutex_lock(&u4_net_lock); U4Socket *s=u4_get_socket(id);
    int32_t result=s ? (u4_closesocket(s->host) ? u4_last_error() : 0) : u4_net_error(9);
    if (s && !result) s->active=0;
    pthread_mutex_unlock(&u4_net_lock); return result;
}
static int u4_to_host(const U4Address *src,uint32_t size,struct sockaddr_in *dst) {
    if (!src || size<16 || src->family!=2) return 0;
    memset(dst,0,sizeof(*dst)); dst->sin_family=AF_INET;
    dst->sin_port=src->port; dst->sin_addr.s_addr=src->address; return 1;
}
static GUEST int32_t u4_net_bind(int id,const U4Address *address,uint32_t length) {
    struct sockaddr_in host;
    if (!u4_to_host(address,length,&host)) return u4_net_error(22);
    pthread_mutex_lock(&u4_net_lock); U4Socket *s=u4_get_socket(id);
    int32_t result=s ? (u4_bind(s->host,(struct sockaddr *)&host,sizeof(host)) ? u4_last_error() : 0) : u4_net_error(9);
    pthread_mutex_unlock(&u4_net_lock); return result;
}
static GUEST int32_t u4_net_name(int id,U4Address *address,uint32_t *length) {
    if (!address || !length || *length<16) return u4_net_error(22);
    pthread_mutex_lock(&u4_net_lock); U4Socket *s=u4_get_socket(id);
    struct sockaddr_in host; int size=sizeof(host); int32_t result=0;
    if (!s) result=u4_net_error(9);
    else if (u4_getsockname(s->host,(struct sockaddr *)&host,&size)) result=u4_last_error();
    else { memset(address,0,16);address->length=16;address->family=2;address->port=host.sin_port;address->address=host.sin_addr.s_addr;*length=16; }
    pthread_mutex_unlock(&u4_net_lock); return result;
}
static int u4_host_option(int level,int option,int *host_level,int *host_option) {
    *host_level=level; *host_option=option;
    if (level==0xffff) {
        switch(option) {
        case 4: *host_option=SO_REUSEADDR; break; case 8: *host_option=SO_KEEPALIVE; break;
        case 0x20: *host_option=SO_BROADCAST; break; case 0x1001: *host_option=SO_SNDBUF; break;
        case 0x1002: *host_option=SO_RCVBUF; break; case 0x1007: *host_option=SO_ERROR; break;
        case 0x1008: *host_option=SO_TYPE; break; default:return 0;
        }
        *host_level=SOL_SOCKET; return 1;
    }
    if (level==6 && option==1) { *host_level=IPPROTO_TCP; *host_option=TCP_NODELAY; return 1; }
    if (level==0 && option==4) { *host_level=IPPROTO_IP; *host_option=IP_TTL; return 1; }
    return 0;
}
static GUEST int32_t u4_net_set_option(int id,int level,int option,const void *value,uint32_t size) {
    if (!value || size<4) return u4_net_error(22);
    pthread_mutex_lock(&u4_net_lock); U4Socket *s=u4_get_socket(id); int32_t result=0;
    int host_level,host_option;
    if (!s) result=u4_net_error(9);
    else if (level==0xffff && option==0x1200) {
        /* The offline provider uses nonblocking reads on every host socket. */
        if (!*(const int *)value) result=u4_net_error(45);
    } else if (!u4_host_option(level,option,&host_level,&host_option)) {
        printf("UFC4: unsupported socket option level=%x option=%x\n",level,option); result=u4_net_error(42);
    } else if (u4_setsockopt(s->host,host_level,host_option,value,(int)size)) result=u4_last_error();
    pthread_mutex_unlock(&u4_net_lock); return result;
}
static GUEST int32_t u4_net_get_option(int id,int level,int option,void *value,uint32_t *size) {
    if (!value || !size || *size<4 || *size>INT_MAX) return u4_net_error(22);
    pthread_mutex_lock(&u4_net_lock); U4Socket *s=u4_get_socket(id); int32_t result=0;
    int host_level,host_option,length=(int)*size;
    if (!s) result=u4_net_error(9);
    else if (level==0xffff && option==0x1200) {*(int *)value=1;*size=4;}
    else if (!u4_host_option(level,option,&host_level,&host_option)) result=u4_net_error(42);
    else if (u4_getsockopt(s->host,host_level,host_option,value,&length)) result=u4_last_error();
    else *size=(uint32_t)length;
    pthread_mutex_unlock(&u4_net_lock); return result;
}
static GUEST int32_t u4_net_offline(int id) {
    pthread_mutex_lock(&u4_net_lock); int valid=u4_get_socket(id)!=NULL;
    pthread_mutex_unlock(&u4_net_lock); return u4_net_error(valid?51:9);
}
static GUEST int64_t u4_net_receive(int id,void *buffer,uint64_t size,int flags,U4Address *from,uint32_t *from_length) {
    (void)flags;
    if ((!buffer && size) || size>INT_MAX || (from && (!from_length || *from_length<16))) return u4_net_error(22);
    pthread_mutex_lock(&u4_net_lock); U4Socket *s=u4_get_socket(id); int64_t result;
    struct sockaddr_in host; int length=sizeof(host);
    if (!s) result=u4_net_error(9);
    else {
        result=u4_recvfrom(s->host,buffer,(int)size,0,from?(struct sockaddr *)&host:NULL,from?&length:NULL);
        if (result==SOCKET_ERROR) result=u4_last_error();
        else if (from) { memset(from,0,16);from->length=16;from->family=2;from->port=host.sin_port;from->address=host.sin_addr.s_addr;*from_length=16; }
    }
    pthread_mutex_unlock(&u4_net_lock); return result;
}
static GUEST int64_t u4_net_recv(int id,void *buffer,uint64_t size,int flags) { return u4_net_receive(id,buffer,size,flags,NULL,NULL); }

uintptr_t ufc4_services_resolve(const char *name) {
    uintptr_t dialog=u4_dialog_resolve(name);if(dialog)return dialog;
    const char *symbol=runtime_symbol(name);
    const char *library=strchr(name,'#');
    if (symbol && library && !strcmp(library,"#libSceNet")) {
#define ENTRY(n,f) if (!strcmp(symbol,n)) return (uintptr_t)f
        ENTRY("sceNetSocket",u4_net_socket); ENTRY("sceNetBind",u4_net_bind);
        ENTRY("sceNetSocketClose",u4_net_close); ENTRY("sceNetGetsockname",u4_net_name);
        ENTRY("sceNetSetsockopt",u4_net_set_option); ENTRY("sceNetGetsockopt",u4_net_get_option);
        ENTRY("sceNetErrnoLoc",u4_net_errno_location); ENTRY("sceNetRecvfrom",u4_net_receive);
        ENTRY("sceNetRecv",u4_net_recv); ENTRY("sceNetConnect",u4_net_offline);
        ENTRY("sceNetSend",u4_net_offline); ENTRY("sceNetSendto",u4_net_offline);
#undef ENTRY
    }
    return runtime_services_resolve(name);
}
/* The measured donor inlined the services resolver as a runtime_lookup call. */
uintptr_t ufc4_services_lookup(const void *table,size_t count,const char *name) {
    (void)table; (void)count; return ufc4_services_resolve(name);
}
