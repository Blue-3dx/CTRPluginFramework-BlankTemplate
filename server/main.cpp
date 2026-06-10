/*
 * vc3ds-server — Voice Chat Relay Server
 * ---------------------------------------
 * Linux / aarch64 (or any POSIX system)
 *
 * TCP 7001 : control  (HELLO, room list, join/leave, talking status)
 * UDP 7002 : voice    (Opus packets, relayed to room members)
 *
 * Build:
 *   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
 *
 * Run:
 *   ./vc3ds-server [--port-tcp 7001] [--port-udp 7002] [--rooms 8]
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Shared protocol header (same file used by the 3DS plugin) */
#include "Protocol.hpp"

/* ================================================================
 * Configuration
 * ================================================================ */

static uint16_t g_tcpPort = VC_TCP_PORT;
static uint16_t g_udpPort = VC_UDP_PORT;
static int      g_maxRooms = MAX_ROOMS;

/* ================================================================
 * Data model
 * ================================================================ */

struct Client {
    int         fd;           /* TCP socket                              */
    uint32_t    session_id;   /* assigned on HELLO_ACK                  */
    std::string username;
    uint8_t     room_id;      /* 0xFF = not in a room                   */
    bool        talking;

    /* UDP address learned from first incoming voice packet */
    sockaddr_in udp_addr;
    bool        udp_known;

    /* TCP receive buffer */
    uint8_t     rx_buf[sizeof(PktHeader) + 512];
    int         rx_len;

    Client() : fd(-1), session_id(0), room_id(0xFF), talking(false),
               udp_known(false), rx_len(0) {
        memset(&udp_addr, 0, sizeof(udp_addr));
        memset(rx_buf,    0, sizeof(rx_buf));
    }
};

struct Room {
    uint8_t     id;
    std::string name;
    uint8_t     max_users;
    std::vector<uint32_t> members;   /* session_ids */

    Room() : id(0), max_users(MAX_USERS_PER_ROOM) {}
};

/* ================================================================
 * Global state (mutex-protected)
 * ================================================================ */

static std::mutex              g_mtx;
static std::map<uint32_t, Client> g_clients;   /* session_id → Client */
static std::vector<Room>       g_rooms;
static uint32_t                g_nextId = 1;
static std::atomic<bool>       g_running{true};

/* ================================================================
 * Helpers
 * ================================================================ */

static void set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static bool send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p   += n;
        len -= (size_t)n;
    }
    return true;
}

static bool send_pkt(int fd, PktType type,
                     const void *payload = nullptr, uint16_t plen = 0) {
    PktHeader hdr;
    hdr.type   = type;
    hdr.length = plen;
    if (!send_all(fd, &hdr, sizeof(hdr))) return false;
    if (plen && payload) return send_all(fd, payload, plen);
    return true;
}

/* Broadcast a packet to every member of a room except 'exclude_id' */
static void broadcast_room(uint8_t room_id, uint32_t exclude_id,
                            PktType type,
                            const void *payload = nullptr, uint16_t plen = 0)
{
    if (room_id >= (uint8_t)g_rooms.size()) return;
    for (uint32_t sid : g_rooms[room_id].members) {
        if (sid == exclude_id) continue;
        auto it = g_clients.find(sid);
        if (it != g_clients.end())
            send_pkt(it->second.fd, type, payload, plen);
    }
}

/* Build a UserEntry from a client */
static UserEntry make_user_entry(const Client &c) {
    UserEntry e;
    e.id      = c.session_id;
    memset(e.username, 0, sizeof(e.username));
    strncpy(e.username, c.username.c_str(), MAX_USERNAME_LEN - 1);
    e.talking = c.talking ? 1 : 0;
    return e;
}

/* ================================================================
 * Client lifecycle
 * ================================================================ */

static void client_leave_room(uint32_t sid) {
    auto it = g_clients.find(sid);
    if (it == g_clients.end()) return;
    Client &c = it->second;
    if (c.room_id == 0xFF) return;

    uint8_t rid = c.room_id;
    c.room_id   = 0xFF;
    c.talking   = false;

    Room &room = g_rooms[rid];
    room.members.erase(
        std::remove(room.members.begin(), room.members.end(), sid),
        room.members.end());

    /* Tell others */
    PktUserLeft msg;
    msg.user_id = sid;
    broadcast_room(rid, sid, PktType::USER_LEFT, &msg, sizeof(msg));
    printf("[room %u] %s left  (now %zu/%u)\n",
           rid, c.username.c_str(), room.members.size(), room.max_users);
}

