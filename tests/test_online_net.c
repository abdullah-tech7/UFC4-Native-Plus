/* SPDX-License-Identifier: GPL-2.0-or-later
 * Exercises the PS4 network ABI without linking the renderer or running the game.
 * --listen PORT / --peer IP PORT also allow Linux/Windows transport checks.
 * Passing these checks does not prove UFC's login/matchmaking/simulation works. */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}
int platform_sleep_ns(uint64_t ns) {
    struct timespec t = {.tv_sec = (time_t)(ns / 1000000000), .tv_nsec = (long)(ns % 1000000000)};
    return nanosleep(&t, NULL);
}
void runtime_thread_attach_host(const char *name) { (void)name; }
/* Stack scanning is only an opt-in diagnostic in the full runtime; this ABI test never enables it. */
uintptr_t platform_thread_stack_top(void) { return 0; }
uint64_t platform_monotonic_ns(void) {
#ifdef _WIN32
    return GetTickCount64() * 1000000ull;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint16_t vport; char zero[6]; } Address;
typedef int32_t (ABI *Create)(const char *, int, int, int);
typedef int32_t (ABI *Bind)(int, const void *, uint32_t);
typedef int32_t (ABI *Name)(int, void *, uint32_t *);
typedef int32_t (ABI *SendTo)(int, const void *, uint64_t, int, const void *, uint32_t);
typedef int32_t (ABI *RecvFrom)(int, void *, uint64_t, int, void *, uint32_t *);
typedef int32_t (ABI *Option)(int, int, int, const void *, uint32_t);
typedef int32_t (ABI *Close)(int);
typedef int32_t (ABI *Shutdown)(int, int);
typedef int32_t *(ABI *ErrnoLoc)(void);
typedef int32_t (ABI *HttpTemplate)(int, const char *, int, int);
typedef int32_t (ABI *HttpConnection)(int, const char *, int);
typedef int32_t (ABI *HttpRequest)(int, const char *, const char *, uint64_t);
typedef int32_t (ABI *HttpSend)(int, const void *, uint64_t);
typedef int32_t (ABI *HttpStatus)(int, int *);
typedef int32_t (ABI *HttpHeader)(int, char **, uint64_t *);
typedef int32_t (ABI *HttpCallback)(int, void *, void *);
typedef int32_t (ABI *HttpNonblock)(int, int);
typedef int32_t (ABI *HttpParse)(const char *, uint64_t, const char *, const char **, uint64_t *);
typedef struct { uint32_t events, detail; int id; void *arg; } HttpEvent;
typedef int32_t (ABI *HttpEpollCreate)(int, void **);
typedef int32_t (ABI *HttpEpollSet)(int, void *, void *);
typedef int32_t (ABI *HttpWait)(void *, HttpEvent *, int, int32_t);
#define GET(type, name) ((type)runtime_net_resolve(name))
typedef struct { uint32_t events, pad; uint64_t ident, data; } NetEvent;
typedef int32_t (ABI *EpollCreate)(const char *, int);
typedef int32_t (ABI *EpollControl)(int, int, int, const NetEvent *);
typedef int32_t (ABI *EpollWait)(int, NetEvent *, int, int);
static int socket_udp(const char *ip, int port, Address *address);

static int socket_p2p(Address *addr, unsigned vport) {
    int id = GET(Create, "sceNetSocket")("virtual P2P", 2, 6, 0);
    assert(id > 0);
    addr->vport = htons((uint16_t)vport);
    assert(GET(Bind, "sceNetBind")(id, addr, sizeof *addr) == 0);
    uint32_t len = sizeof *addr;
    assert(GET(Name, "sceNetGetsockname")(id, addr, &len) == 0);
    assert(addr->vport == htons((uint16_t)vport));
    int enabled = 1;
    assert(GET(Option, "sceNetSetsockopt")(id, 0xffff, 0x1200, &enabled, 4) == 0);
    return id;
}

