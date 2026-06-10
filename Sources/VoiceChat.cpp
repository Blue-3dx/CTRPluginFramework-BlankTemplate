#include "VoiceChat.hpp"
#include <string.h>
#include <3ds.h>

namespace VoicePlugin {

VoiceChat *VoiceChat::s_instance = nullptr;

/* =========================================================
 * Construction / Destruction
 * ========================================================= */

VoiceChat::VoiceChat() {
    LightLock_Init(&m_userLock);
    s_instance = this;
}

VoiceChat::~VoiceChat() {
    Disconnect();
    s_instance = nullptr;
}

/* =========================================================
 * Connect
 * ========================================================= */

bool VoiceChat::Connect() {
    if (m_connected) return true;
    if (m_username.empty()) return false;

    /* Init SOC service (reference-counted; harmless if game already did it) */
    if (!m_net.InitSoc()) return false;

    /* Open TCP + UDP */
    if (!m_net.Connect(m_serverAddr)) {
        m_net.ShutdownSoc();
        return false;
    }

    /* Init audio subsystems */
    if (!m_audio.Init()) {
        m_net.Disconnect();
        m_net.ShutdownSoc();
        return false;
    }

    /* Register network callbacks */
    m_net.SetControlCallback([this](PktType t, const uint8_t *p, uint16_t l) {
        OnControlMessage(t, p, l);
    });
    m_net.SetVoiceCallback([this](const VoiceHeader &h, const uint8_t *d, uint16_t l) {
        OnVoicePacket(h, d, l);
    });

    /* Register audio encode callback → sends UDP */
    m_audio.SetEncodedFrameCallback([this](const uint8_t *data, size_t len) {
        m_net.SendVoice(m_sessionId, m_userId, m_udpSeq++,
                        data, (uint16_t)len);
        /* Update talking status on the server once per session */
        static bool wasTalking = false;
        if (!wasTalking) {
            PktTalkingChange tc;
            tc.user_id = m_userId;
            tc.talking  = 1;
            m_net.SendTCP(PktType::TALKING_START, &tc, sizeof(tc));
            wasTalking = true;
        }
    });

    /* Send HELLO */
    PktHello hello;
    memset(&hello, 0, sizeof(hello));
    strncpy(hello.username, m_username.c_str(), MAX_USERNAME_LEN - 1);
    if (!m_net.SendTCP(PktType::HELLO, &hello, sizeof(hello))) {
        m_audio.Shutdown();
        m_net.Disconnect();
        m_net.ShutdownSoc();
        return false;
    }

    /* Register OSD callback for the talking overlay */
    CTRPluginFramework::OSD::Run(OSDCallback);

    /* Start voice thread (stackSize=64KB, priority=0x3A, cpu=-1) */
    m_threadRunning = true;
    m_voiceThread   = threadCreate(VoiceThreadEntry, this, 64 * 1024, 0x3A, -1, false);
    if (!m_voiceThread) {
        m_threadRunning = false;
        CTRPluginFramework::OSD::Stop(OSDCallback);
        m_audio.Shutdown();
        m_net.Disconnect();
        m_net.ShutdownSoc();
        return false;
    }

    m_connected   = true;
    m_currentRoom = 0xFF;
    m_roomUsers.clear();

    /* Ask for room list immediately */
    RequestRoomList();
    return true;
}

/* =========================================================
 * Disconnect
 * ========================================================= */

void VoiceChat::Disconnect() {
    if (!m_connected && !m_threadRunning) return;

    /* Signal voice thread to stop */
    m_threadRunning = false;
    if (m_voiceThread) {
        threadJoin(m_voiceThread, U64_MAX);
        threadFree(m_voiceThread);
        m_voiceThread = nullptr;
    }

    CTRPluginFramework::OSD::Stop(OSDCallback);

    if (m_net.IsConnected()) {
        /* Politely leave room first */
        if (m_currentRoom != 0xFF) {
            m_net.SendTCP(PktType::LEAVE_ROOM);
        }
        m_net.SendTCP(PktType::PING); /* flush; ignore response */
    }

    m_audio.Shutdown();
    m_net.Disconnect();
    m_net.ShutdownSoc();

    m_connected   = false;
    m_sessionId   = 0;
    m_userId      = 0;
    m_currentRoom = 0xFF;
    m_udpSeq      = 0;

    LightLock_Lock(&m_userLock);
    m_roomUsers.clear();
    LightLock_Unlock(&m_userLock);
}

/* =========================================================
 * Room control
 * ========================================================= */

void VoiceChat::RequestRoomList() {
    if (!m_net.IsConnected()) return;
    m_net.SendTCP(PktType::ROOM_LIST_REQ);
}

void VoiceChat::JoinRoom(uint8_t roomId) {
    if (!m_net.IsConnected()) return;
    if (m_currentRoom != 0xFF) LeaveRoom();

    PktJoinRoom jr;
    jr.room_id = roomId;
    m_net.SendTCP(PktType::JOIN_ROOM, &jr, sizeof(jr));
}

void VoiceChat::LeaveRoom() {
    if (!m_net.IsConnected()) return;
    m_net.SendTCP(PktType::LEAVE_ROOM);
    m_currentRoom = 0xFF;

    LightLock_Lock(&m_userLock);
    m_roomUsers.clear();
    LightLock_Unlock(&m_userLock);
}

/* =========================================================
 * Settings
 * ========================================================= */

void VoiceChat::SetMuted(bool m) {
    m_muted = m;
    m_audio.SetMuted(m);

    if (m_net.IsConnected() && m_currentRoom != 0xFF) {
        PktTalkingChange tc;
        tc.user_id = m_userId;
        tc.talking  = m ? 0 : 1;
        m_net.SendTCP(m ? PktType::TALKING_STOP : PktType::TALKING_START,
                      &tc, sizeof(tc));
    }
}

void VoiceChat::SetOverlayEnabled(bool en) {
    m_overlayEnabled = en;
}

/* =========================================================
 * Update room entry display names (called from menu after refresh)
 * ========================================================= */

void VoiceChat::UpdateRoomEntries(CTRPluginFramework::MenuEntry **entries, int count) {
    for (int i = 0; i < count && i < (int)m_rooms.size(); i++) {
        const RoomInfo &r = m_rooms[i];
        std::string name  = r.name;
        name += " (";
        name += std::to_string(r.count);
        name += "/";
        name += std::to_string(r.max);
        name += ")";
        entries[i]->Name() = name;
    }
}

/* =========================================================
 * Voice background thread
 * ========================================================= */

void VoiceChat::VoiceThreadEntry(void *arg) {
    static_cast<VoiceChat *>(arg)->VoiceLoop();
}

void VoiceChat::VoiceLoop() {
    /* ~20 ms loop interval */
    static const s64 LOOP_NS = 20000000LL;

    bool prevTalking = false;

    while (m_threadRunning) {
        u64 t0 = svcGetSystemTick();

        /* --- Network poll --- */
        m_net.PollTCP();
        m_net.PollUDP();

        /* --- Audio: capture + encode → UDP (inside encode callback) --- */
        if (m_currentRoom != 0xFF) {
            m_audio.CaptureAndEncode();
            m_audio.FlushToSpeaker();

            /* Talking status changes → inform server */
            bool talking = m_audio.IsTalking();
            if (talking != prevTalking) {
                prevTalking = talking;
                PktTalkingChange tc;
                tc.user_id = m_userId;
                tc.talking  = talking ? 1 : 0;
                m_net.SendTCP(talking ? PktType::TALKING_START : PktType::TALKING_STOP,
                              &tc, sizeof(tc));
            }
        }

        /* Sleep remainder of frame */
        s64 elapsed = (s64)(svcGetSystemTick() - t0);
        s64 ticksPerNs = SYSCLOCK_ARM11 / 1000000000LL;
        s64 remaining  = (LOOP_NS * ticksPerNs) - elapsed;
        if (remaining > 0) {
            svcSleepThread(remaining / ticksPerNs);
        }
    }
}

/* =========================================================
 * TCP control message dispatcher
 * ========================================================= */

void VoiceChat::OnControlMessage(PktType type, const uint8_t *payload, uint16_t len) {
    switch (type) {

    case PktType::HELLO_ACK: {
        if (len < sizeof(PktHelloAck)) break;
        const auto *ack = (const PktHelloAck *)payload;
        m_sessionId = ack->session_id;
        m_userId    = ack->session_id;   /* server uses same id for simplicity */

        /* Now send UDP hello so server knows our UDP endpoint */
        /* (done via a zero-len voice packet) */
        VoiceHeader vh;
        memset(&vh, 0, sizeof(vh));
        vh.session_id = m_sessionId;
        vh.data_len   = 0;
        m_net.SendVoice(m_sessionId, m_userId, 0, nullptr, 0);
        break;
    }

    case PktType::ROOM_LIST: {
        if (len < 1) break;
        const auto *prl = (const PktRoomList *)payload;
        m_rooms.clear();
        int n = prl->count;
        if (n > MAX_ROOMS) n = MAX_ROOMS;
        for (int i = 0; i < n; i++) {
            RoomInfo r;
            r.id    = prl->rooms[i].id;
            r.name  = std::string(prl->rooms[i].name, ROOM_NAME_LEN);
            r.count = prl->rooms[i].count;
            r.max   = prl->rooms[i].max;
            m_rooms.push_back(r);
        }
        break;
    }

    case PktType::JOIN_ROOM_ACK: {
        if (len < sizeof(PktJoinRoomAck)) break;
        const auto *ack = (const PktJoinRoomAck *)payload;
        m_currentRoom = ack->room_id;

        LightLock_Lock(&m_userLock);
        m_roomUsers.clear();
        for (int i = 0; i < ack->user_count; i++) {
            RemoteUser u;
            u.id       = ack->users[i].id;
            u.username = std::string(ack->users[i].username, MAX_USERNAME_LEN);
            u.talking  = ack->users[i].talking != 0;
            m_roomUsers.push_back(u);
        }
        LightLock_Unlock(&m_userLock);
        break;
    }

    case PktType::USER_JOINED: {
        if (len < sizeof(PktUserJoined)) break;
        const auto *puj = (const PktUserJoined *)payload;
        LightLock_Lock(&m_userLock);
        RemoteUser u;
        u.id       = puj->user.id;
        u.username = std::string(puj->user.username, MAX_USERNAME_LEN);
        u.talking  = puj->user.talking != 0;
        if (m_roomUsers.size() < MAX_USERS_PER_ROOM) {
            m_roomUsers.push_back(u);
        }
        LightLock_Unlock(&m_userLock);
        break;
    }

    case PktType::USER_LEFT: {
        if (len < sizeof(PktUserLeft)) break;
        const auto *pul = (const PktUserLeft *)payload;
        m_audio.RemoveDecoder(pul->user_id);
        LightLock_Lock(&m_userLock);
        RemoveUser(pul->user_id);
        LightLock_Unlock(&m_userLock);
        break;
    }

    case PktType::TALKING_CHANGE: {
        if (len < sizeof(PktTalkingChange)) break;
        const auto *ptc = (const PktTalkingChange *)payload;
        LightLock_Lock(&m_userLock);
        if (RemoteUser *u = FindUser(ptc->user_id)) {
            u->talking = ptc->talking != 0;
        }
        LightLock_Unlock(&m_userLock);
        break;
    }

    default:
        break;
    }
}

/* =========================================================
 * UDP voice packet handler
 * ========================================================= */

void VoiceChat::OnVoicePacket(const VoiceHeader &hdr, const uint8_t *data, uint16_t len) {
    /* Ignore our own voice bounced back (shouldn't happen with a good server) */
    if (hdr.user_id == m_userId) return;
    if (len == 0) return;   /* UDP hello probe */

    m_audio.DecodeIncoming(hdr.user_id, data, len);
}

/* =========================================================
 * OSD Callback (called every frame by CTRPF, any thread)
 * ========================================================= */

bool VoiceChat::OSDCallback(const CTRPluginFramework::Screen &screen) {
    VoiceChat *vc = s_instance;
    if (!vc || !vc->m_overlayEnabled || !vc->m_connected) return false;
    if (!screen.IsTop) return false;   /* draw only on top screen */

    using namespace CTRPluginFramework;

    /* Position: top-left corner, small text */
    int x = 5, y = 5;

    /* Room label */
    std::string label = "VC: ";
    if (vc->m_currentRoom == 0xFF) {
        label += "(lobby)";
    } else {
        label += "Room ";
        label += std::to_string((int)vc->m_currentRoom);
    }

    /* Muted indicator */
    if (vc->m_muted) label += " [MUTED]";

    screen.Draw(label, x, y, Color::Yellow, Color::Black);
    y += 12;

    /* User list - lock briefly to read */
    if (LightLock_TryLock(&vc->m_userLock) == 0) {
        for (const auto &u : vc->m_roomUsers) {
            Color c = u.talking ? Color::Lime : Color::White;
            screen.Draw(u.username, x + 4, y, c, Color::Black);
            y += 12;
        }
        LightLock_Unlock(&vc->m_userLock);
    }

    return true;
}

/* =========================================================
 * Helpers
 * ========================================================= */

RemoteUser *VoiceChat::FindUser(uint32_t id) {
    for (auto &u : m_roomUsers) {
        if (u.id == id) return &u;
    }
    return nullptr;
}

void VoiceChat::RemoveUser(uint32_t id) {
    for (auto it = m_roomUsers.begin(); it != m_roomUsers.end(); ++it) {
        if (it->id == id) { m_roomUsers.erase(it); return; }
    }
}

} /* namespace VoicePlugin */
