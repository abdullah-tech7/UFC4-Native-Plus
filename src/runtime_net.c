/* SPDX-License-Identifier: GPL-2.0-or-later
 * ufc3: online play between friends (BB_ONLINE_SERVER=<host address>). Off by default: without
 * the variable nothing here is resolved and runtime_services.c keeps the console offline.
 *
 * With it, the game sees a PSN account that is signed in with PS Plus, a connected network, and
 * real sockets. Every host name it resolves under ea.com goes to the online server
 * (tools/ufc3_server.py, run by the player who hosts); any other name fails. Direct-IP socket
 * egress is not filtered yet. The game's HTTP requests (sceHttp) go to the same server in plain
 * HTTP. PSN web APIs (friends, invitations) stay unavailable.
 *
 * BB_NET_LOG=1 (default on in this mode): every call, for working out the online flow. */
#define _GNU_SOURCE
#include "runtime.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#else
#define strncasecmp _strnicmp
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET HostSocket;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/uio.h>
typedef int HostSocket;
#define BAD_SOCKET (-1)
#define close_socket close
#endif

/* ---- configuration ---- */
static int online = -1;
static char server_host[256];
static uint32_t server_addr; /* network order */
static int net_log;
static uint64_t net_trace_size;
#ifndef _WIN32
/* BB_MEMGREP=text:file: while the game runs, creating "<file>.go" makes it search all of its
 * readable memory for <text> and save 16 KiB around each hit to <file> (then "<file>.go" is
 * removed). Finds data the game unpacked at run time (UI scripts) for offline analysis. */
static void *memgrep_thread(void *arg) {
    (void)arg;
    char spec[512]; snprintf(spec, sizeof spec, "%s", getenv("BB_MEMGREP"));
    char *colon = strchr(spec, ':');
    if (!colon) return NULL;
    *colon = 0;
    const char *path = colon + 1;
    char go[600]; snprintf(go, sizeof go, "%s.go", path);
    for (;;) {
        sleep(1);
        if (access(go, F_OK) != 0) continue;
        /* "<file>.go" may name the text to find instead (first line). */
        static char needle_buf[600];
        snprintf(needle_buf, sizeof needle_buf, "%s", spec);
        FILE *g = fopen(go, "r");
        if (g) {
            char line[256];
            if (fgets(line, sizeof line, g)) { line[strcspn(line, "\r\n")] = 0; if (line[0]) snprintf(needle_buf, sizeof needle_buf, "%s", line); }
            fclose(g);
        }
        const char *needle = needle_buf;
        const size_t nlen = strlen(needle);
        FILE *out = fopen(path, "wb"), *maps = fopen("/proc/self/maps", "r");
        char row[512]; int hits = 0;
        while (out && maps && fgets(row, sizeof row, maps) && hits < 400) {
            unsigned long a0, a1; char perm[5], file[256] = "";
            if (sscanf(row, "%lx-%lx %4s %*s %*s %*s %255s", &a0, &a1, perm, file) < 3 || perm[0] != 'r') continue;
            if (file[0] == '/' || file[0] == '[') continue; /* host libraries, stack, vdso */
            if (a1 - a0 > (8ul << 30)) continue;
            /* Copy through process_vm_readv: pages that are mapped but not backed fail instead of
             * faulting. Chunks overlap by the needle length so no hit is split. */
            static char chunk[(1 << 20) + 512];
            for (unsigned long at = a0; at < a1 && hits < 400; at += (1ul << 20)) {
                size_t want = a1 - at < (1ul << 20) + nlen ? a1 - at : (1ul << 20) + nlen;
                struct iovec local = {chunk, want}, remote = {(void *)at, want};
                ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
                if (got <= 0) continue;
                const char *p = chunk, *end = chunk + got;
                while (hits < 400 && (p = memmem(p, (size_t)(end - p), needle, nlen))) {
                    unsigned long addr = at + (unsigned long)(p - chunk);
                    unsigned long from = addr > a0 + 8192 ? addr - 8192 : a0;
                    unsigned long to = addr + 8192 < a1 ? addr + 8192 : a1;
                    static char ctx[16384 + 64];
                    struct iovec l2 = {ctx, to - from}, r2 = {(void *)from, to - from};
                    ssize_t n = process_vm_readv(getpid(), &l2, 1, &r2, 1, 0);
                    fprintf(out, "\n==== hit %d at %#lx ====\n", hits, addr);
                    if (n > 0) fwrite(ctx, 1, (size_t)n, out);
                    ++hits; p += nlen;
                }
            }
        }
        if (maps) fclose(maps);
        if (out) fclose(out);
        fprintf(stderr, "Net: memgrep: %d hits for \"%s\" saved to %s\n", hits, needle, path);
        remove(go);
    }
    return NULL;
}
#endif
static void configure(void) {
    if (online >= 0) return;
#ifndef _WIN32
    if (getenv("BB_MEMGREP")) { pthread_t t; pthread_create(&t, NULL, memgrep_thread, NULL); pthread_detach(t); }
#endif
    const char *server = getenv("BB_ONLINE_SERVER");
    online = server && *server;
    if (!online) return;
    snprintf(server_host, sizeof server_host, "%s", server);
    char *colon = strrchr(server_host, ':');
    if (colon && !strchr(colon, ']')) *colon = 0; /* a port is ignored: the server's ports are fixed */
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    struct in_addr a;
    if (inet_pton(AF_INET, server_host, &a) == 1) server_addr = a.s_addr;
    else {
        struct addrinfo hints = {.ai_family = AF_INET}, *res = NULL;
        if (getaddrinfo(server_host, NULL, &hints, &res) == 0 && res) {
            server_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(res);
        }
    }
    const char *log = getenv("BB_NET_LOG");
    net_log = log && *log ? atoi(log) : 1;
    const char *trace = getenv("BB_NET_TRACE_SIZE");
    net_trace_size = trace && *trace ? strtoull(trace, NULL, 0) : 0;
    char text[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &server_addr, text, sizeof text);
    printf("Online: server %s (%s); PSN simulated (signed in, PS Plus); only *.ea.com is resolved\n",
           server_host, text);
}
#define LOG(...) do { if (net_log) { printf("Net: " __VA_ARGS__); putchar('\n'); } } while (0)

/* ---- PS4 network errors: 0x80410100 | errno (BSD numbers), errno also in sceNetErrnoLoc ---- */
static __thread int32_t net_errno_value;
static ABI int32_t *net_errno_loc(void) { return &net_errno_value; }
static int32_t fail(int ps_errno) { net_errno_value = ps_errno; return (int32_t)(0x80410100u | (uint32_t)ps_errno); }
static int host_errno(void) {
#ifdef _WIN32
    switch (WSAGetLastError()) {
    case WSAEWOULDBLOCK: return 35; case WSAEINPROGRESS: return 36; case WSAEALREADY: return 37;
    case WSAENOTSOCK: return 38; case WSAEMSGSIZE: return 40; case WSAEADDRINUSE: return 48;
    case WSAEADDRNOTAVAIL: return 49; case WSAENETDOWN: return 50; case WSAENETUNREACH: return 51;
    case WSAECONNABORTED: return 53; case WSAECONNRESET: return 54; case WSAENOBUFS: return 55;
    case WSAEISCONN: return 56; case WSAENOTCONN: return 57; case WSAETIMEDOUT: return 60;
    case WSAECONNREFUSED: return 61; case WSAEHOSTUNREACH: return 65; case WSAEINVAL: return 22;
    case WSAEBADF: return 9; case WSAEINTR: return 4; default: return 22;
    }
#else
    switch (errno) {
    case EAGAIN: return 35; case EINPROGRESS: return 36; case EALREADY: return 37;
    case ENOTSOCK: return 38; case EMSGSIZE: return 40; case EADDRINUSE: return 48;
    case EADDRNOTAVAIL: return 49; case ENETDOWN: return 50; case ENETUNREACH: return 51;
    case ECONNABORTED: return 53; case ECONNRESET: return 54; case ENOBUFS: return 55;
    case EISCONN: return 56; case ENOTCONN: return 57; case ETIMEDOUT: return 60;
    case ECONNREFUSED: return 61; case EHOSTUNREACH: return 65; case EPIPE: return 32;
    case EINVAL: return 22; case EBADF: return 9; case EINTR: return 4; default: return 22;
    }
#endif
}

