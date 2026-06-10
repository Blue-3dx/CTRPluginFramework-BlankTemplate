#pragma once
#ifndef PROTOCOL_HPP
#define PROTOCOL_HPP

/*
 * Voice Chat Protocol
 * -------------------
 * TCP port 7001  - Control messages (connect, rooms, user events)
 * UDP port 7002  - Opus voice packets
 *
 * All multi-byte integers are little-endian (native ARM / x86-64).
 */

#include <stdint.h>

#define VC_TCP_PORT          7001
#define VC_UDP_PORT          7002
#define MAX_ROOMS            8
#define MAX_USERS_PER_ROOM   5
#define MAX_USERNAME_LEN     16   /* including null terminator */
#define ROOM_NAME_LEN        16

/* Opus config - 16 kHz mono, 20 ms frames */
#define OPUS_SAMPLE_RATE     16000
#define OPUS_CHANNELS        1
#define OPUS_FRAME_MS        20
#define OPUS_FRAME_SAMPLES   320   /* 16000 * 20 / 1000 */
#define OPUS_MAX_PACKET      400   /* max compressed bytes per frame */

/* ------------------------------------------------------------------ */
/* Control message type byte                                           */
/* ------------------------------------------------------------------ */
enum class PktType : uint8_t {
    /* Client → Server */
    HELLO           = 0x01,   /* first message after TCP connect       */
    ROOM_LIST_REQ   = 0x03,   /* request room list                     */
    JOIN_ROOM       = 0x05,   /* join a room by id                     */
    LEAVE_ROOM      = 0x07,   /* leave current room                    */
    TALKING_START   = 0x0A,   /* local VAD fired                       */
    TALKING_STOP    = 0x0B,   /* local VAD ended                       */
    PING            = 0x0C,

    /* Server → Client */
    HELLO_ACK       = 0x02,   /* session_id assigned                   */
    ROOM_LIST       = 0x04,   /* room list response                    */
    JOIN_ROOM_ACK   = 0x06,   /* joined; includes current user list    */
    USER_JOINED     = 0x08,   /* another user joined our room          */
    USER_LEFT       = 0x09,   /* a user left our room                  */
    TALKING_CHANGE  = 0x0D,   /* talking status broadcast              */
    PONG            = 0x0E,
    ERROR           = 0xFF
};

/* ------------------------------------------------------------------ */
/* Packet layout                                                       */
/* Every TCP message: [PktHeader][payload bytes]                       */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)

struct PktHeader {
    PktType  type;
    uint16_t length;   /* payload bytes that follow this header */
};

/* ---- HELLO (client → server) ---- */
struct PktHello {
    char username[MAX_USERNAME_LEN];
};

/* ---- HELLO_ACK (server → client) ---- */
struct PktHelloAck {
    uint32_t session_id;
};

/* ---- ROOM_LIST (server → client) ---- */
struct RoomEntry {
    uint8_t  id;
    char     name[ROOM_NAME_LEN];
    uint8_t  count;
    uint8_t  max;
};

struct PktRoomList {
    uint8_t   count;
    RoomEntry rooms[MAX_ROOMS];
};

/* ---- JOIN_ROOM (client → server) ---- */
struct PktJoinRoom {
    uint8_t room_id;
};

/* ---- JOIN_ROOM_ACK (server → client) ---- */
struct UserEntry {
    uint32_t id;
    char     username[MAX_USERNAME_LEN];
    uint8_t  talking;   /* 0 or 1 */
};

struct PktJoinRoomAck {
    uint8_t   room_id;
    uint8_t   user_count;
    UserEntry users[MAX_USERS_PER_ROOM];
};

/* ---- USER_JOINED (server → room members) ---- */
struct PktUserJoined {
    UserEntry user;
};

/* ---- USER_LEFT (server → room members) ---- */
struct PktUserLeft {
    uint32_t user_id;
};

/* ---- TALKING_START/STOP (bidirectional) ---- */
struct PktTalkingChange {
    uint32_t user_id;
    uint8_t  talking;   /* 1 = started, 0 = stopped */
};

/* ---- ERROR (server → client) ---- */
struct PktError {
    char message[64];
};

/* ------------------------------------------------------------------ */
/* UDP Voice Packet                                                    */
/* Layout: [VoiceHeader][opus_data bytes]                             */
/* ------------------------------------------------------------------ */
struct VoiceHeader {
    uint32_t session_id;   /* sender's session id (for server routing) */
    uint32_t user_id;      /* sender's user id   (for client display)  */
    uint16_t sequence;     /* monotonically increasing per sender      */
    uint16_t data_len;     /* opus payload bytes that follow           */
};

#pragma pack(pop)

#endif /* PROTOCOL_HPP */