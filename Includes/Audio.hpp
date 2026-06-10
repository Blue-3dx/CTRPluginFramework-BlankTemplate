#pragma once
#ifndef AUDIO_HPP
#define AUDIO_HPP

#include <3ds.h>
#include <opus/opus.h>
#include <stdint.h>
#include <stddef.h>
#include <functional>

namespace VoicePlugin {

/*
 * AudioManager
 * ------------
 * Wraps:
 *   - MICU (microphone) at 16360 Hz PCM16, read as 16000 Hz for Opus
 *   - NDSP channel 23 for speaker output (avoids conflicts with game channels)
 *   - One Opus encoder (outgoing voice)
 *   - Up to MAX_USERS_PER_ROOM Opus decoders (incoming voice, keyed by user_id)
 *
 * Voice Activity Detection is amplitude-based (simple but effective).
 */

#define AUDIO_MIC_RATE        16360    /* MICU_SAMPLE_RATE_16360 in Hz    */
#define AUDIO_OPUS_RATE       16000    /* Opus encoder sample rate        */
#define AUDIO_FRAME_SAMPLES   320      /* 20 ms @ 16 kHz                  */
#define AUDIO_MIC_BUF_SIZE    (0x8000) /* 32 KB mic ring buffer           */
#define AUDIO_NDSP_CHANNEL    23       /* NDSP channel (avoid game audio) */
#define AUDIO_NDSP_BUFS       4        /* double-buffer playback queue    */
#define AUDIO_VAD_THRESHOLD   600      /* RMS threshold for voice detect  */
#define AUDIO_MAX_DECODERS    5        /* max simultaneous remote users   */
#define AUDIO_OPUS_MAX_BYTES  400      /* max encoded bytes per frame     */

/* Called by AudioManager when an encoded frame is ready to send */
using EncodedFrameCallback = std::function<void(const uint8_t *data, size_t len)>;

class AudioManager {
public:
    AudioManager();
    ~AudioManager();

    /* Call once at startup. Returns false on fatal error. */
    bool Init();
    void Shutdown();

    /* Register callback invoked with each outgoing encoded Opus frame */
    void SetEncodedFrameCallback(EncodedFrameCallback cb) { m_encCallback = cb; }

    /*
     * Call in the voice thread loop (~every 20 ms).
     * Reads new mic samples → VAD → Opus encode → invokes callback.
     */
    void CaptureAndEncode();

    /*
     * Feed an incoming Opus packet for a specific remote user.
     * Decodes into an internal per-user PCM buffer.
     */
    void DecodeIncoming(uint32_t userId, const uint8_t *opusData, size_t len);

    /* Mix all decoded user buffers and submit to NDSP. Call in voice loop. */
    void FlushToSpeaker();

    /* Remove a user's decoder (they disconnected) */
    void RemoveDecoder(uint32_t userId);

    void SetMuted(bool muted)         { m_muted = muted; }
    bool IsMuted()              const { return m_muted; }
    bool IsTalking()            const { return m_isTalking; }
    bool IsInited()             const { return m_inited; }
    bool IsMicAvailable()       const { return m_micInited; }
    bool IsSpeakerAvailable()   const { return m_ndspInited; }

private:
    bool InitMic();
    bool InitNdsp();
    bool InitOpus();
    void ShutdownMic();
    void ShutdownNdsp();
    void ShutdownOpus();

    /* Find or create a decoder slot for userId. Returns nullptr if full. */
    OpusDecoder *GetOrCreateDecoder(uint32_t userId);

    /* True if RMS of samples exceeds VAD_THRESHOLD */
    bool CheckVAD(const int16_t *samples, int count);

    /* Mic state */
    u8  *m_micBuffer   = nullptr;
    u32  m_micReadOff  = 0;          /* offset of our last read in ring buf */
    bool m_micInited   = false;

    /* NDSP state - linear memory required for DMA */
    bool         m_ndspInited                          = false;
    ndspWaveBuf  m_waveBufs[AUDIO_NDSP_BUFS];
    int16_t     *m_ndspPcm[AUDIO_NDSP_BUFS]            = {};
    int          m_ndspBufIdx                          = 0;

    /* Mix buffer accumulates all decoded streams before NDSP submit */
    int32_t      m_mixBuf[AUDIO_FRAME_SAMPLES]         = {};

    /* Opus */
    OpusEncoder *m_encoder                             = nullptr;
    OpusDecoder *m_decoders[AUDIO_MAX_DECODERS]        = {};
    uint32_t     m_decoderIds[AUDIO_MAX_DECODERS]      = {};
    int16_t      m_decodedBufs[AUDIO_MAX_DECODERS][AUDIO_FRAME_SAMPLES] = {};
    uint8_t      m_encPacket[AUDIO_OPUS_MAX_BYTES]     = {};

    bool m_inited    = false;
    bool m_muted     = false;
    bool m_isTalking = false;

    EncodedFrameCallback m_encCallback;
};

} /* namespace VoicePlugin */
#endif /* AUDIO_HPP */