/* ---- sockets: PS4 ids (1..MAX_SOCKETS-1) over host sockets ---- */
#define MAX_SOCKETS 256
typedef struct P2pPort P2pPort;
typedef struct P2pPacket P2pPacket;
typedef struct {
    int used, type, nonblocking; HostSocket s; char name[32];
    P2pPort *p2p;
    P2pPacket *head, *tail;
    size_t queued_bytes; unsigned queued_packets;
    uint16_t vport;
    int connected, shutdown_flags;
    unsigned char peer[16];
} Socket;
static Socket sockets[MAX_SOCKETS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Socket *get_socket(int id) { return id > 0 && id < MAX_SOCKETS && sockets[id].used ? &sockets[id] : NULL; }

/* PS4 sockaddr_in: u8 len, u8 family, u16 port, u32 addr, u16 vport, char zero[6]. */
typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint16_t vport; char zero[6]; } PsSockaddrIn;
static void to_host(const void *ps, struct sockaddr_in *h) {
    const PsSockaddrIn *a = ps;
    memset(h, 0, sizeof *h);
    h->sin_family = AF_INET; h->sin_port = a->port; h->sin_addr.s_addr = a->addr;
}
static void to_ps(const struct sockaddr_in *h, void *ps, uint32_t *len) {
    PsSockaddrIn a = {.len = sizeof a, .family = 2, .port = h->sin_port, .addr = h->sin_addr.s_addr};
    if (ps) memcpy(ps, &a, sizeof a);
    if (len) *len = sizeof a;
}
static const char *addr_text(const void *ps) {
    static __thread char text[48];
    const PsSockaddrIn *a = ps;
    char ip[INET_ADDRSTRLEN] = "?";
    if (a) inet_ntop(AF_INET, &a->addr, ip, sizeof ip);
    snprintf(text, sizeof text, "%s:%u", ip, a ? ntohs(a->port) : 0);
    return text;
}

/* The server runs as a normal user: ports below 1024 on it (443, 80) are reached at +40000. */
static void remap_port(struct sockaddr_in *h) {
    if (h->sin_addr.s_addr == server_addr && ntohs(h->sin_port) < 1024)
        h->sin_port = htons((uint16_t)(ntohs(h->sin_port) + 40000));
}
#include "runtime_net_p2p.h"
static ABI int32_t net_init(void) { configure(); return 0; }
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t net_socket(const char *name, int family, int type, int protocol) {
    (void)family;
    const int stream = type == 1 || type == 10; /* STREAM, STREAM_P2P */
    HostSocket s = socket(AF_INET, stream ? SOCK_STREAM : SOCK_DGRAM, stream ? IPPROTO_TCP : IPPROTO_UDP);
    if (s == BAD_SOCKET) return fail(host_errno());
    pthread_mutex_lock(&lock);
    int id = 0;
    for (int i = 1; i < MAX_SOCKETS; ++i) if (!sockets[i].used) { id = i; break; }
    if (id) sockets[id] = (Socket){.used = 1, .type = type, .s = s};
    if (id) snprintf(sockets[id].name, sizeof sockets[id].name, "%s", name ? name : "");
    pthread_mutex_unlock(&lock);
    if (!id) { close_socket(s); return fail(24); }
    LOG("socket \"%s\" type %d proto %d -> %d", name ? name : "", type, protocol, id);
    return id;
}
static ABI int32_t net_close(int id) {
    pthread_mutex_lock(&lock);
    Socket *k = get_socket(id);
    if (k) {
        if (k->p2p) p2p_release_locked(k);
        else close_socket(k->s);
        k->used = 0;
    }
    pthread_mutex_unlock(&lock);
    LOG("close %d", id);
    return k ? 0 : fail(9);
}
static uint32_t local_address(void);
static ABI int32_t net_bind(int id, const void *addr, uint32_t len) {
    Socket *k = get_socket(id); if (!k || !addr) return fail(9);
    if (len < sizeof(PsSockaddrIn)) return fail(22);
    if (k->type == 6) return p2p_bind(id, addr);
    struct sockaddr_in h; to_host(addr, &h);
    /* The game binds to the address netctl reported; listen on every interface instead so the
     * local server (loopback) and the other player (LAN) both reach the socket. */
    if (h.sin_addr.s_addr == local_address()) h.sin_addr.s_addr = htonl(INADDR_ANY);
    const int r = bind(k->s, (struct sockaddr *)&h, sizeof h);
    LOG("bind %d %s -> %d", id, addr_text(addr), r);
    return r == 0 ? 0 : fail(host_errno());
}
static ABI int32_t net_listen(int id, int backlog) {
    Socket *k = get_socket(id); if (!k) return fail(9);
    LOG("listen %d", id);
    return listen(k->s, backlog) == 0 ? 0 : fail(host_errno());
}
static ABI int32_t net_accept(int id, void *addr, uint32_t *len) {
    Socket *k = get_socket(id); if (!k) return fail(9);
    struct sockaddr_in h; socklen_t hl = sizeof h;
    HostSocket s = accept(k->s, (struct sockaddr *)&h, &hl);
    if (s == BAD_SOCKET) return fail(host_errno());
    pthread_mutex_lock(&lock);
    int nid = 0;
    for (int i = 1; i < MAX_SOCKETS; ++i) if (!sockets[i].used) { nid = i; break; }
    if (nid) sockets[nid] = (Socket){.used = 1, .type = k->type, .s = s};
    pthread_mutex_unlock(&lock);
    if (!nid) { close_socket(s); return fail(24); }
    to_ps(&h, addr, len);
    LOG("accept %d -> %d from %s", id, nid, addr_text(addr));
    return nid;
}
static ABI int32_t net_connect(int id, const void *addr, uint32_t len) {
    Socket *k = get_socket(id); if (!k || !addr) return fail(9);
    if (len < sizeof(PsSockaddrIn)) return fail(22);
    if (k->type == 6) {
        int r = p2p_autobind(id);
        if (r) return r;
        pthread_mutex_lock(&lock);
        memcpy(k->peer, addr, sizeof k->peer); k->connected = 1;
        pthread_mutex_unlock(&lock);
        return 0;
    }
    struct sockaddr_in h; to_host(addr, &h);
    remap_port(&h);
    const int r = connect(k->s, (struct sockaddr *)&h, sizeof h);
    const int e = r == 0 ? 0 : host_errno();
    LOG("connect %d %s -> %s", id, addr_text(addr), r == 0 ? "ok" : e == 36 || e == 35 ? "in progress" : "error");
    if (r == 0) return 0;
    return fail(e == 35 ? 36 : e); /* non-blocking connect: EINPROGRESS */
}
static int host_flags(int flags) {
    int f = 0;
    if (flags & 0x2) f |= MSG_PEEK;
#ifndef _WIN32
    if (flags & 0x80) f |= MSG_DONTWAIT;
    if (flags & 0x40) f |= MSG_WAITALL;
    f |= MSG_NOSIGNAL;
#endif
    return f;
}
/* BB_NET_LOG=3: the first bytes of each datagram (peer-to-peer game traffic), in hex. */
static void log_bytes(const char *what, const void *data, size_t size) {
    char hex[3 * 96 + 1]; size_t n = size < 96 ? size : 96;
    for (size_t i = 0; i < n; ++i) snprintf(hex + 3 * i, 4, "%02x ", ((const uint8_t *)data)[i]);
    hex[3 * n] = 0;
    LOG("  %s data (%zu): %s", what, size, hex);
}
/* BB_NET_TRACE_SIZE=N: print guest callers of UDP datagrams exactly N bytes long. */
__attribute__((noinline)) static void net_trace_stack(const char *what, int id, size_t size, const void *peer) {
    if (!net_trace_size || size != net_trace_size) return;
    const uintptr_t lo = 0x800000000ull, hi = lo + 0x50d96dcull;
    const uintptr_t top = platform_thread_stack_top();
    const uintptr_t *sp = (const uintptr_t *)__builtin_frame_address(0);
    char line[1400] = {0}; int n = 0, found = 0;
    for (int i = 0; i < 20000 && (uintptr_t)(sp + i + 1) <= top && found < 36 && n < (int)sizeof line - 24; ++i) {
        if (sp[i] >= lo && sp[i] < hi) {
            n += snprintf(line + n, sizeof line - (size_t)n, " +%#lx", (unsigned long)(sp[i] - lo));
            ++found;
        }
    }
    LOG("trace: UDP %s socket %d peer %s size %zu guest return addresses:%s",
        what, id, peer ? addr_text(peer) : "-", size, found ? line : " (none)");
}
/* BB_NET_TRAP=comp:cmd (hex): when the game sends that Blaze request, log the return addresses
 * into the game's code still on this thread's stack (guest offsets), to find the caller. */
