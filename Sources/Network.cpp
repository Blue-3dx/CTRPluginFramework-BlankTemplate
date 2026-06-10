#include "Network.hpp"
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

namespace VoicePlugin {

NetworkManager::NetworkManager() {
    memset(&m_serverUdpAddr, 0, sizeof(m_serverUdpAddr));
    memset(m_tcpBuf,         0, sizeof(m_tcpBuf));
}

NetworkManager::~NetworkManager() {
    Disconnect();
    ShutdownSoc();
}

/* =========================================================
 * SOC service
 * ========================================================= */

bool NetworkManager::InitSoc() {
    if (m_socInited) return true;

    m_socBuf = (u32 *)memalign(0x1000, NET_SOC_BUF_SIZE);
    if (!m_socBuf) return false;

    Result r = socInit(m_socBuf, NET_SOC_BUF_SIZE);
    if (R_FAILED(r)) {
        free(m_socBuf);
        m_socBuf   = nullptr;
        return false;
    }

    m_socInited = true;
    return true;
}

void NetworkManager::ShutdownSoc() {
    if (!m_socInited) return;
    socExit();
    if (m_socBuf) { free(m_socBuf); m_socBuf = nullptr; }
    m_socInited = false;
}

/* =========================================================
 * Connect / Disconnect
 * ========================================================= */

bool NetworkManager::Connect(const std::string &serverIp, uint16_t tcpPort, uint16_t udpPort) {
    if (!m_socInited) return false;

    /* ---- TCP control socket ---- */
    m_tcpSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_tcpSock < 0) return false;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(tcpPort);
    addr.sin_addr.s_addr = inet_addr(serverIp.c_str());

    if (connect(m_tcpSock, (sockaddr *)&addr, sizeof(addr)) < 0) {
        close(m_tcpSock);
        m_tcpSock = -1;
        return false;
    }

    /* Make TCP receives non-blocking after connect */
    int flags = fcntl(m_tcpSock, F_GETFL, 0);
    fcntl(m_tcpSock, F_SETFL, flags | O_NONBLOCK);

    /* ---- UDP voice socket ---- */
    m_udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpSock < 0) {
        close(m_tcpSock);
        m_tcpSock = -1;
        return false;
    }

    /* Non-blocking UDP */
    flags = fcntl(m_udpSock, F_GETFL, 0);
    fcntl(m_udpSock, F_SETFL, flags | O_NONBLOCK);

    /* Bind to any local port */
    sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = 0;
    local.sin_addr.s_addr = INADDR_ANY;
    bind(m_udpSock, (sockaddr *)&local, sizeof(local));

    /* Remember server UDP endpoint */
    memset(&m_serverUdpAddr, 0, sizeof(m_serverUdpAddr));
    m_serverUdpAddr.sin_family      = AF_INET;
    m_serverUdpAddr.sin_port        = htons(udpPort);
    m_serverUdpAddr.sin_addr.s_addr = inet_addr(serverIp.c_str());

    m_tcpBufLen = 0;
    return true;
}

void NetworkManager::Disconnect() {
    if (m_tcpSock >= 0) { close(m_tcpSock); m_tcpSock = -1; }
    if (m_udpSock >= 0) { close(m_udpSock); m_udpSock = -1; }
    m_tcpBufLen = 0;
}

/* =========================================================
 * Send helpers
 * ========================================================= */

bool NetworkManager::SendTCP(PktType type, const void *payload, uint16_t payloadLen) {
    if (m_tcpSock < 0) return false;

    PktHeader hdr;
    hdr.type   = type;
    hdr.length = payloadLen;

    /* Blocking send for control messages (small, infrequent) */
    int flags = fcntl(m_tcpSock, F_GETFL, 0);
    fcntl(m_tcpSock, F_SETFL, flags & ~O_NONBLOCK);

    bool ok = true;
    if (send(m_tcpSock, &hdr, sizeof(hdr), 0) != (int)sizeof(hdr)) { ok = false; }
    if (ok && payloadLen > 0 && payload) {
        if (send(m_tcpSock, payload, payloadLen, 0) != payloadLen) { ok = false; }
    }

    /* Restore non-blocking */
    fcntl(m_tcpSock, F_SETFL, flags | O_NONBLOCK);
    return ok;
}