static void p2p_virtual_ports(void) {
    Address local = {.len = 16, .family = 2};
    assert(inet_pton(AF_INET, "127.0.0.1", &local.addr) == 1);
    int a = socket_p2p(&local, 30);
    Address other = local;
    int b = socket_p2p(&other, 400);
    assert(local.port == other.port);
    int duplicate = GET(Create, "sceNetSocket")("duplicate", 2, 6, 0);
    assert((uint32_t)GET(Bind, "sceNetBind")(duplicate, &other, 16) == 0x80410130);
    assert(GET(Close, "sceNetSocketClose")(duplicate) == 0);
    int ep = GET(EpollCreate, "sceNetEpollCreate")("virtual readiness", 0);
    NetEvent event = {.events = 1, .data = 30};
    assert(GET(EpollControl, "sceNetEpollControl")(ep, 1, a, &event) == 0);
    event.data = 400;
    assert(GET(EpollControl, "sceNetEpollControl")(ep, 1, b, &event) == 0);
    Address raw, from; uint32_t len = 16; char buffer[64];
    int udp = socket_udp("127.0.0.1", 0, &raw);
    /* Test actual wire format with a plain UDP peer, independently of our encoder. */
    unsigned char wire[] = {0xff, 0x83, 0x01, 0x2c, 0x01, 0x90, 'b'};
    assert(GET(SendTo, "sceNetSendto")(udp, wire, sizeof wire, 0, &local, 16) == sizeof wire);
    NetEvent ready[2];
    assert(GET(EpollWait, "sceNetEpollWait")(ep, ready, 2, 100000) == 1);
    assert(ready[0].ident == (uint64_t)b && ready[0].data == 400 && ready[0].events == 1);
    assert((uint32_t)GET(RecvFrom, "sceNetRecvfrom")(a, buffer, sizeof buffer, 0, NULL, NULL) == 0x80410123);
    assert(GET(RecvFrom, "sceNetRecvfrom")(b, buffer, sizeof buffer, 2, &from, &len) == 1 && buffer[0] == 'b');
    assert(from.port == raw.port && ntohs(from.vport) == 300);
    assert(GET(RecvFrom, "sceNetRecvfrom")(b, buffer, sizeof buffer, 0, &from, &len) == 1);
    assert(GET(EpollWait, "sceNetEpollWait")(ep, ready, 2, 0) == 0);
    /* Byte vports and optional comid; malformed and unbound destinations cannot
     * leak into the first bound socket or produce a false epoll notification. */
    unsigned char short_wire[] = {0xff, 0xc3, 40, 30, 'a'};
    unsigned char comid[] = {0xff, 0xe3, 1, 2, 3, 4, 40, 30, 'c'};
    unsigned char invalid[] = {0xff, 0xe3, 1};
    unsigned char missing[] = {0xff, 0xc3, 40, 99, 'x'};
    assert(GET(SendTo, "sceNetSendto")(udp, invalid, sizeof invalid, 0, &local, 16) == sizeof invalid);
    assert(GET(SendTo, "sceNetSendto")(udp, missing, sizeof missing, 0, &local, 16) == sizeof missing);
    assert(GET(EpollWait, "sceNetEpollWait")(ep, ready, 2, 10000) == 0);
    assert(GET(SendTo, "sceNetSendto")(udp, short_wire, sizeof short_wire, 0, &local, 16) == sizeof short_wire);
    assert(GET(SendTo, "sceNetSendto")(udp, comid, sizeof comid, 0, &local, 16) == sizeof comid);
    assert(GET(EpollWait, "sceNetEpollWait")(ep, ready, 2, 100000) == 1 && ready[0].ident == (uint64_t)a);
    assert(GET(RecvFrom, "sceNetRecvfrom")(a, buffer, sizeof buffer, 0, &from, &len) == 1 && buffer[0] == 'a');
    assert(ntohs(from.vport) == 40);
    assert(GET(RecvFrom, "sceNetRecvfrom")(a, buffer, sizeof buffer, 0, &from, &len) == 1 && buffer[0] == 'c');
    /* Shutting down a virtual socket must not shut down its siblings. */
    assert(GET(Shutdown, "sceNetShutdown")(a, 2) == 0);
    assert(GET(RecvFrom, "sceNetRecvfrom")(a, buffer, sizeof buffer, 0, NULL, NULL) == 0);
    assert((uint32_t)GET(SendTo, "sceNetSendto")(a, "x", 1, 0, &raw, 16) == 0x80410120);
    /* Closing one virtual socket leaves the physical port and the other alive. */
    assert(GET(Close, "sceNetSocketClose")(a) == 0);
    raw.vport = htons(40);
    assert(GET(SendTo, "sceNetSendto")(b, "z", 1, 0, &raw, 16) == 1);
    assert(GET(RecvFrom, "sceNetRecvfrom")(udp, buffer, sizeof buffer, 0, &from, &len) == 7);
    const unsigned char expected[] = {0xff, 0x83, 1, 0x90, 0, 40, 'z'};
    assert(!memcmp(buffer, expected, sizeof expected) && from.port == other.port);
    assert(GET(Close, "sceNetSocketClose")(b) == 0);
    assert(GET(Close, "sceNetSocketClose")(udp) == 0);
    assert(GET(Close, "sceNetEpollDestroy")(ep) == 0);
    /* Rebind the exact physical port after its last virtual socket closes. */
    b = socket_p2p(&other, 400);
    assert(GET(Close, "sceNetSocketClose")(b) == 0);
}