static void net_trap(const void *buf, uint64_t size) {
    static int comp = -1, cmd = -1;
    if (comp == -1) {
        const char *t = getenv("BB_NET_TRAP");
        if (!t || sscanf(t, "%x:%x", (unsigned *)&comp, (unsigned *)&cmd) != 2) comp = -2;
    }
    if (comp < 0 || size < 16) return;
    const uint8_t *b = buf;
    int hit = 0;
    for (uint64_t i = 0; i + 16 <= size && !hit; ++i)
        hit = b[i + 6] == (comp >> 8 & 0xff) && b[i + 7] == (comp & 0xff) && b[i + 8] == (cmd >> 8 & 0xff) &&
              b[i + 9] == (cmd & 0xff) && (b[i + 13] >> 5) == 0 && b[i] == 0 && b[i + 1] == 0;
    if (!hit) return;
    const uintptr_t lo = 0x800000000ull, hi = 0x800000000ull + 0x50d96dcull;
    const uintptr_t top = platform_thread_stack_top();
    const uintptr_t *sp = (const uintptr_t *)__builtin_frame_address(0);
    char line[2048]; int n = 0, found = 0;
    for (int i = 0; i < 20000 && (uintptr_t)(sp + i + 1) <= top && found < 60 && n < (int)sizeof line - 24; ++i)
        if (sp[i] >= lo && sp[i] < hi) { n += snprintf(line + n, sizeof line - n, " +%#lx", (unsigned long)(sp[i] - lo)); ++found; }
    LOG("trap: Blaze %x:%x sent; game return addresses on the stack:%s", comp, cmd, line);
#ifndef _WIN32
    /* BB_NET_TRAP_DUMP=file: also save the game image as it is now (initialized reflection tables,
     * game state), readable regions only, for offline analysis. Once. */
    static int dumped;
    const char *path = getenv("BB_NET_TRAP_DUMP");
    if (path && !dumped) {
        dumped = 1;
        FILE *out = fopen(path, "wb"), *maps = fopen("/proc/self/maps", "r");
        const uintptr_t end = lo + 0x2e3a000ull;
        char row[256];
        uintptr_t at = lo;
        static const char zero[4096];
        while (out && maps && fgets(row, sizeof row, maps)) {
            unsigned long a0, a1; char perm[5];
            if (sscanf(row, "%lx-%lx %4s", &a0, &a1, perm) != 3 || a1 <= lo || a0 >= end) continue;
            if (a0 < lo) a0 = lo;
            if (a1 > end) a1 = end;
            for (; at < a0; at += 4096) fwrite(zero, 1, 4096, out);
            if (perm[0] == 'r') fwrite((const void *)a0, 1, a1 - a0, out);
            else for (uintptr_t z = a0; z < a1; z += 4096) fwrite(zero, 1, 4096, out);
            at = a1;
        }
        if (maps) fclose(maps);
        if (out) fclose(out);
        LOG("trap: game image saved to %s", path);
    }
#endif
}
static ABI int32_t net_send(int id, const void *buf, uint64_t size, int flags) {
    Socket *k = get_socket(id); if (!k) return fail(9);
    net_trap(buf, size);
    if (k->type == 6) return p2p_send(id, buf, size, flags, NULL);
    const long r = send(k->s, buf, (int)size, host_flags(flags));
    if (net_log > 1) LOG("send %d %llu -> %ld", id, (unsigned long long)size, r);
    return r >= 0 ? (int32_t)r : fail(host_errno());
}
static ABI int32_t net_sendto(int id, const void *buf, uint64_t size, int flags, const void *addr, uint32_t len) {
    Socket *k = get_socket(id); if (!k) return fail(9);
    if (addr && len < sizeof(PsSockaddrIn)) return fail(22);
    if (k->type == 6) return p2p_send(id, buf, size, flags, addr);
    if (k->type == 2) net_trace_stack("send", id, (size_t)size, addr);
    struct sockaddr_in h;
    long r;
    if (addr) { to_host(addr, &h); r = sendto(k->s, buf, (int)size, host_flags(flags), (struct sockaddr *)&h, sizeof h); }
    else r = send(k->s, buf, (int)size, host_flags(flags));
    if (net_log > 1) LOG("sendto %d %s %llu -> %ld", id, addr ? addr_text(addr) : "-", (unsigned long long)size, r);
    if (net_log > 2 && r > 0) log_bytes("sendto", buf, (size_t)r);
    return r >= 0 ? (int32_t)r : fail(host_errno());
}
static ABI int32_t net_recv(int id, void *buf, uint64_t size, int flags) {
    Socket *k = get_socket(id); if (!k) return fail(9);
    if (k->type == 6) return p2p_recv(id, buf, size, flags, NULL, NULL);
    const long r = recv(k->s, buf, (int)size, host_flags(flags));
    if (net_log > 1 && r != -1) LOG("recv %d %llu -> %ld", id, (unsigned long long)size, r);
    return r >= 0 ? (int32_t)r : fail(host_errno());
}
static ABI int32_t net_recvfrom(int id, void *buf, uint64_t size, int flags, void *addr, uint32_t *len) {
    Socket *k = get_socket(id); if (!k) return fail(9);
    if (k->type == 6) return p2p_recv(id, buf, size, flags, addr, len);
    struct sockaddr_in h; socklen_t hl = sizeof h;
    const long r = recvfrom(k->s, buf, (int)size, host_flags(flags), (struct sockaddr *)&h, &hl);
    if (r < 0) return fail(host_errno());
    to_ps(&h, addr, len);
    if (k->type == 2) net_trace_stack("recv", id, (size_t)r, addr);
    if (net_log > 1) LOG("recvfrom %d %s -> %ld", id, addr_text(addr), r);
    if (net_log > 2 && r > 0) log_bytes("recvfrom", buf, (size_t)r);
    return (int32_t)r;
}
static ABI int32_t net_shutdown(int id, int how) {
    Socket *k = get_socket(id); if (!k) return fail(9);
    if (how < 0 || how > 2) return fail(22);
    if (k->type == 6) {
        pthread_mutex_lock(&lock);
        k->shutdown_flags |= how == 0 ? 1 : how == 1 ? 2 : 3;
        pthread_mutex_unlock(&lock);
        return 0;
    }
    return shutdown(k->s, how) == 0 ? 0 : fail(host_errno());
}
static int set_nonblocking(Socket *k, int on) {
    k->nonblocking = on;
    if (k->p2p) return 0; /* shared transport stays nonblocking; NBIO belongs to the virtual socket */
#ifdef _WIN32
    u_long v = on ? 1 : 0;
    return ioctlsocket(k->s, FIONBIO, &v);
#else
    const int f = fcntl(k->s, F_GETFL, 0);
    return fcntl(k->s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
#endif
}
static ABI int32_t net_setsockopt(int id, int level, int name, const void *value, uint32_t len) {
    Socket *k = get_socket(id); if (!k) return fail(9);
    const int v = value && len >= 4 ? *(const int *)value : 0;
    int r = 0;
    if (level == 0xffff) {
        switch (name) {
        case 0x1200: r = set_nonblocking(k, v != 0); break;                                   /* NBIO */
        case 0x4: r = setsockopt(k->s, SOL_SOCKET, SO_REUSEADDR, (const char *)&v, sizeof v); break;
        case 0x20: r = setsockopt(k->s, SOL_SOCKET, SO_BROADCAST, (const char *)&v, sizeof v); break;
        case 0x1001: r = setsockopt(k->s, SOL_SOCKET, SO_SNDBUF, (const char *)&v, sizeof v); break;
        case 0x1002: r = setsockopt(k->s, SOL_SOCKET, SO_RCVBUF, (const char *)&v, sizeof v); break;
        case 0x8: r = setsockopt(k->s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&v, sizeof v); break;
        default: break; /* timeouts, crypto, priority, name: accepted, not applied */
        }
    } else if (level == 6 && name == 1) {
        r = setsockopt(k->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&v, sizeof v);
    }
    LOG("setsockopt %d level %#x opt %#x = %d -> %d", id, level, name, v, r);
    return r == 0 ? 0 : fail(host_errno());
}
static ABI int32_t net_getsockopt(int id, int level, int name, void *value, uint32_t *len) {
    Socket *k = get_socket(id); if (!k || !value) return fail(9);
    int v = 0;
    if (level == 0xffff && name == 0x1007) { /* SO_ERROR */
        int e = 0; socklen_t el = sizeof e;
        getsockopt(k->s, SOL_SOCKET, SO_ERROR, (char *)&e, &el);
        if (e) {
#ifdef _WIN32
            WSASetLastError(e);
#else
            errno = e;
#endif
            v = host_errno();
        }
    } else if (level == 0xffff && name == 0x1008) v = k->type == 1 || k->type == 10 ? 1 : 2;
    else if (level == 0xffff && name == 0x1200) v = k->nonblocking;
    *(int *)value = v;
    if (len) *len = 4;
    return 0;
}
static ABI int32_t net_getsockname(int id, void *addr, uint32_t *len) {
    Socket *k = get_socket(id); if (!k) return fail(9);
    struct sockaddr_in h; socklen_t hl = sizeof h;
    if (getsockname(k->s, (struct sockaddr *)&h, &hl) != 0) return fail(host_errno());
    to_ps(&h, addr, len);
    if (k->p2p && addr) ((PsSockaddrIn *)addr)->vport = htons(k->vport);
    return 0;
}
static ABI int32_t net_getpeername(int id, void *addr, uint32_t *len) {
    Socket *k = get_socket(id); if (!k) return fail(9);
    if (k->type == 6) {
        if (!k->connected) return fail(57);
        if (addr) memcpy(addr, k->peer, sizeof k->peer);
        if (len) *len = sizeof k->peer;
        return 0;
    }
    struct sockaddr_in h; socklen_t hl = sizeof h;
    if (getpeername(k->s, (struct sockaddr *)&h, &hl) != 0) return fail(host_errno());
    to_ps(&h, addr, len);
    return 0;
}
static ABI int32_t net_abort(int id, int flags) { (void)flags; LOG("abort %d", id); return 0; }

/* ---- epoll over poll(): PS4 event {u32 events, pad; u64 ident; u64 data} ---- */
#define MAX_EPOLLS 32
#define MAX_WATCH 64
typedef struct { uint32_t events, pad; uint64_t ident, data; } PsEpollEvent;
typedef struct { int used; int count; struct { int id; uint32_t events; uint64_t data; } watch[MAX_WATCH]; } Epoll;
static Epoll epolls[MAX_EPOLLS];
static ABI int32_t net_epoll_create(const char *name, int flags) {
    (void)flags;
    pthread_mutex_lock(&lock);
    int id = 0;
    for (int i = 1; i < MAX_EPOLLS; ++i) if (!epolls[i].used) { id = i; break; }
    if (id) epolls[id] = (Epoll){.used = 1};
    pthread_mutex_unlock(&lock);
    if (net_log > 1) LOG("epoll create \"%s\" -> %d", name ? name : "", id);
    return id ? id : fail(24);
}
static ABI int32_t net_epoll_destroy(int id) {
    if (id > 0 && id < MAX_EPOLLS) epolls[id].used = 0;
    return 0;
}
static ABI int32_t net_epoll_control(int eid, int op, int id, const PsEpollEvent *event) {
    if (eid <= 0 || eid >= MAX_EPOLLS || !epolls[eid].used) return fail(9);
    Epoll *e = &epolls[eid];
    pthread_mutex_lock(&lock);
    int at = -1;
    for (int i = 0; i < e->count; ++i) if (e->watch[i].id == id) at = i;
    int r = 0;
    if (op == 1 || op == 2) { /* ADD, MOD */
        if (at < 0) { if (e->count >= MAX_WATCH) r = fail(24); else at = e->count++; }
        if (at >= 0 && event) { e->watch[at].id = id; e->watch[at].events = event->events; e->watch[at].data = event->data; }
    } else if (op == 3 && at >= 0) {
        e->watch[at] = e->watch[--e->count];
    }
    pthread_mutex_unlock(&lock);
    if (net_log > 1) LOG("epoll %d op %d socket %d events %#x", eid, op, id, event ? event->events : 0);
    return r;
}
static ABI int32_t net_epoll_wait(int eid, PsEpollEvent *events, int max, int timeout_us) {
    if (eid <= 0 || eid >= MAX_EPOLLS || !epolls[eid].used || !events || max <= 0) return fail(22);
    uint64_t deadline = timeout_us < 0 ? UINT64_MAX : platform_monotonic_ns() + (uint64_t)timeout_us * 1000;
    for (;;) {
#ifdef _WIN32
        WSAPOLLFD fds[MAX_WATCH];
#else
        struct pollfd fds[MAX_WATCH];
#endif
        int ids[MAX_WATCH]; uint64_t data[MAX_WATCH]; uint32_t wants[MAX_WATCH];
        int n = 0, queued = 0, has_p2p = 0;
        pthread_mutex_lock(&lock);
        Epoll *e = &epolls[eid];
        if (!e->used) { pthread_mutex_unlock(&lock); return fail(9); }
        for (int i = 0; i < e->count; ++i) {
            Socket *k = get_socket(e->watch[i].id);
            if (!k) continue;
            if (k->p2p) {
                has_p2p = 1;
                p2p_pump_locked(k->p2p);
            }
            fds[n].fd = k->s;
            wants[n] = e->watch[i].events;
            if (k->p2p && k->head && (wants[n] & 1)) queued = 1;
            fds[n].events = (short)(((wants[n] & 1) ? POLLIN : 0) | ((wants[n] & 2) ? POLLOUT : 0));
            fds[n].revents = 0;
            ids[n] = e->watch[i].id; data[n] = e->watch[i].data; ++n;
        }
        pthread_mutex_unlock(&lock);
        uint64_t now = platform_monotonic_ns();
        int ms = deadline == UINT64_MAX ? -1 : now >= deadline ? 0 :
            (int)((deadline - now + 999999) / 1000000);
        if (queued) ms = 0;
        else if (has_p2p && (ms < 0 || ms > 10)) ms = 10;
        int r = 0;
        if (n) {
#ifdef _WIN32
            r = WSAPoll(fds, (ULONG)n, ms);
#else
            r = poll(fds, (nfds_t)n, ms);
#endif
        } else {
            if (ms < 0 || ms > 10) ms = 10;
            if (ms) platform_sleep_ns((uint64_t)ms * 1000000);
        }
        if (r < 0) return fail(host_errno());
        int out = 0;
        pthread_mutex_lock(&lock);
        for (int i = 0; i < n && out < max; ++i) {
            Socket *k = get_socket(ids[i]);
            if (!k || k->s != fds[i].fd) continue;
            uint32_t ev = 0;
            if (k->p2p) {
                p2p_pump_locked(k->p2p);
                if (k->head && (wants[i] & 1)) ev |= 1;
            } else if (fds[i].revents & POLLIN) ev |= 1;
            if (fds[i].revents & POLLOUT) ev |= 2;
            if (fds[i].revents & POLLERR) ev |= 8;
            if (fds[i].revents & POLLHUP) ev |= 0x10;
            if (ev) events[out++] = (PsEpollEvent){.events = ev, .ident = (uint64_t)ids[i], .data = data[i]};
        }
        pthread_mutex_unlock(&lock);
        if (out || (deadline != UINT64_MAX && platform_monotonic_ns() >= deadline)) return out;
    }
}
static ABI int32_t net_epoll_abort(int eid, int flags) { (void)eid; (void)flags; return 0; }

/* ---- resolver: *.ea.com (and the server's own name) -> the server; anything else fails ---- */
static int ends_with(const char *s, const char *suffix) {
    const size_t a = strlen(s), b = strlen(suffix);
    if (a < b) return 0;
    for (size_t i = 0; i < b; ++i) if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suffix[i])) return 0;
    return 1;
}
static int32_t resolver_error;
static ABI int32_t resolver_create(const char *name, int pool, int flags) { (void)name; (void)pool; (void)flags; return 1; }
static ABI int32_t resolver_destroy(int id) { (void)id; return 0; }
static ABI int32_t resolver_ntoa(int id, const char *host, uint32_t *addr, int timeout, int retry, int flags) {
    (void)id; (void)timeout; (void)retry; (void)flags;
    if (!host || !addr) return fail(22);
    struct in_addr literal;
    if (inet_pton(AF_INET, host, &literal) == 1) { *addr = literal.s_addr; return 0; }
    if (ends_with(host, ".ea.com") || ends_with(host, ".ea.net") || !strcmp(host, server_host)) {
        *addr = server_addr;
        LOG("resolve %s -> online server", host);
        return 0;
    }
    LOG("resolve %s -> refused (only ea.com goes to the online server)", host);
    resolver_error = (int32_t)0x804101e2; /* RESOLVER_ENODNS */
    net_errno_value = 0xe2;
    return resolver_error;
}
static ABI int32_t resolver_aton(int id, const uint32_t *addr, char *host, int size, int timeout, int retry, int flags) {
    (void)id; (void)timeout; (void)retry; (void)flags;
    if (!addr || !host || size <= 0) return fail(22);
    inet_ntop(AF_INET, addr, host, (socklen_t)size);
    return 0;
}
static ABI int32_t resolver_get_error(int id, int32_t *result) { (void)id; if (result) *result = resolver_error; return 0; }
static ABI int32_t resolver_abort(int id, int flags) { (void)id; (void)flags; return 0; }

