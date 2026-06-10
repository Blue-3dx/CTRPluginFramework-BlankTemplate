#pragma once
#ifndef VOICECHAT_HPP
#define VOICECHAT_HPP

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <string>
#include <vector>
#include <atomic>
#include "Protocol.hpp"
#include "Audio.hpp"
#include "Network.hpp"

namespace VoicePlugin {

/* ---------- Public data structures ---------- */

struct RoomInfo {
    uint8_t     id;
    std::string name;
    uint8_t     count;
    uint8_t     max;
};

struct RemoteUser {
    uint32_t    id;
    std::string username;
    bool        talking;
};

/* ---------- VoiceChat ---------- */

class VoiceChat {
public:
    VoiceChat();
    ~VoiceChat();

    /* ---- Connection ---- */
    bool Connect();       /* opens TCP + UDP; starts voice thread */
    void Disconnect();    /* stops thread, closes sockets, clears state */
    bool IsConnected() const { return m_connected; }

    /* ---- Rooms ---- */
    void RequestRoomList();
    void JoinRoom(uint8_t roomId);
    void LeaveRoom();

    const std::vector<RoomInfo> &GetRooms() const { return m_rooms; }
    uint8_t GetCurrentRoom()              const { return m_currentRoom; }
    bool    IsInRoom()                    const { return m_currentRoom != 0xFF; }

    /* ---- Users in current room (for OSD) ---- */
    const std::vector<RemoteUser> &GetRoomUsers() const { return m_roomUsers; }

    /* ---- Settings ---- */
    void SetServerAddress(const std::string &addr) { m_serverAddr = addr; }
    const std::string &GetServerAddress()    const { return m_serverAddr; }

    void SetUsername(const std::string &name) { m_username = name; }
    const std::string &GetUsername()    const { return m_username; }

    void SetMuted(bool m);
    bool IsMuted()   const { return m_muted; }
    bool IsTalking() const { return m_audio.IsTalking(); }

    /* ---- OSD overlay ---- */
    void SetOverlayEnabled(bool en);
    bool IsOverlayEnabled() const { return m_overlayEnabled; }

    /* Update room entry names after a ROOM_LIST response.
     * Pass in the array of MenuEntry* for each room slot. */
    void UpdateRoomEntries(CTRPluginFramework::MenuEntry **entries, int count);

private:
    /* Voice processing thread entry */
    static void VoiceThreadEntry(void *arg);
    void VoiceLoop();

    /* TCP control message dispatcher */
    void OnControlMessage(PktType type, const uint8_t *payload, uint16_t len);

    /* UDP voice packet handler */
    void OnVoicePacket(const VoiceHeader &hdr, const uint8_t *data, uint16_t len);

    /* OSD callback registered with CTRPluginFramework */
    static bool OSDCallback(const CTRPluginFramework::Screen &screen);

    /* Helpers */
    RemoteUser *FindUser(uint32_t id);
    void        RemoveUser(uint32_t id);

    /* Server */
    std::string m_serverAddr  = "192.168.1.100";  /* change to your server IP */
    std::string m_username;

    /* State */
    bool    m_connected       = false;
    uint8_t m_currentRoom     = 0xFF;   /* 0xFF = not in a room */
    uint32_t m_sessionId      = 0;
    uint32_t m_userId         = 0;
    bool    m_muted           = false;
    bool    m_overlayEnabled  = false;

    /* Data */
    std::vector<RoomInfo>   m_rooms;
    std::vector<RemoteUser> m_roomUsers;

    /* Subsystems */
    AudioManager   m_audio;
    NetworkManager m_net;

    /* Background voice thread */
    Thread          m_voiceThread = nullptr;
    std::atomic<bool> m_threadRunning{false};
    LightLock       m_userLock;   /* guards m_roomUsers for OSD reads */

    /* UDP sequence counter */
    uint16_t m_udpSeq = 0;

    /* Outgoing encoded frame handler (set in Connect) */
    friend class AudioManager;

    /* Pointer to this instance for the static OSD callback */
    static VoiceChat *s_instance;
};

} /* namespace VoicePlugin */
#endif /* VOICECHAT_HPP */