static int socket_udp(const char *ip, int port, Address *address) {
    int id = GET(Create, "sceNetSocket")("P2P transport test", 2, 2, 0);
    assert(id > 0);
    *address = (Address){.len = 16, .family = 2, .port = htons((uint16_t)port)};
    assert(inet_pton(AF_INET, ip, &address->addr) == 1);
    assert(GET(Bind, "sceNetBind")(id, address, sizeof *address) == 0);
    uint32_t size = sizeof *address;
    assert(GET(Name, "sceNetGetsockname")(id, address, &size) == 0 && size == 16);
    return id;
}

static void udp_roundtrip(void) {
    Address first, second, from;
    int a = socket_udp("127.0.0.1", 0, &first), b = socket_udp("127.0.0.1", 0, &second);
    const char packet[] = "UFC3 peer transport";
    char buffer[64]; uint32_t size = sizeof from;
    assert(GET(SendTo, "sceNetSendto")(a, packet, sizeof packet, 0, &second, sizeof second) == sizeof packet);
    assert(GET(RecvFrom, "sceNetRecvfrom")(b, buffer, sizeof buffer, 0, &from, &size) == sizeof packet);
    assert(!memcmp(buffer, packet, sizeof packet) && from.port == first.port);
    assert(GET(SendTo, "sceNetSendto")(b, buffer, sizeof packet, 0, &from, sizeof from) == sizeof packet);
    assert(GET(RecvFrom, "sceNetRecvfrom")(a, buffer, sizeof buffer, 0, &from, &size) == sizeof packet);
    int enabled = 1;
    assert(GET(Option, "sceNetSetsockopt")(a, 0xffff, 0x1200, &enabled, 4) == 0);
    assert((uint32_t)GET(RecvFrom, "sceNetRecvfrom")(a, buffer, sizeof buffer, 0, NULL, NULL) == 0x80410123);
    assert(*GET(ErrnoLoc, "sceNetErrnoLoc")() == 35);
    assert(GET(Close, "sceNetSocketClose")(a) == 0);
    assert(GET(Close, "sceNetSocketClose")(b) == 0);
}