/* ---- NetCtl: connected, with the address of the interface that reaches the server ---- */
static uint32_t route_source(uint32_t to) { /* the interface address a datagram to `to` leaves from */
    uint32_t source = 0;
    HostSocket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in h = {.sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = to};
    if (s != BAD_SOCKET && connect(s, (struct sockaddr *)&h, sizeof h) == 0) { /* sends nothing */
        socklen_t hl = sizeof h;
        if (getsockname(s, (struct sockaddr *)&h, &hl) == 0) source = h.sin_addr.s_addr;
    }
    if (s != BAD_SOCKET) close_socket(s);
    return source;
}
static int is_loopback(uint32_t a) { return (ntohl(a) >> 24) == 127; }
static uint32_t local_address(void) {
    /* The address the other player reaches this PC at. With the server on this PC (127.0.0.1)
     * that is the LAN interface, not loopback; the game announces it to the peer. */
    static uint32_t cached;
    if (cached) return cached;
    cached = route_source(server_addr);
    if (!cached || is_loopback(cached)) {
        const uint32_t lan = route_source(htonl(0xc0a80101)); /* any LAN route: 192.168.1.1 */
        if (lan && !is_loopback(lan)) cached = lan;
    }
    if (!cached) cached = htonl(0x7f000001);
    return cached;
}
static ABI int32_t netctl_init(void) { configure(); return 0; }
static ABI int32_t netctl_term(void) { return 0; }
static ABI int32_t netctl_state(int32_t *state) { if (!state) return (int32_t)0x80412107; *state = 3; return 0; } /* IPOBTAINED */
static ABI int32_t netctl_info(int code, void *info) {
    if (!info) return (int32_t)0x80412107;
    switch (code) {
    case 14: inet_ntop(AF_INET, &(uint32_t){local_address()}, info, 16); break;   /* IP_ADDRESS */
    case 15: snprintf(info, 16, "255.255.255.0"); break;                            /* NETMASK */
    case 16: case 17: inet_ntop(AF_INET, &server_addr, info, 16); break;            /* ROUTE, DNS */
    case 18: snprintf(info, 16, "0.0.0.0"); break;
    case 3: *(uint32_t *)info = 1500; break;                                         /* MTU */
    case 4: *(uint32_t *)info = 1; break;                                            /* LINK: up */
    case 11: *(uint32_t *)info = 0; break;                                           /* IP_CONFIG: DHCP */
    case 1: *(uint32_t *)info = 0; break;                                            /* DEVICE: wired */
    case 2: memset(info, 0, 6); ((uint8_t *)info)[0] = 0x02; ((uint8_t *)info)[5] = 0x33; break; /* ETHER_ADDR */
    default: memset(info, 0, 16); break;
    }
    LOG("netctl info %d", code);
    return 0;
}
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return (int32_t)0x80412107;
    /* {size, stun status, nat type, mapped address} */
    info[1] = 1; info[2] = 2; info[3] = local_address();
    return 0;
}
static ABI int32_t netctl_register(void *cb, void *arg, int32_t *cid) { (void)cb; (void)arg; if (cid) *cid = 1; return 0; }
static ABI int32_t netctl_ok(void) { return 0; }
static ABI int32_t netctl_result(int code, int *result) { (void)code; if (result) *result = 0; return 0; }

