#include "Audio.hpp"
#include <3ds.h>
#include <malloc.h>
#include <string.h>
#include <stdint.h>
#include <cmath>

namespace VoicePlugin {

AudioManager::AudioManager() {
    memset(m_waveBufs,   0, sizeof(m_waveBufs));
    memset(m_decoderIds, 0, sizeof(m_decoderIds));
    memset(m_mixBuf,     0, sizeof(m_mixBuf));
}

AudioManager::~AudioManager() {
    Shutdown();
}

bool AudioManager::Init() {
    if (m_inited) return true;

    if (!InitOpus()) return false;  /* opus first, cheapest failure */
    InitMic();      /* non-fatal: some games own the mic */
    InitNdsp();     /* non-fatal: some games own all DSP channels */

    m_inited = true;
    return true;
}

void AudioManager::Shutdown() {
    if (!m_inited) return;
    ShutdownNdsp();
    ShutdownMic();
    ShutdownOpus();
    m_inited    = false;
    m_isTalking = false;
}

/* =========================================================
 * Microphone
 * ========================================================= */

bool AudioManager::InitMic() {
    /* Allocate ring buffer in regular FCRAM; mic DMA writes here */
    m_micBuffer = (u8 *)memalign(0x1000, AUDIO_MIC_BUF_SIZE);
    if (!m_micBuffer) return false;

    memset(m_micBuffer, 0, AUDIO_MIC_BUF_SIZE);

    if (R_FAILED(micInit(m_micBuffer, AUDIO_MIC_BUF_SIZE))) {
        free(m_micBuffer);
        m_micBuffer = nullptr;
        return false;
    }

    /*
     * MICU_SAMPLE_RATE_16360 ≈ 16 kHz
     * We capture raw PCM16_SIGNED into the ring buffer.
     * loop=true keeps capturing continuously.
     */
    if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED,
                                    MICU_SAMPLE_RATE_16360,
                                    0,
                                    AUDIO_MIC_BUF_SIZE,
                                    true))) {
        micExit();
        free(m_micBuffer);
        m_micBuffer = nullptr;
        return false;
    }

    m_micReadOff = 0;
    m_micInited  = true;
    return true;
}

void AudioManager::ShutdownMic() {
    if (!m_micInited) return;
    MICU_StopSampling();
    micExit();
    if (m_micBuffer) { free(m_micBuffer); m_micBuffer = nullptr; }
    m_micInited = false;
}

/* =========================================================
 * NDSP Speaker
 * ========================================================= */

bool AudioManager::InitNdsp() {
    if (R_FAILED(ndspInit())) return false;

    ndspSetOutputMode(NDSP_OUTPUT_MONO);

    ndspChnReset(AUDIO_NDSP_CHANNEL);
    ndspChnInitParams(AUDIO_NDSP_CHANNEL);
    ndspChnSetInterp(AUDIO_NDSP_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AUDIO_NDSP_CHANNEL, (float)AUDIO_OPUS_RATE);
    ndspChnSetFormat(AUDIO_NDSP_CHANNEL, NDSP_FORMAT_MONO_PCM16);

    /* Allocate wave buffers in linear (DSP-accessible) memory */
    for (int i = 0; i < AUDIO_NDSP_BUFS; i++) {
        m_ndspPcm[i] = (int16_t *)linearAlloc(AUDIO_FRAME_SAMPLES * sizeof(int16_t));
        if (!m_ndspPcm[i]) {
            ShutdownNdsp();
            return false;
        }
        memset(m_ndspPcm[i], 0, AUDIO_FRAME_SAMPLES * sizeof(int16_t));
        memset(&m_waveBufs[i], 0, sizeof(ndspWaveBuf));
        m_waveBufs[i].data_vaddr = m_ndspPcm[i];
        m_waveBufs[i].nsamples   = AUDIO_FRAME_SAMPLES;
        m_waveBufs[i].status     = NDSP_WBUF_DONE;
    }

    m_ndspInited = true;
    return true;
}