static void client_disconnect(uint32_t sid) {
    auto it = g_clients.find(sid);
    if (it == g_clients.end()) return;
    client_leave_room(sid);
    close(it->second.fd);
    printf("[server] %s (#%u) disconnected\n",
           it->second.username.c_str(), sid);
    g_clients.erase(it);
}

/* ================================================================
 * TCP message handlers
 * ================================================================ */

static void handle_hello(Client &c, const uint8_t *payload, uint16_t len) {
    if (len < sizeof(PktHello)) return;
    const PktHello *h = (const PktHello *)payload;

    char name[MAX_USERNAME_LEN + 1] = {};
    strncpy(name, h->username, MAX_USERNAME_LEN);
    c.username = name;

    PktHelloAck ack;
    ack.session_id = c.session_id;
    send_pkt(c.fd, PktType::HELLO_ACK, &ack, sizeof(ack));
    printf("[server] HELLO from '%s' (id=%u)\n", name, c.session_id);
}

static void handle_room_list_req(Client &c) {
    PktRoomList prl;
    memset(&prl, 0, sizeof(prl));
    prl.count = (uint8_t)std::min((int)g_rooms.size(), MAX_ROOMS);
    for (int i = 0; i < prl.count; i++) {
        prl.rooms[i].id    = g_rooms[i].id;
        prl.rooms[i].count = (uint8_t)g_rooms[i].members.size();
        prl.rooms[i].max   = g_rooms[i].max_users;
        memset(prl.rooms[i].name, 0, ROOM_NAME_LEN);
        strncpy(prl.rooms[i].name, g_rooms[i].name.c_str(), ROOM_NAME_LEN - 1);
    }
    send_pkt(c.fd, PktType::ROOM_LIST, &prl, sizeof(prl));
}

static void handle_join_room(Client &c, const uint8_t *payload, uint16_t len) {
    if (len < sizeof(PktJoinRoom)) return;
    const PktJoinRoom *jr = (const PktJoinRoom *)payload;
    uint8_t rid = jr->room_id;

    if (rid >= (uint8_t)g_rooms.size()) {
        PktError err;
        snprintf(err.message, sizeof(err.message), "Room %u does not exist", rid);
        send_pkt(c.fd, PktType::ERROR, &err, sizeof(err));
        return;
    }

    Room &room = g_rooms[rid];
    if ((int)room.members.size() >= room.max_users) {
        PktError err;
        snprintf(err.message, sizeof(err.message), "Room %u is full (%u/%u)",
                 rid, (unsigned)room.members.size(), room.max_users);
        send_pkt(c.fd, PktType::ERROR, &err, sizeof(err));
        return;
    }

    /* Leave previous room if needed */
    if (c.room_id != 0xFF) client_leave_room(c.session_id);

    c.room_id = rid;
    room.members.push_back(c.session_id);

    /* Build ACK with current user list */
    PktJoinRoomAck ack;
    memset(&ack, 0, sizeof(ack));
    ack.room_id    = rid;
    ack.user_count = 0;
    for (uint32_t sid : room.members) {
        auto it = g_clients.find(sid);
        if (it == g_clients.end()) continue;
        if (ack.user_count >= MAX_USERS_PER_ROOM) break;
        ack.users[ack.user_count++] = make_user_entry(it->second);
    }
    send_pkt(c.fd, PktType::JOIN_ROOM_ACK, &ack, sizeof(ack));

    /* Tell existing members someone joined */
    PktUserJoined puj;
    puj.user = make_user_entry(c);
    broadcast_room(rid, c.session_id, PktType::USER_JOINED, &puj, sizeof(puj));

    printf("[room %u] %s joined  (now %zu/%u)\n",
           rid, c.username.c_str(), room.members.size(), room.max_users);
}

static void handle_talking(Client &c, PktType type) {
    bool talking = (type == PktType::TALKING_START);
    if (c.talking == talking) return;
    c.talking = talking;

    if (c.room_id == 0xFF) return;
    PktTalkingChange ptc;
    ptc.user_id = c.session_id;
    ptc.talking  = talking ? 1 : 0;
    broadcast_room(c.room_id, c.session_id,
                   PktType::TALKING_CHANGE, &ptc, sizeof(ptc));
}