/* ---- PSN: signed in, PS Plus, an authorization code the server accepts ---- */
static const char *player_name(void) {
    const char *n = getenv("BB_USER_NAME");
    return n && *n ? n : "Player 1";
}
static uint64_t account_id(void) {
    uint64_t h = 1469598103934665603ull;
    for (const char *p = player_name(); *p; ++p) h = (h ^ (unsigned char)*p) * 1099511628211ull;
    return h & 0x7fffffffffffffffull;
}
static void online_id(char *out20) { /* {char data[16]; s8 term; s8 dummy[3]} */
    memset(out20, 0, 20);
    char clean[17]; int n = 0;
    for (const char *p = player_name(); *p && n < 16; ++p)
        clean[n++] = isalnum((unsigned char)*p) || *p == '-' || *p == '_' ? *p : '_';
    clean[n] = 0;
    memcpy(out20, clean, (size_t)n);
}
static int32_t next_request = 0x20000001;
static ABI int32_t np_state(int32_t user, uint32_t *state) { (void)user; if (state) *state = 2; return 0; }
static ABI int32_t np_online_id(int32_t user, char *id) { (void)user; if (!id) return (int32_t)0x80550003; online_id(id); return 0; }
static ABI int32_t np_np_id(int32_t user, char *id) { (void)user; if (!id) return (int32_t)0x80550003; memset(id, 0, 36); online_id(id); return 0; }
static ABI int32_t np_account_id(int32_t user, uint64_t *id) { (void)user; if (id) *id = account_id(); return 0; }
static ABI int32_t np_country(int32_t user, char *code) { (void)user; if (code) { code[0] = 'u'; code[1] = 's'; code[2] = 0; code[3] = 0; } return 0; }
static ABI int32_t np_request(const void *param) {
    (void)param;
    int32_t id = __atomic_fetch_add(&next_request, 1, __ATOMIC_RELAXED);
    if (net_log) LOG("PSN: request %#x created", id);
    return id;
}
static ABI int32_t np_ok(void) { return 0; }
static ABI int32_t np_user_by_account(uint64_t account, int32_t *user) {
    /* The only signed-in user is the local one (UserService id 1). */
    int mine = account == account_id();
    if (net_log) LOG("PSN: user for account %#llx -> %s", (unsigned long long)account, mine ? "1" : "not signed in here");
    if (!mine) return (int32_t)0x80550006;
    if (user) *user = 1;
    return 0;
}
static ABI int32_t np_poll(int32_t req, int32_t *result) { if (net_log > 1) LOG("PSN: poll %#x", req); if (result) *result = 0; return 0; }
static ABI int32_t np_wait(int32_t req, int32_t *result) { (void)req; if (result) *result = 0; return 0; }
static ABI int32_t np_check_plus(int32_t req, const void *param, uint8_t *result) {
    (void)req; (void)param;
    if (result) { memset(result, 0, 33); result[0] = 1; }
    LOG("PSN: PS Plus check -> authorized");
    return 0;
}
static ABI int32_t np_parental(int32_t req, int32_t user, int8_t *age, uint8_t *info) {
    (void)req; (void)user;
    if (age) *age = 30;
    if (info) memset(info, 0, 3);
    if (net_log) LOG("PSN: parental control -> unrestricted, age 30");
    return 0;
}
static ABI int32_t np_availability(int32_t req, int32_t user) { (void)req; (void)user; LOG("PSN: availability -> ok"); return 0; }
static ABI int32_t np_auth_code(int32_t req, const void *param, char *code, int32_t *issuer) {
    (void)req; (void)param;
    if (code) { memset(code, 0, 136); snprintf(code, 129, "UFC3NATIVE-%016llx", (unsigned long long)account_id()); }
    if (issuer) *issuer = 256;
    LOG("PSN: authorization code for %s", player_name());
    return 0;
}
/* The same answer, logged: which PSN web API the game wanted (sceNpWebApiCreateRequest:
 * user context, api group, path, method). */
static ABI int32_t np_webapi_context(int32_t lib, int32_t user) {
    /* A signed-out context stops the game before it says which profile or
     * session it wanted. A local id lets CreateRequest log the path. */
    if (net_log) LOG("PSN: web API context for library %d user %d -> 1", lib, user);
    return 1;
}
typedef struct {
    int used;
    int64_t id;
    char body[1024];
    size_t len, read_at;
} WebReq;
static WebReq web_reqs[8];