static pthread_barrier_t errno_barrier;
static void *errno_worker(void *unused) {
    (void)unused;
    assert((uint32_t)GET(Close, "sceNetSocketClose")(-1) == 0x80410109);
    pthread_barrier_wait(&errno_barrier);
    pthread_barrier_wait(&errno_barrier);
    assert(*GET(ErrnoLoc, "sceNetErrnoLoc")() == 9);
    return NULL;
}
static void errno_is_per_thread(void) {
    pthread_t thread;
    assert(pthread_barrier_init(&errno_barrier, NULL, 2) == 0);
    assert(pthread_create(&thread, NULL, errno_worker, NULL) == 0);
    pthread_barrier_wait(&errno_barrier);
    *GET(ErrnoLoc, "sceNetErrnoLoc")() = 22;
    pthread_barrier_wait(&errno_barrier);
    assert(pthread_join(thread, NULL) == 0);
    assert(*GET(ErrnoLoc, "sceNetErrnoLoc")() == 22);
    pthread_barrier_destroy(&errno_barrier);
}

static int callback_count;
static void header_parser(void) {
    const char header[] = "HTTP/1.1 302 Found\r\nX-Location: wrong\r\nlOcAtIoN:\t/success?code=42\r\nEmpty:\r\n\r\n";
    const char *value = NULL; uint64_t size = 0;
    HttpParse parse = GET(HttpParse, "sceHttpParseResponseHeader");
    assert(parse && parse(header, sizeof header - 1, "Location", &value, &size) == 0);
    assert(size == strlen("/success?code=42") && !memcmp(value, "/success?code=42", (size_t)size));
    assert(parse(header, sizeof header - 1, "Empty", &value, &size) == 0 && size == 0);
    assert((uint32_t)parse(header, 18, "Location", &value, &size) == 0x80432025);
    assert((uint32_t)parse(NULL, 0, "Location", &value, &size) == 0x80432060);
    assert((uint32_t)parse(header, sizeof header, NULL, &value, &size) == 0x804321fe);
    const char bounded[] = {'A', ':', ' ', 'x'};
    assert(parse(bounded, sizeof bounded, "A", &value, &size) == 0 && size == 1 && *value == 'x');
}
static ABI int redirect_callback(int id, int status, int *method, const char *location, void *arg) {
    assert(status == 302 && *method == 0 && arg == &callback_count);
    assert(strstr(location, "?code="));
    char *header; uint64_t size;
    assert(GET(HttpHeader, "sceHttpGetAllResponseHeaders")(id, &header, &size) == 0);
    assert(size > 0 && strstr(header, "Location:"));
    const char *parsed; uint64_t length;
    assert(GET(HttpParse, "sceHttpParseResponseHeader")(header, size, "Location", &parsed, &length) == 0);
    assert(length == strlen(location) && !memcmp(parsed, location, (size_t)length));
    ++callback_count;
    return -1;
}
static void http_redirect(int nonblocking, int epoll) {
    callback_count = 0;
    int tmpl = GET(HttpTemplate, "sceHttpCreateTemplate")(1, "test", 1, 0);
    assert(tmpl > 0);
    assert(GET(HttpNonblock, "sceHttpSetNonblock")(tmpl, nonblocking) == 0);
    assert(GET(HttpCallback, "sceHttpSetRedirectCallback")(tmpl, redirect_callback, &callback_count) == 0);
    int conn = GET(HttpConnection, "sceHttpCreateConnectionWithURL")(tmpl, "https://accounts.ea.com", 0);
    int req = GET(HttpRequest, "sceHttpCreateRequestWithURL2")(conn, "GET",
        "https://accounts.ea.com/connect/auth?client_id=UFC2018_PS4_CLIENT", 0);
    void *handle = NULL;
    if (epoll) {
        assert(GET(HttpEpollCreate, "sceHttpCreateEpoll")(1, &handle) == 0 && handle);
        assert(GET(HttpEpollSet, "sceHttpSetEpoll")(req, handle, &callback_count) == 0);
    }
    int send_result = GET(HttpSend, "sceHttpSendRequest")(req, NULL, 0);
    assert(nonblocking ? (uint32_t)send_result == 0x80431082 : send_result == 0);
    if (epoll) {
        HttpEvent event; int count = 0;
        for (int i = 0; i < 10000 && count == 0; ++i)
            count = GET(HttpWait, "sceHttpWaitRequest")(handle, &event, 1, 1000);
        assert(count == 1 && (event.events & 3) == 3);
        assert(event.id == req && event.arg == &callback_count);
        assert(event.detail == event.events);
        assert(GET(HttpWait, "sceHttpWaitRequest")(handle, &event, 1, 0) == 0);
    }
    for (int i = 0; i < 10000 && (uint32_t)send_result == 0x80431082; ++i) {
        send_result = GET(HttpSend, "sceHttpSendRequest")(req, NULL, 0);
        if ((uint32_t)send_result == 0x80431082) platform_sleep_ns(1000000);
    }
    assert(send_result == 0);
    int status = 0;
    assert(GET(HttpStatus, "sceHttpGetStatusCode")(req, &status) == 0 && status == 302);
    assert(callback_count == 1);
    assert(GET(HttpStatus, "sceHttpGetStatusCode")(req, &status) == 0 && callback_count == 1);
    assert(GET(Close, "sceHttpDeleteRequest")(req) == 0);
    assert(GET(Close, "sceHttpDeleteConnection")(conn) == 0);
    assert(GET(Close, "sceHttpDeleteTemplate")(tmpl) == 0);
}