void AudioManager::ShutdownNdsp() {
    if (!m_ndspInited) return;
    ndspChnReset(AUDIO_NDSP_CHANNEL);
    ndspExit();
    for (int i = 0; i < AUDIO_NDSP_BUFS; i++) {
        if (m_ndspPcm[i]) { linearFree(m_ndspPcm[i]); m_ndspPcm[i] = nullptr; }
    }
    m_ndspInited = false;
}

/* =========================================================
 * Opus
 * ========================================================= */

bool AudioManager::InitOpus() {
    int err = 0;
    m_encoder = opus_encoder_create(AUDIO_OPUS_RATE, AUDIO_CHANNELS, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !m_encoder) return false;

    /* Low complexity for ARM11 */
    opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(m_encoder, OPUS_SET_DTX(1));          /* skip silent frames */

    return true;
}

void AudioManager::ShutdownOpus() {
    if (m_encoder) { opus_encoder_destroy(m_encoder); m_encoder = nullptr; }
    for (int i = 0; i < AUDIO_MAX_DECODERS; i++) {
        if (m_decoders[i]) { opus_decoder_destroy(m_decoders[i]); m_decoders[i] = nullptr; }
        m_decoderIds[i] = 0;
    }
}

OpusDecoder *AudioManager::GetOrCreateDecoder(uint32_t userId) {
    /* Look for existing slot */
    for (int i = 0; i < AUDIO_MAX_DECODERS; i++) {
        if (m_decoderIds[i] == userId && m_decoders[i]) return m_decoders[i];
    }
    /* Find free slot */
    for (int i = 0; i < AUDIO_MAX_DECODERS; i++) {
        if (!m_decoders[i]) {
            int err = 0;
            m_decoders[i] = opus_decoder_create(AUDIO_OPUS_RATE, AUDIO_CHANNELS, &err);
            if (err != OPUS_OK) return nullptr;
            m_decoderIds[i] = userId;
            return m_decoders[i];
        }
    }
    return nullptr; /* room full - shouldn't happen with MAX_USERS_PER_ROOM=5 */
}

void AudioManager::RemoveDecoder(uint32_t userId) {
    for (int i = 0; i < AUDIO_MAX_DECODERS; i++) {
        if (m_decoderIds[i] == userId && m_decoders[i]) {
            opus_decoder_destroy(m_decoders[i]);
            m_decoders[i]    = nullptr;
            m_decoderIds[i]  = 0;
            memset(m_decodedBufs[i], 0, sizeof(m_decodedBufs[i]));
        }
    }
}

/* =========================================================
 * VAD (Voice Activity Detection)
 * ========================================================= */

bool AudioManager::CheckVAD(const int16_t *samples, int count) {
    /* Simple RMS amplitude check */
    int64_t sum = 0;
    for (int i = 0; i < count; i++) {
        int32_t s = samples[i];
        sum += s * s;
    }
    int32_t rms = (int32_t)sqrt((double)(sum / count));
    return rms > AUDIO_VAD_THRESHOLD;
}

/* =========================================================
 * Capture & Encode  (called in voice thread, ~every 20 ms)
 * ========================================================= */

