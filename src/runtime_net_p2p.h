/* SPDX-License-Identifier: GPL-2.0-or-later
 * PS4 datagram P2P transport. Architecture and wire format compared against
 * droogie/bbhost src/hle/net.cpp (566184a); this is a C implementation for this
 * runtime. One physical UDP port serves multiple independent virtual sockets.
 * All port/queue operations run under `lock`. Physical sockets are nonblocking;
 * recv and epoll pump incoming datagrams into bounded per-vport queues. */
struct P2pPort {
    int used, refs;
    HostSocket fd;
    struct sockaddr_in local;
};
struct P2pPacket {
    P2pPacket *next;
    PsSockaddrIn from;
    size_t size;
    unsigned char data[];
};
static P2pPort p2p_ports[MAX_SOCKETS];

static int p2p_host_nonblock(HostSocket fd) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}
static void p2p_release_locked(Socket *k) {
    while (k->head) {
        P2pPacket *next = k->head->next;
        free(k->head); k->head = next;
    }
    k->tail = NULL; k->queued_bytes = 0; k->queued_packets = 0;
    if (--k->p2p->refs == 0) {
        close_socket(k->p2p->fd);
        k->p2p->used = 0;
    }
    k->p2p = NULL; k->s = BAD_SOCKET;
}
static int32_t p2p_bind(int id, const PsSockaddrIn *addr) {
    struct sockaddr_in h; to_host(addr, &h);
    pthread_mutex_lock(&lock);
    Socket *k = get_socket(id);
    int error = 0;
    P2pPort *port = NULL, *unused = NULL;
    if (!k) error = 9;
    else if (k->p2p) error = 22;
    if (!error) {
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            P2pPort *p = &p2p_ports[i];
            if (!p->used) { if (!unused) unused = p; continue; }
            if (h.sin_port && p->local.sin_port == h.sin_port &&
                p->local.sin_addr.s_addr == h.sin_addr.s_addr) { port = p; break; }
        }
        if (port) {
            for (int i = 1; i < MAX_SOCKETS; ++i)
                if (sockets[i].used && sockets[i].p2p == port &&
                    sockets[i].vport == ntohs(addr->vport)) { error = 48; break; }
        } else if (!unused) error = 24;
        else {
            if (bind(k->s, (struct sockaddr *)&h, sizeof h)) error = host_errno();
            else if (p2p_host_nonblock(k->s)) error = host_errno();
            else {
                socklen_t size = sizeof h;
                if (getsockname(k->s, (struct sockaddr *)&h, &size)) error = host_errno();
                else {
                    port = unused;
                    *port = (P2pPort){.used = 1, .fd = k->s, .local = h};
                }
            }
        }
        if (!error) {
            if (port->fd != k->s) close_socket(k->s);
            k->s = port->fd; k->p2p = port; k->vport = ntohs(addr->vport);
            ++port->refs;
        }
    }
    pthread_mutex_unlock(&lock);
    LOG("bind P2P %d %s vport %u -> %s", id, addr_text(addr), ntohs(addr->vport), error ? "error" : "ok");
    return error ? fail(error) : 0;
}
static int32_t p2p_autobind(int id) {
    pthread_mutex_lock(&lock);
    Socket *k = get_socket(id);
    int bound = k && k->p2p;
    pthread_mutex_unlock(&lock);
    if (bound) return 0;
    PsSockaddrIn local = {.len = 16, .family = 2};
    return p2p_bind(id, &local);
}
static size_t p2p_decode(const unsigned char *data, size_t size, uint16_t *src, uint16_t *dst) {
    /* Reject ordinary UDP, control frames and truncated headers. Optional comid
     * precedes the virtual ports. Packet type 3 is datagram P2P. */
    if (size < 4 || data[0] != 0xff || (data[1] & 0x9f) != 0x83) return 0;
    size_t at = (data[1] & 0x20) ? 6 : 2;
    if (data[1] & 0x40) {
        if (size < at + 2) return 0;
        *src = data[at]; *dst = data[at + 1]; return at + 2;
    }
    if (size < at + 4) return 0;
    *src = (uint16_t)((data[at] << 8) | data[at + 1]);
    *dst = (uint16_t)((data[at + 2] << 8) | data[at + 3]);
    return at + 4;
}
static void p2p_pump_locked(P2pPort *port) {
    unsigned char wire[65536];
    /* Limit a pump so a busy peer cannot starve other sockets. */
    for (int count = 0; count < 256; ++count) {
        struct sockaddr_in from; socklen_t len = sizeof from;
        int n = recvfrom(port->fd, (char *)wire, sizeof wire, 0, (struct sockaddr *)&from, &len);
        if (n < 0) break;
        uint16_t src, dst;
        size_t header = p2p_decode(wire, (size_t)n, &src, &dst);
        if (!header) continue;
        Socket *target = NULL;
        for (int i = 1; i < MAX_SOCKETS; ++i)
            if (sockets[i].used && sockets[i].p2p == port && sockets[i].vport == dst) {
                target = &sockets[i]; break;
            }
        if (!target || (target->shutdown_flags & 1)) continue;
        if (target->connected) {
            const PsSockaddrIn *peer = (const void *)target->peer;
            if (peer->addr != from.sin_addr.s_addr || peer->port != from.sin_port ||
                ntohs(peer->vport) != src) continue;
        }
        size_t bytes = (size_t)n - header;
        if (target->queued_packets >= 128 || target->queued_bytes + bytes > 262144) continue;
        if (net_log && bytes >= 48)
            LOG("P2P recv %u bytes on vport %u", (unsigned)bytes, dst);
        P2pPacket *packet = malloc(sizeof *packet + bytes);
        if (!packet) continue;
        *packet = (P2pPacket){.from = {.len = 16, .family = 2, .port = from.sin_port,
            .addr = from.sin_addr.s_addr, .vport = htons(src)}, .size = bytes};
        memcpy(packet->data, wire + header, bytes);
        if (target->tail) target->tail->next = packet;
        else target->head = packet;
        target->tail = packet; ++target->queued_packets; target->queued_bytes += bytes;
    }
}
static int32_t p2p_send(int id, const void *buf, uint64_t size, int flags, const PsSockaddrIn *to) {
    (void)flags; /* physical transport is always nonblocking */
    if ((!buf && size) || size > 65501) return fail(size > 65501 ? 40 : 22);
    int32_t result = p2p_autobind(id);
    if (result) return result;
    pthread_mutex_lock(&lock);
    Socket *k = get_socket(id);
    if (!k || !k->p2p) { pthread_mutex_unlock(&lock); return fail(9); }
    if (k->shutdown_flags & 2) { pthread_mutex_unlock(&lock); return fail(32); }
    if (!to && k->connected) to = (const void *)k->peer;
    if (!to) { pthread_mutex_unlock(&lock); return fail(57); }
    unsigned char wire[65507];
    uint16_t dst = ntohs(to->vport);
    size_t header;
    wire[0] = 0xff;
    if (k->vport < 256 && dst < 256) {
        wire[1] = 0xc3; wire[2] = (unsigned char)k->vport; wire[3] = (unsigned char)dst;
        header = 4;
    } else {
        wire[1] = 0x83; wire[2] = (unsigned char)(k->vport >> 8); wire[3] = (unsigned char)k->vport;
        wire[4] = (unsigned char)(dst >> 8); wire[5] = (unsigned char)dst; header = 6;
    }
    if (size) memcpy(wire + header, buf, (size_t)size);
    struct sockaddr_in host; to_host(to, &host);
    int n = sendto(k->p2p->fd, (const char *)wire, (int)(header + size), 0,
        (struct sockaddr *)&host, sizeof host);
    int error = n < 0 ? host_errno() : 0;
    pthread_mutex_unlock(&lock);
    if (net_log && size >= 48)
        LOG("P2P send %d %llu bytes -> %s", id, (unsigned long long)size, error ? "error" : "ok");
    return error ? fail(error) : (int32_t)size;
}
static int32_t p2p_recv(int id, void *buf, uint64_t size, int flags, void *addr, uint32_t *len) {
    if (!buf && size) return fail(22);
    for (;;) {
        pthread_mutex_lock(&lock);
        Socket *k = get_socket(id);
        if (!k || !k->p2p) { pthread_mutex_unlock(&lock); return fail(9); }
        if (k->shutdown_flags & 1) { pthread_mutex_unlock(&lock); return 0; }
        p2p_pump_locked(k->p2p);
        P2pPacket *packet = k->head;
        if (packet) {
            size_t count = size < packet->size ? (size_t)size : packet->size;
            if (count) memcpy(buf, packet->data, count);
            if (addr && len) {
                size_t address_size = *len < sizeof packet->from ? *len : sizeof packet->from;
                memcpy(addr, &packet->from, address_size); *len = sizeof packet->from;
            }
            if (!(flags & 2)) {
                k->head = packet->next; if (!k->head) k->tail = NULL;
                --k->queued_packets; k->queued_bytes -= packet->size; free(packet);
            }
            pthread_mutex_unlock(&lock); return (int32_t)count;
        }
        int nonblock = k->nonblocking || (flags & 0x80);
        pthread_mutex_unlock(&lock);
        if (nonblock) return fail(35);
        /* A short sleep also observes concurrent close without polling a reused
         * host fd. epoll uses readiness of these queues, never a foreign vport. */
        platform_sleep_ns(1000000);
    }
}