static const char *peer_name(void) {
    const char *n = getenv("BB_PEER_NAME");
    return n && *n ? n : "Rival";
}
static void profile_json(char *out, size_t cap, const char *ids) {
    /* One object per account id. The local id uses this PC's player name. */
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n, "{\"profiles\":[");
    int first = 1;
    const char *p = ids;
    while (*p && n + 160 < cap) {
        char tok[32]; int k = 0;
        while (*p && *p != ',' && *p != '&' && k < 31) tok[k++] = *p++;
        tok[k] = 0;
        if (*p == ',') ++p;
        if (!k) break;
        unsigned long long id = strtoull(tok, NULL, 10);
        const char *name = id == account_id() ? player_name() : peer_name();
        n += (size_t)snprintf(out + n, cap - n,
            "%s{\"accountId\":\"%s\",\"onlineId\":\"%s\",\"personalDetail\":{\"displayName\":\"%s\"}}",
            first ? "" : ",", tok, name, name);
        first = 0;
        if (*p == '&' || *p == '?') break;
    }
    snprintf(out + n, cap - n, "]}");
}
static ABI int32_t np_webapi_request(int64_t ctx, const char *group, const char *path, int32_t method,
                                      uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)ctx;
    if (net_log) LOG("PSN: web API request %s %s (method %d) a5=%#llx a6=%#llx a7=%#llx",
                     group ? group : "-", path ? path : "-", method,
                     (unsigned long long)a5, (unsigned long long)a6, (unsigned long long)a7);
    if (!group || !path || !strstr(group, "userProfile")) return (int32_t)0x80550006;
    WebReq *slot = NULL;
    for (int i = 0; i < 8; ++i) if (!web_reqs[i].used) { slot = &web_reqs[i]; break; }
    if (!slot) return (int32_t)0x80550006;
    int64_t id = __atomic_fetch_add(&next_request, 1, __ATOMIC_RELAXED);
    *slot = (WebReq){.used = 1, .id = id};
    if (strstr(path, "/profiles")) {
        const char *ids = strstr(path, "accountIds=");
        profile_json(slot->body, sizeof slot->body, ids ? ids + 11 : "");
    } else snprintf(slot->body, sizeof slot->body, "{}");
    slot->len = strlen(slot->body);
    /* Argument 6 is the int64 request id the game reads back. It sits in guest memory. */
    int stored = 0;
    if (a6 > 0x10000 && (a6 & 7) == 0) { *(int64_t *)a6 = id; stored = 1; }
    if (net_log) LOG("PSN: profile stored id %lld (%s), %zu bytes", (long long)id, stored ? "ok" : "nowhere", slot->len);
    return 0;
}
static WebReq *web_req(int64_t id) {
    for (int i = 0; i < 8; ++i) if (web_reqs[i].used && web_reqs[i].id == id) return &web_reqs[i];
    return NULL;
}
static ABI int32_t np_webapi_send2(int64_t id, const void *data, uint64_t size, int32_t *option) {
    (void)data; (void)size;
    if (net_log) LOG("PSN: web API send %lld option %p", (long long)id, (void *)option);
    if (!web_req(id)) return (int32_t)0x80550006;
    if ((uintptr_t)option > 0x10000) *option = 200; /* httpStatus is the first field */
    return 0;
}
static ABI int32_t np_webapi_read(int64_t id, void *buf, uint64_t size) {
    WebReq *r = web_req(id);
    if (!r || !buf) return (int32_t)0x80550006;
    size_t left = r->read_at < r->len ? r->len - r->read_at : 0;
    size_t n = size < left ? (size_t)size : left;
    if (n) memcpy(buf, r->body + r->read_at, n);
    r->read_at += n;
    if (net_log) LOG("PSN: web API read %lld -> %zu", (long long)id, n);
    return (int32_t)n;
}
static ABI int32_t np_webapi_delete_request(int64_t id) {
    WebReq *r = web_req(id);
    if (r) r->used = 0;
    return 0;
}

/* ---- sceHttp: plain HTTP/1.0 to the online server ---- */
#define MAX_HTTP 64
typedef struct {
    int used, kind; /* 1 template, 2 connection, 3 request */
    int parent;
    char url[512], method[16], headers[2048];
    char *response; size_t response_size, body_at, read_at;
    int status, sent, epoll, nonblocking, redirect_pending; void *user_arg;
    int32_t transport_error;
    pthread_t worker; int worker_started;
    void *redirect_cb, *redirect_arg; /* sceHttpSetRedirectCallback, inherited from the parents */
} Http;
typedef int32_t (ABI *HttpRedirectCallback)(int32_t request, int32_t status, int32_t *method,
                                             const char *location, void *arg);
static Http https[MAX_HTTP];
typedef struct { uint32_t events, detail; int id; void *user_arg; } HttpEvent;
static int http_new(int kind, int parent) {
    pthread_mutex_lock(&lock);
    int id = 0;
    for (int i = 1; i < MAX_HTTP; ++i) if (!https[i].used) { id = i; break; }
    if (id) { memset(&https[id], 0, sizeof https[id]); https[id].used = 1; https[id].kind = kind; https[id].parent = parent; }
    pthread_mutex_unlock(&lock);
    return id ? id : (int32_t)0x80431022;
}
static Http *http_get(int id) { return id > 0 && id < MAX_HTTP && https[id].used ? &https[id] : NULL; }
static int32_t http_ready_error(Http *h, int id) {
    (void)id;
    const int state = __atomic_load_n(&h->sent, __ATOMIC_ACQUIRE);
    if (state < 0) return (int32_t)0x80431082; /* EAGAIN: asynchronous operation pending */
    if (state == 0) return (int32_t)0x80431065; /* BEFORE_SEND */
    return h->transport_error;
}
static int http_nonblocking(Http *h) {
    for (Http *at = h; at; at = http_get(at->parent)) {
        if (at->nonblocking) return 1;
        if (!at->parent) break;
    }
    return 0;
}
static void http_dispatch_redirect(int id) {
    Http *h = http_get(id);
    if (!h || !h->redirect_pending) return;
    /* Clear first: ProtoHttp's callback queries headers on this same request. */
    h->redirect_pending = 0;
    void *cb = NULL, *arg = NULL;
    for (Http *at = h; at && !cb; at = http_get(at->parent)) {
        cb = at->redirect_cb; arg = at->redirect_arg;
        if (!at->parent) break;
    }
    char location[1024] = "";
    char *buf = h->response;
    for (char *line = strstr(buf, "\r\n"); line && line < buf + h->body_at;
         line = strstr(line + 2, "\r\n")) {
        if (strncasecmp(line + 2, "Location:", 9)) continue;
        const char *v = line + 11;
        while (*v == ' ' || *v == '\t') ++v;
        const char *e = strstr(v, "\r\n");
        snprintf(location, sizeof location, "%.*s", (int)(e ? e - v : (long)strlen(v)), v);
        break;
    }
    if (cb && location[0]) {
        int32_t method = !strcmp(h->method, "POST") ? 1 : 0;
        const int32_t result = ((HttpRedirectCallback)cb)(id, h->status, &method, location, arg);
        LOG("http %d: redirect callback -> %d", id, result);
    }
}
static ABI int32_t http_init(int mem, int ssl, uint64_t size) { (void)mem; (void)ssl; (void)size; configure(); return 1; }
static ABI int32_t http_template(int ctx, const char *agent, int ver, int proxy) { (void)ctx; (void)ver; (void)proxy; LOG("http template agent \"%s\"", agent ? agent : ""); return http_new(1, 0); }
static ABI int32_t http_connection(int tmpl, const char *url, int keep) {
    (void)keep; int id = http_new(2, tmpl);
    if (id > 0 && url) snprintf(https[id].url, sizeof https[id].url, "%s", url);
    return id;
}
static ABI int32_t http_request2(int conn, const char *method, const char *url, uint64_t length) {
    (void)length; int id = http_new(3, conn);
    if (id > 0) {
        snprintf(https[id].method, sizeof https[id].method, "%s", method ? method : "GET");
        snprintf(https[id].url, sizeof https[id].url, "%s", url ? url : "");
        const char *query = strchr(https[id].url, '?');
        LOG("http request %d %s %.*s", id, https[id].method,
            (int)(query ? query - https[id].url : (long)strlen(https[id].url)), https[id].url);
    }
    return id;
}
static ABI int32_t http_request(int conn, int method, const char *url, uint64_t length) {
    static const char *const names[] = {"GET", "POST", "HEAD", "OPTIONS", "PUT", "DELETE", "TRACE", "CONNECT"};
    return http_request2(conn, method >= 0 && method < 8 ? names[method] : "GET", url, length);
}
static ABI int32_t http_add_header(int id, const char *name, const char *value, int mode) {
    (void)mode; Http *h = http_get(id); if (!h || !name) return (int32_t)0x80431022;
    const size_t at = strlen(h->headers);
    snprintf(h->headers + at, sizeof h->headers - at, "%s: %s\r\n", name, value ? value : "");
    return 0;
}
static int http_connect(HostSocket s, const struct sockaddr_in *addr) {
    Socket temporary = {.s = s};
    if (set_nonblocking(&temporary, 1)) return -1;
    int result = connect(s, (const struct sockaddr *)addr, sizeof *addr);
    if (result != 0) {
        int error = host_errno();
        if (error != 35 && error != 36) return -1;
#ifdef _WIN32
        WSAPOLLFD fd = {.fd = s, .events = POLLOUT};
        result = WSAPoll(&fd, 1, 5000);
#else
        struct pollfd fd = {.fd = s, .events = POLLOUT};
        result = poll(&fd, 1, 5000);
#endif
        int pending = 0; socklen_t length = sizeof pending;
        if (result <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&pending, &length) || pending) return -1;
    }
    if (set_nonblocking(&temporary, 0)) return -1;