void AudioManager::CaptureAndEncode() {
    if (!m_micInited || !m_encoder) return;

    /* How many bytes have been written to the ring buffer since our last read? */
    u32 hwOffset = 0;
    micGetLastSampleOffset(&hwOffset);

    u32 available;
    if (hwOffset >= m_micReadOff) {
        available = hwOffset - m_micReadOff;
    } else {
        available = (AUDIO_MIC_BUF_SIZE - m_micReadOff) + hwOffset;
    }

    /* One opus frame = AUDIO_FRAME_SAMPLES PCM16 samples = frame * 2 bytes */
    const u32 frameBytes = AUDIO_FRAME_SAMPLES * sizeof(int16_t);
    if (available < frameBytes) return;   /* not enough data yet */

    /* Collect one frame's worth of samples (handling wrap) */
    int16_t pcm[AUDIO_FRAME_SAMPLES];
    u32 end = m_micReadOff + frameBytes;

    if (end <= AUDIO_MIC_BUF_SIZE) {
        memcpy(pcm, m_micBuffer + m_micReadOff, frameBytes);
    } else {
        u32 part1 = AUDIO_MIC_BUF_SIZE - m_micReadOff;
        memcpy(pcm,          m_micBuffer + m_micReadOff, part1);
        memcpy(pcm + (part1 / sizeof(int16_t)), m_micBuffer, frameBytes - part1);
    }
    m_micReadOff = (m_micReadOff + frameBytes) % AUDIO_MIC_BUF_SIZE;

    /* Voice Activity Detection */
    bool talking = !m_muted && CheckVAD(pcm, AUDIO_FRAME_SAMPLES);
    m_isTalking  = talking;

    if (!talking) return;   /* DTX: skip encoding silent frames */

    /* Opus encode */
    int encoded = opus_encode(m_encoder, pcm, AUDIO_FRAME_SAMPLES,
                              m_encPacket, AUDIO_OPUS_MAX_BYTES);
    if (encoded <= 0) return;

    if (m_encCallback) {
        m_encCallback(m_encPacket, (size_t)encoded);
    }
}

/* =========================================================
 * Decode incoming Opus (called in voice thread per packet)
 * ========================================================= */

void AudioManager::DecodeIncoming(uint32_t userId, const uint8_t *opusData, size_t len) {
    OpusDecoder *dec = GetOrCreateDecoder(userId);
    if (!dec) return;

    /* Find slot index for this decoder to know which decode buffer to use */
    int slot = -1;
    for (int i = 0; i < AUDIO_MAX_DECODERS; i++) {
        if (m_decoderIds[i] == userId) { slot = i; break; }
    }
    if (slot < 0) return;

    int samples = opus_decode(dec,
                              opusData, (opus_int32)len,
                              m_decodedBufs[slot],
                              AUDIO_FRAME_SAMPLES,
                              0 /* no FEC */);
    if (samples <= 0) {
        /* Packet loss concealment */
        int plc = opus_decode(dec, nullptr, 0, m_decodedBufs[slot], AUDIO_FRAME_SAMPLES, 1);
        (void)plc;
    }
}

/* =========================================================
 * Mix all decoded streams → NDSP
 * ========================================================= */

void AudioManager::FlushToSpeaker() {
    if (!m_ndspInited) return;

    /* Check if the current wave buffer is free */
    ndspWaveBuf *wb = &m_waveBufs[m_ndspBufIdx];
    if (wb->status != NDSP_WBUF_DONE) return;   /* still playing */

    /* Zero the mix buffer */
    memset(m_mixBuf, 0, sizeof(m_mixBuf));

    /* Accumulate all active decoder outputs */
    for (int d = 0; d < AUDIO_MAX_DECODERS; d++) {
        if (!m_decoders[d]) continue;
        for (int s = 0; s < AUDIO_FRAME_SAMPLES; s++) {
            m_mixBuf[s] += (int32_t)m_decodedBufs[d][s];
        }
    }

    /* Clamp and write to NDSP PCM buffer */
    int16_t *dst = m_ndspPcm[m_ndspBufIdx];
    for (int s = 0; s < AUDIO_FRAME_SAMPLES; s++) {
        int32_t v = m_mixBuf[s];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        dst[s] = (int16_t)v;
    }

    /* Flush D-cache so DSP sees the new data */
    DSP_FlushDataCache(dst, AUDIO_FRAME_SAMPLES * sizeof(int16_t));

    /* Resubmit */
    wb->nsamples = AUDIO_FRAME_SAMPLES;
    wb->status   = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(AUDIO_NDSP_CHANNEL, wb);

    m_ndspBufIdx = (m_ndspBufIdx + 1) % AUDIO_NDSP_BUFS;
}

} /* namespace VoicePlugin */