bool NetworkManager::SendVoice(uint32_t sessionId, uint32_t userId, uint16_t sequence,
                               const uint8_t *opusData, uint16_t opusLen) {
    if (m_udpSock < 0) return false;

    /* Stack-allocated packet (header + opus, max ~410 bytes) */
    uint8_t pkt[sizeof(VoiceHeader) + OPUS_MAX_PACKET];
    VoiceHeader *hdr = (VoiceHeader *)pkt;
    hdr->session_id = sessionId;
    hdr->user_id    = userId;
    hdr->sequence   = sequence;
    hdr->data_len   = opusLen;
    memcpy(pkt + sizeof(VoiceHeader), opusData, opusLen);

    ssize_t sent = sendto(m_udpSock, pkt, sizeof(VoiceHeader) + opusLen, 0,
                          (sockaddr *)&m_serverUdpAddr, sizeof(m_serverUdpAddr));
    return sent > 0;
}

void NetworkManager::SendUdpHello(uint32_t sessionId) {
    /* A bare VoiceHeader with data_len=0 lets the server learn our UDP port */
    VoiceHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.session_id = sessionId;
    hdr.data_len   = 0;
    sendto(m_udpSock, &hdr, sizeof(hdr), 0,
           (sockaddr *)&m_serverUdpAddr, sizeof(m_serverUdpAddr));
}

/* =========================================================
 * Poll TCP  (call in voice thread, non-blocking)
 * ========================================================= */

void NetworkManager::PollTCP() {
    if (m_tcpSock < 0 || !m_ctrlCb) return;

    /* Try to fill receive buffer */
    ssize_t n = recv(m_tcpSock, m_tcpBuf + m_tcpBufLen,
                     (int)sizeof(m_tcpBuf) - m_tcpBufLen, 0);
    if (n > 0) m_tcpBufLen += (int)n;
    else if (n == 0) { /* connection closed */ Disconnect(); return; }
    /* n < 0 with EAGAIN/EWOULDBLOCK is normal for non-blocking */

    /* Dispatch complete messages */
    while (m_tcpBufLen >= (int)sizeof(PktHeader)) {
        PktHeader *hdr = (PktHeader *)m_tcpBuf;
        int total = (int)sizeof(PktHeader) + (int)hdr->length;

        if (m_tcpBufLen < total) break;   /* incomplete payload */

        m_ctrlCb(hdr->type,
                 m_tcpBuf + sizeof(PktHeader),
                 hdr->length);

        /* Shift remaining bytes to front */
        m_tcpBufLen -= total;
        if (m_tcpBufLen > 0) {
            memmove(m_tcpBuf, m_tcpBuf + total, m_tcpBufLen);
        }
    }
}

/* =========================================================
 * Poll UDP  (call in voice thread, non-blocking)
 * ========================================================= */

void NetworkManager::PollUDP() {
    if (m_udpSock < 0 || !m_voiceCb) return;

    uint8_t buf[sizeof(VoiceHeader) + OPUS_MAX_PACKET + 16];

    while (true) {
        ssize_t n = recv(m_udpSock, buf, sizeof(buf), 0);
        if (n <= 0) break;   /* EAGAIN or error */

        if ((size_t)n < sizeof(VoiceHeader)) continue;   /* malformed */

        VoiceHeader *hdr = (VoiceHeader *)buf;
        uint16_t    dataLen = hdr->data_len;

        if ((size_t)n < sizeof(VoiceHeader) + dataLen) continue;

        m_voiceCb(*hdr,
                  buf + sizeof(VoiceHeader),
                  dataLen);
    }
}

} /* namespace VoicePlugin */