/* ================================================================
 * TCP receive / dispatch for one client
 * Returns false if the client should be disconnected
 * ================================================================ */

static bool process_tcp(Client &c) {
    ssize_t n = recv(c.fd, c.rx_buf + c.rx_len,
                     (int)sizeof(c.rx_buf) - c.rx_len, 0);
    if (n == 0) return false;           /* clean close */
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;                   /* error */
    }
    c.rx_len += (int)n;

    while (c.rx_len >= (int)sizeof(PktHeader)) {
        PktHeader *hdr   = (PktHeader *)c.rx_buf;
        int        total = (int)sizeof(PktHeader) + (int)hdr->length;
        if (c.rx_len < total) break;    /* incomplete payload */

        const uint8_t *payload = c.rx_buf + sizeof(PktHeader);
        uint16_t       plen    = hdr->length;

        switch (hdr->type) {
            case PktType::HELLO:          handle_hello(c, payload, plen);    break;
            case PktType::ROOM_LIST_REQ:  handle_room_list_req(c);           break;
            case PktType::JOIN_ROOM:      handle_join_room(c, payload, plen);break;
            case PktType::LEAVE_ROOM:     client_leave_room(c.session_id);   break;
            case PktType::TALKING_START:
            case PktType::TALKING_STOP:   handle_talking(c, hdr->type);      break;
            case PktType::PING:
                send_pkt(c.fd, PktType::PONG);
                break;
            default: break;
        }

        c.rx_len -= total;
        if (c.rx_len > 0)
            memmove(c.rx_buf, c.rx_buf + total, c.rx_len);
    }
    return true;
}

/* ================================================================
 * UDP voice relay thread
 * ================================================================ */