#ifdef _WIN32
    const DWORD timeout = 5000;
#else
    const struct timeval timeout = {.tv_sec = 5};
#endif
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout) ||
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout)) return -1;
    return 0;
}
static int http_send_all(HostSocket s, const void *data, uint64_t size) {
    const char *bytes = data;
    while (size) {
        int count = size > 65536 ? 65536 : (int)size;
        const long sent = send(s, bytes, count, host_flags(0));
        if (sent <= 0) return -1;
        bytes += sent; size -= (uint64_t)sent;
    }
    return 0;
}
static int32_t http_perform(int id, const void *body, uint64_t size) {
    Http *h = http_get(id); if (!h) return (int32_t)0x80431022;
    /* scheme://host[:port]/path; the server's plain-HTTP listener has the URL's port. */
    const char *url = h->url, *p = strstr(url, "://");
    const int https_scheme = !strncmp(url, "https", 5);
    p = p ? p + 3 : url;
    char host[256]; int port = https_scheme ? 443 : 80;
    const char *slash = strchr(p, '/'), *colon = strchr(p, ':');
    const size_t host_len = (size_t)((colon && (!slash || colon < slash) ? colon : slash ? slash : p + strlen(p)) - p);
    snprintf(host, sizeof host, "%.*s", (int)host_len, p);
    if (colon && (!slash || colon < slash)) port = atoi(colon + 1);
    const char *path = slash ? slash : "/";
    uint32_t addr = 0;
    if (resolver_ntoa(1, host, &addr, 0, 0, 0) != 0) { h->status = 0; LOG("http %d: %s refused", id, host); return (int32_t)0x80431063; }
    HostSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = addr};
    remap_port(&a);
    if (s == BAD_SOCKET || http_connect(s, &a) != 0) {
        if (s != BAD_SOCKET) close_socket(s);
        LOG("http %d: cannot reach the online server on port %d", id, port);
        return (int32_t)0x80431063;
    }
    char head[4096];
    char psn_name[20]; online_id(psn_name);
    /* Metadata for our local PSN replacement. It binds OAuth and Blaze to the
     * same simulated account without relying on the TCP peer's IP (NAT). */
    const int n = snprintf(head, sizeof head, "%s %s HTTP/1.0\r\nHost: %s\r\n%s"
        "X-UFC3-Psn-Id: %llu\r\nX-UFC3-Online-Id: %s\r\nContent-Length: %llu\r\nConnection: close\r\n\r\n",
        h->method, path, host, h->headers, (unsigned long long)account_id(), psn_name,
        (unsigned long long)(body ? size : 0));
    if (n < 0 || (size_t)n >= sizeof head || http_send_all(s, head, (uint64_t)n) ||
        (body && size && http_send_all(s, body, size))) {
        close_socket(s);
        return (int32_t)0x80431063;
    }
    size_t cap = 65536, used = 0; char *buf = malloc(cap);
    if (!buf) { close_socket(s); return (int32_t)0x80431022; }
    for (;;) {
        if (used + 4096 > cap) {
            if (cap >= 2 * 1024 * 1024) { free(buf); close_socket(s); return (int32_t)0x80431022; }
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) { free(buf); close_socket(s); return (int32_t)0x80431022; }
            buf = grown;
        }
        const long r = recv(s, buf + used, (int)(cap - used - 1), 0);
        if (r < 0) { free(buf); close_socket(s); return (int32_t)0x80431063; }
        if (r == 0) break;
        used += (size_t)r;
    }
    close_socket(s);
    buf[used] = 0;
    h->response = buf; h->response_size = used;
    char *end = strstr(buf, "\r\n\r\n");
    h->body_at = end ? (size_t)(end - buf) + 4 : used;
    h->status = used > 12 ? atoi(buf + 9) : 0;
    const char *query = strchr(path, '?');
    LOG("http %d: %s %s:%d%.*s -> %d, %zu bytes", id, h->method, host, port,
        (int)(query ? query - path : (long)strlen(path)), path, h->status, used - h->body_at);
    /* The local redirect URI is consumed by the game. SendRequest delivers its callback when
     * the nonblocking caller retries a completed transfer, before returning success. */
    if (h->status == 301 || h->status == 302 || h->status == 303 || h->status == 307) {
        h->redirect_pending = 1;
    }
    if (h->epoll) {
        /* Non-blocking callers wait for IN on the epoll set with sceHttpSetEpoll. */
    }
    return 0;
}
typedef struct { int id; void *body; uint64_t size; } HttpWork;
static void *http_worker(void *ptr) {
    HttpWork *work = ptr;
    Http *h = http_get(work->id);
    h->transport_error = http_perform(work->id, work->body, work->size);
    free(work->body);
    free(work);
    /* Publishes the response as well as its status/error to polling threads. */
    __atomic_store_n(&h->sent, 1, __ATOMIC_RELEASE);
    return NULL;
}
static ABI int32_t http_send(int id, const void *body, uint64_t size) {
    Http *h = http_get(id); if (!h) return (int32_t)0x80431022;
    const int state = __atomic_load_n(&h->sent, __ATOMIC_ACQUIRE);
    if (state < 0) return (int32_t)0x80431082;
    if (state > 0) {
        const int32_t error = h->transport_error;
        if (!error) http_dispatch_redirect(id);
        return error;
    }
    if (http_nonblocking(h)) {
        if (size > 1024 * 1024) return (int32_t)0x80431022;
        HttpWork *work = calloc(1, sizeof *work);
        if (!work) return (int32_t)0x80431022;
        work->id = id;
        work->size = body ? size : 0;
        if (work->size) {
            work->body = malloc((size_t)work->size);
            if (!work->body) { free(work); return (int32_t)0x80431022; }
            memcpy(work->body, body, (size_t)work->size);
        }
        __atomic_store_n(&h->sent, -1, __ATOMIC_RELEASE);
        if (pthread_create(&h->worker, NULL, http_worker, work) != 0) {
            __atomic_store_n(&h->sent, 0, __ATOMIC_RELEASE);
            free(work->body); free(work);
            return (int32_t)0x80431022;
        }
        h->worker_started = 1;
        return (int32_t)0x80431082;
    }
    h->transport_error = http_perform(id, body, size);
    __atomic_store_n(&h->sent, 1, __ATOMIC_RELEASE);
    const int32_t error = h->transport_error;
    if (!error) http_dispatch_redirect(id);
    return error;
}
static ABI int32_t http_status(int id, int *status) {
    Http *h = http_get(id); if (!h || !status) return (int32_t)0x80431022;
    const int32_t error = http_ready_error(h, id);
    /* UFC's ProtoHttpPS4 explicitly retries BEFORE_SEND while waiting for the status line;
     * it treats EAGAIN here as fatal (guest +0x131499a). Body reads still use EAGAIN. */
    if (error) return (uint32_t)error == 0x80431082 ? (int32_t)0x80431065 : error;
    h = http_get(id); if (!h) return (int32_t)0x80431022;
    *status = h->status;
    LOG("http %d: status %d asked", id, h->status);
    return 0;
}
static ABI int32_t http_length(int id, int *result, uint64_t *length) {
    Http *h = http_get(id); if (!h) return (int32_t)0x80431022;
    const int32_t error = http_ready_error(h, id); if (error) return error;
    if (result) *result = 0;
    if (length) *length = h->response ? h->response_size - h->body_at : 0;
    return 0;
}
static ABI int32_t http_read(int id, void *data, uint64_t size) {
    Http *h = http_get(id); if (!h) return (int32_t)0x80431022;
    const int32_t error = http_ready_error(h, id); if (error) return error;
    if (!h->response) return h->transport_error ? h->transport_error : (int32_t)0x80431022;
    const size_t left = h->response_size - h->body_at - h->read_at;
    const size_t n = left < size ? left : (size_t)size;
    memcpy(data, h->response + h->body_at + h->read_at, n);
    h->read_at += n;
    LOG("http %d: read %zu", id, n);
    return (int32_t)n;
}
static ABI int32_t http_all_headers(int id, char **header, uint64_t *size) {
    Http *h = http_get(id); if (!h) return (int32_t)0x80431022;
    const int32_t error = http_ready_error(h, id); if (error) return error;
    if (!h->response) return h->transport_error ? h->transport_error : (int32_t)0x80431022;
    if (header) *header = h->response;
    if (size) *size = h->body_at;
    LOG("http %d: headers asked (%zu bytes)", id, h->body_at);
    return 0;
}
static ABI int32_t http_parse_header(const char *header, uint64_t size, const char *field,
                                    const char **value, uint64_t *length) {
    if (!header) return (int32_t)0x80432060;
    if (!field || !value || !length || !*field) return (int32_t)0x804321fe;
    const size_t field_size = strlen(field);
    for (uint64_t at = 0; at < size;) {
        uint64_t end = at;
        while (end < size && header[end] != '\n') ++end;
        if (end - at > field_size && header[at + field_size] == ':' &&
            !strncasecmp(header + at, field, field_size)) {
            uint64_t first = at + field_size + 1, last = end;
            while (first < last && (header[first] == ' ' || header[first] == '\t')) ++first;
            if (last > first && header[last - 1] == '\r') --last;
            *value = header + first;
            *length = last - first;
            LOG("http parse header %s: found (%llu bytes)", field, (unsigned long long)*length);
            return 0;
        }
        at = end + 1;
    }
    LOG("http parse header %s: not found", field);
    return (int32_t)0x80432025;
}
static ABI int32_t http_delete(int id) {
    Http *h = http_get(id);
    if (h) {
        if (h->worker_started) pthread_join(h->worker, NULL);
        free(h->response); h->used = 0;
    }
    LOG("http %d: deleted", id);
    return 0;
}
static ABI int32_t http_ok(void) { return 0; }
static ABI int32_t http_set_nonblock(int id, int enabled) {
    Http *h = http_get(id); if (!h) return (int32_t)0x80431022;
    h->nonblocking = enabled != 0;
    LOG("http %d: nonblocking %d", id, h->nonblocking);
    return 0;
}
static ABI int32_t http_set_redirect(int id, void *cb, void *arg) {
    Http *h = http_get(id); if (!h) return (int32_t)0x80431022;
    h->redirect_cb = cb; h->redirect_arg = arg;
    LOG("http %d: redirect callback set", id);
    return 0;
}
static ABI int32_t http_epoll_create(int ctx, void **handle) { (void)ctx; if (handle) *handle = (void *)(uintptr_t)0x1000; return 0; }
static ABI int32_t http_set_epoll(int id, void *handle, void *arg) { Http *h = http_get(id); if (h) { h->epoll = 1; h->user_arg = arg; } (void)handle; return 0; }
static ABI int32_t http_wait(void *handle, HttpEvent *events, int max, int32_t timeout) {
    (void)handle;
    int n = 0;
    pthread_mutex_lock(&lock);
    for (int i = 1; i < MAX_HTTP && n < max; ++i)
        if (https[i].used && https[i].kind == 3 && https[i].epoll &&
            __atomic_load_n(&https[i].sent, __ATOMIC_ACQUIRE) == 1) {
            const uint32_t bits = https[i].transport_error ? 8u : 0x10003u;
            events[n++] = (HttpEvent){.events = bits, .detail = bits,
                                      .id = i, .user_arg = https[i].user_arg};
            __atomic_store_n(&https[i].sent, 2, __ATOMIC_RELEASE); /* reported once */
        }
    pthread_mutex_unlock(&lock);
    if (!n && timeout > 0) platform_sleep_ns((uint64_t)(timeout < 100000 ? timeout : 100000) * 1000ull);
    return n;
}
static ABI int32_t http_errno(int id, int *err) { (void)id; if (err) *err = 0; return 0; }