static void peer_transport(const char *ip, int port, int listener, int p2p) {
    Address local, remote;
    int id;
    if (p2p) {
        local = (Address){.len = 16, .family = 2, .port = htons((uint16_t)(listener ? port : 0))};
        id = socket_p2p(&local, listener ? 40 : 30);
        int nonblock = 0;
        assert(GET(Option, "sceNetSetsockopt")(id, 0xffff, 0x1200, &nonblock, 4) == 0);
    } else id = socket_udp("0.0.0.0", listener ? port : 0, &local);
    if (listener) { puts("READY"); fflush(stdout); }
    remote = (Address){.len = 16, .family = 2, .port = htons((uint16_t)port), .vport = htons(40)};
    assert(inet_pton(AF_INET, ip, &remote.addr) == 1);
    const char packet[] = "UFC3 Linux/Windows peer probe";
    char buffer[64]; uint32_t length = sizeof remote;
    if (!listener) assert(GET(SendTo, "sceNetSendto")(id, packet, sizeof packet, 0, &remote, sizeof remote) == sizeof packet);
    assert(GET(RecvFrom, "sceNetRecvfrom")(id, buffer, sizeof buffer, 0, &remote, &length) == sizeof packet);
    assert(!memcmp(buffer, packet, sizeof packet));
    if (p2p) assert(ntohs(remote.vport) == (listener ? 30 : 40));
    if (listener) assert(GET(SendTo, "sceNetSendto")(id, buffer, sizeof packet, 0, &remote, sizeof remote) == sizeof packet);
    GET(Close, "sceNetSocketClose")(id);
    puts(p2p ? "Peer P2P roundtrip OK" : "Peer UDP roundtrip OK");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    _putenv_s("BB_ONLINE_SERVER", "127.0.0.1");
    _putenv_s("BB_NET_LOG", "0");
#else
    setenv("BB_ONLINE_SERVER", "127.0.0.1", 1);
    setenv("BB_NET_LOG", "0", 1);
#endif
    if (argc == 3 && (!strcmp(argv[1], "--listen") || !strcmp(argv[1], "--p2p-listen")))
        peer_transport("127.0.0.1", atoi(argv[2]), 1, !strcmp(argv[1], "--p2p-listen"));
    else if (argc == 4 && (!strcmp(argv[1], "--peer") || !strcmp(argv[1], "--p2p-peer")))
        peer_transport(argv[2], atoi(argv[3]), 0, !strcmp(argv[1], "--p2p-peer"));
    else {
        header_parser();
        udp_roundtrip();
        p2p_virtual_ports();
        errno_is_per_thread();
        if (argc == 2 && !strcmp(argv[1], "--http")) {
            http_redirect(0, 0);
            http_redirect(1, 0);
            http_redirect(1, 1);
        }
        puts("Online runtime ABI checks OK");
    }
    return 0;
}