static void udp_thread(int udpFd) {
    uint8_t buf[sizeof(VoiceHeader) + OPUS_MAX_PACKET + 32];

    while (g_running) {
        sockaddr_in src;
        socklen_t   srclen = sizeof(src);
        ssize_t n = recvfrom(udpFd, buf, sizeof(buf), 0,
                             (sockaddr *)&src, &srclen);
        if (n <= 0) {
            /* non-blocking: just spin */
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        if ((size_t)n < sizeof(VoiceHeader)) continue;

        VoiceHeader *vh = (VoiceHeader *)buf;

        std::lock_guard<std::mutex> lock(g_mtx);

        /* Learn / update client's UDP endpoint */
        auto cit = g_clients.find(vh->session_id);
        if (cit == g_clients.end()) continue;
        Client &sender = cit->second;

        if (!sender.udp_known) {
            sender.udp_addr  = src;
            sender.udp_known = true;
        }

        /* Zero-length = UDP hello probe; don't relay */
        if (vh->data_len == 0 || (size_t)n < sizeof(VoiceHeader) + vh->data_len)
            continue;

        if (sender.room_id == 0xFF) continue;

        /* Relay to every other member that has a known UDP address */
        const Room &room = g_rooms[sender.room_id];
        for (uint32_t sid : room.members) {
            if (sid == vh->session_id) continue;
            auto it = g_clients.find(sid);
            if (it == g_clients.end() || !it->second.udp_known) continue;

            sendto(udpFd, buf, (size_t)n, 0,
                   (sockaddr *)&it->second.udp_addr,
                   sizeof(it->second.udp_addr));
        }
    }
}

/* ================================================================
 * Main
 * ================================================================ */

static void print_usage(const char *prog) {
    printf("Usage: %s [--tcp-port PORT] [--udp-port PORT] [--rooms N]\n", prog);
    printf("  Defaults: TCP=%u  UDP=%u  rooms=%d\n",
           VC_TCP_PORT, VC_UDP_PORT, MAX_ROOMS);
}

int main(int argc, char **argv) {
    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tcp-port") && i + 1 < argc)
            g_tcpPort = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--udp-port") && i + 1 < argc)
            g_udpPort = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rooms") && i + 1 < argc)
            g_maxRooms = std::max(1, std::min(atoi(argv[++i]), MAX_ROOMS));
        else if (!strcmp(argv[i], "--help")) { print_usage(argv[0]); return 0; }
    }

    /* Ignore SIGPIPE so a broken client doesn't kill the server */
    signal(SIGPIPE, SIG_IGN);

    /* Initialise rooms */
    for (int i = 0; i < g_maxRooms; i++) {
        Room r;
        r.id        = (uint8_t)i;
        r.max_users = MAX_USERS_PER_ROOM;
        r.name      = "Room " + std::to_string(i);
        g_rooms.push_back(r);
    }

    /* ---- TCP listen socket ---- */
    int tcpListen = socket(AF_INET6, SOCK_STREAM, 0);
    if (tcpListen < 0) {
        /* Fall back to IPv4 only */
        tcpListen = socket(AF_INET, SOCK_STREAM, 0);
        if (tcpListen < 0) { perror("socket(tcp)"); return 1; }
        int yes = 1;
        setsockopt(tcpListen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in a4;
        memset(&a4, 0, sizeof(a4));
        a4.sin_family      = AF_INET;
        a4.sin_port        = htons(g_tcpPort);
        a4.sin_addr.s_addr = INADDR_ANY;
        if (bind(tcpListen, (sockaddr *)&a4, sizeof(a4)) < 0) { perror("bind(tcp)"); return 1; }
    } else {
        int yes = 1;
        setsockopt(tcpListen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        int no  = 0;
        setsockopt(tcpListen, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
        sockaddr_in6 a6;
        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_port   = htons(g_tcpPort);
        a6.sin6_addr   = in6addr_any;
        if (bind(tcpListen, (sockaddr *)&a6, sizeof(a6)) < 0) { perror("bind(tcp6)"); return 1; }
    }

    listen(tcpListen, 16);
    set_nonblocking(tcpListen);

    /* ---- UDP socket ---- */
    int udpFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpFd < 0) { perror("socket(udp)"); return 1; }
    {
        int yes = 1;
        setsockopt(udpFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    }
    sockaddr_in ua;
    memset(&ua, 0, sizeof(ua));
    ua.sin_family      = AF_INET;
    ua.sin_port        = htons(g_udpPort);
    ua.sin_addr.s_addr = INADDR_ANY;
    if (bind(udpFd, (sockaddr *)&ua, sizeof(ua)) < 0) { perror("bind(udp)"); return 1; }
    set_nonblocking(udpFd);

    printf("[server] Voice Chat Server starting\n");
    printf("[server] TCP :%u   UDP :%u   %d rooms x %d users\n",
           g_tcpPort, g_udpPort, g_maxRooms, MAX_USERS_PER_ROOM);

    /* Start UDP relay thread */
    std::thread udpWorker(udp_thread, udpFd);

    /* ---- Main loop: poll TCP ---- */
    while (g_running) {
        /* Build pollfd array: [0] = listen socket, [1..] = client sockets */
        std::vector<pollfd> pfds;
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            pfds.push_back({ tcpListen, POLLIN, 0 });
            for (auto &kv : g_clients)
                pfds.push_back({ kv.second.fd, POLLIN, 0 });
        }

        int ret = poll(pfds.data(), (nfds_t)pfds.size(), 100 /* ms */);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        std::lock_guard<std::mutex> lock(g_mtx);

        /* New TCP connection */
        if (pfds[0].revents & POLLIN) {
            sockaddr_in caddr;
            socklen_t   clen = sizeof(caddr);
            int cfd = accept(tcpListen, (sockaddr *)&caddr, &clen);
            if (cfd >= 0) {
                set_nonblocking(cfd);
                Client c;
                c.fd         = cfd;
                c.session_id = g_nextId++;
                g_clients[c.session_id] = c;
                printf("[server] New connection from %s (id=%u)\n",
                       inet_ntoa(caddr.sin_addr), c.session_id);
            }
        }

        /* Existing clients - collect disconnects to process after iteration */
        std::vector<uint32_t> to_disconnect;
        int idx = 1;
        for (auto &kv : g_clients) {
            if (idx < (int)pfds.size() && (pfds[idx].revents & (POLLIN | POLLERR | POLLHUP))) {
                if (!process_tcp(kv.second))
                    to_disconnect.push_back(kv.first);
            }
            idx++;
        }
        for (uint32_t sid : to_disconnect)
            client_disconnect(sid);
    }

    g_running = false;
    udpWorker.join();
    close(tcpListen);
    close(udpFd);
    return 0;
}