static const RuntimeExport exports[] = {
    {"sceNetInit", net_init}, {"sceNetTerm", net_term}, {"sceNetErrnoLoc", net_errno_loc},
    {"sceNetSocket", net_socket}, {"sceNetSocketClose", net_close}, {"sceNetBind", net_bind},
    {"sceNetListen", net_listen}, {"sceNetAccept", net_accept}, {"sceNetConnect", net_connect},
    {"sceNetSend", net_send}, {"sceNetSendto", net_sendto}, {"sceNetRecv", net_recv},
    {"sceNetRecvfrom", net_recvfrom}, {"sceNetShutdown", net_shutdown}, {"sceNetSetsockopt", net_setsockopt},
    {"sceNetGetsockopt", net_getsockopt}, {"sceNetGetsockname", net_getsockname},
    {"sceNetGetpeername", net_getpeername}, {"sceNetSocketAbort", net_abort},
    {"sceNetEpollCreate", net_epoll_create}, {"sceNetEpollDestroy", net_epoll_destroy},
    {"sceNetEpollControl", net_epoll_control}, {"sceNetEpollWait", net_epoll_wait},
    {"sceNetEpollAbort", net_epoll_abort},
    {"sceNetResolverCreate", resolver_create}, {"sceNetResolverDestroy", resolver_destroy},
    {"sceNetResolverStartNtoa", resolver_ntoa}, {"sceNetResolverStartAton", resolver_aton},
    {"sceNetResolverGetError", resolver_get_error}, {"sceNetResolverAbort", resolver_abort},
    {"sceNetCtlInit", netctl_init}, {"sceNetCtlTerm", netctl_term}, {"sceNetCtlGetState", netctl_state},
    {"sceNetCtlGetInfo", netctl_info}, {"sceNetCtlGetNatInfo", netctl_nat},
    {"sceNetCtlRegisterCallback", netctl_register}, {"sceNetCtlCheckCallback", netctl_ok},
    {"sceNetCtlUnregisterCallback", netctl_ok}, {"sceNetCtlGetResult", netctl_result},
    {"sceNetCtlRegisterCallbackForNpToolkit", netctl_register}, {"sceNetCtlCheckCallbackForNpToolkit", netctl_ok},
    {"sceNetCtlUnregisterCallbackForNpToolkit", netctl_ok},
    {"sceNpGetState", np_state}, {"sceNpGetOnlineId", np_online_id}, {"sceNpGetNpId", np_np_id},
    {"sceNpGetAccountIdA", np_account_id}, {"sceNpGetAccountCountryA", np_country},
    {"sceNpCreateAsyncRequest", np_request}, {"sceNpCreateRequest", np_request}, {"sceNpDeleteRequest", np_ok},
    {"sceNpAbortRequest", np_ok}, {"sceNpPollAsync", np_poll}, {"sceNpWaitAsync", np_wait},
    {"sceNpCheckNpAvailabilityA", np_availability}, {"sceNpCheckNpAvailability", np_availability},
    {"sceNpCheckPlus", np_check_plus}, {"sceNpGetParentalControlInfoA", np_parental},
    {"sceNpGetUserIdByAccountId", np_user_by_account},
    {"sceNpAuthCreateAsyncRequest", np_request}, {"sceNpAuthDeleteRequest", np_ok},
    {"sceNpAuthAbortRequest", np_ok}, {"sceNpAuthPollAsync", np_poll}, {"sceNpAuthWaitAsync", np_wait},
    {"sceNpAuthGetAuthorizationCodeA", np_auth_code}, {"sceNpAuthGetAuthorizationCode", np_auth_code},
    {"sceNpWebApiCreateContext", np_webapi_context}, {"sceNpWebApiCreateContextA", np_webapi_context},
    {"sceNpWebApiCreateRequest", np_webapi_request}, {"sceNpWebApiDeleteRequest", np_webapi_delete_request},
    {"sceNpWebApiReadData", np_webapi_read}, {"sceNpWebApiSendRequest", np_webapi_send2},
    {"sceNpWebApiSendRequest2", np_webapi_send2},
    {"sceHttpInit", http_init}, {"sceHttpCreateTemplate", http_template},
    {"sceHttpCreateConnectionWithURL", http_connection}, {"sceHttpCreateRequestWithURL", http_request},
    {"sceHttpCreateRequestWithURL2", http_request2}, {"sceHttpAddRequestHeader", http_add_header},
    {"sceHttpSendRequest", http_send}, {"sceHttpGetStatusCode", http_status},
    {"sceHttpGetResponseContentLength", http_length}, {"sceHttpReadData", http_read},
    {"sceHttpGetAllResponseHeaders", http_all_headers}, {"sceHttpDeleteRequest", http_delete},
    {"sceHttpParseResponseHeader", http_parse_header},
    {"sceHttpDeleteConnection", http_delete}, {"sceHttpDeleteTemplate", http_delete},
    {"sceHttpCreateEpoll", http_epoll_create}, {"sceHttpSetEpoll", http_set_epoll},
    {"sceHttpUnsetEpoll", http_ok}, {"sceHttpWaitRequest", http_wait}, {"sceHttpSetNonblock", http_set_nonblock},
    {"sceHttpGetLastErrno", http_errno}, {"sceHttpSetRedirectCallback", http_set_redirect}, {"sceHttpsGetSslError", http_errno},
};

uintptr_t runtime_net_resolve(const char *name) {
    configure();
    if (!online) return 0;
    return RUNTIME_LOOKUP(exports, name);
}
