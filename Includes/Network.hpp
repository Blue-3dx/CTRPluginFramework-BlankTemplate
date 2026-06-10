#pragma once
#ifndef NETWORK_HPP
#define NETWORK_HPP

#include <3ds.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <string>
#include <functional>
#include "Protocol.hpp"

namespace VoicePlugin {

/*
 * NetworkManager
 * --------------
 * Owns the SOC service, a TCP control socket and a UDP voice socket.
 *
 * TCP: blocking send, non-blocking recv (we poll in the voice thread).
 * UDP: non-blocking send/recv via O_NONBLOCK.
 *
 * The SOC service takes a 512 KB heap buffer in linear memory.
 */

#define NET_SOC_BUF_SIZE   (512 * 1024)

/* Fired when a complete TCP control message arrives */
using ControlMessageCallback = std::function<void(PktType, const uint8_t *, uint16_t)>;

/* Fired when a UDP voice packet arrives */
using VoicePacketCallback = std::function<void(const VoiceHeader &, const uint8_t *, uint16_t)>;

class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    /* Initialize the SOC service. Call once before Connect(). */
    bool InitSoc();
    void ShutdownSoc();

    /* Connect TCP + bind UDP. serverIp is dotted-decimal string. */
    bool Connect(const std::string &serverIp, uint16_t tcpPort = VC_TCP_PORT,
                 uint16_t udpPort = VC_UDP_PORT);
    void Disconnect();
    bool IsConnected() const { return m_tcpSock >= 0; }

    /* --- TCP control messages --- */
    bool SendTCP(PktType type, const void *payload = nullptr, uint16_t payloadLen = 0);

    /* Call in the voice thread; fires ControlMessageCallback for each message received */
    void PollTCP();

    /* --- UDP voice packets --- */
    bool SendVoice(uint32_t sessionId, uint32_t userId, uint16_t sequence,
                   const uint8_t *opusData, uint16_t opusLen);

    /* Call in the voice thread; fires VoicePacketCallback for each received datagram */
    void PollUDP();

    /* Callbacks */
    void SetControlCallback(ControlMessageCallback cb) { m_ctrlCb = cb; }
    void SetVoiceCallback(VoicePacketCallback cb)      { m_voiceCb = cb; }

    bool IsSocInited() const { return m_socInited; }

private:
    /* Try to send a "UDP hello" so the server learns our NAT-mapped UDP port */
    void SendUdpHello(uint32_t sessionId);

    bool       m_socInited = false;
    int        m_tcpSock   = -1;
    int        m_udpSock   = -1;
    sockaddr_in m_serverUdpAddr;

    /* TCP receive reassembly buffer */
    uint8_t    m_tcpBuf[sizeof(PktHeader) + 512];
    int        m_tcpBufLen = 0;

    /* SOC heap (must stay alive for the lifetime of the service) */
    u32       *m_socBuf    = nullptr;

    ControlMessageCallback m_ctrlCb;
    VoicePacketCallback    m_voiceCb;
};

} /* namespace VoicePlugin */
#endif /* NETWORK_HPP */
