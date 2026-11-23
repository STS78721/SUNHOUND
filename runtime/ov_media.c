// Adapted from MacNativeGames games/totk/runtime/opus_codec.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// The hardware Opus decoder (nn::codec), decoded with libopus on the host. The software keyboard (nn::swkbd) is
// in swkbd_win.c.
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <opus/opus.h>

#include "host.h"
#include "platform/platform.h"

#define CODEC_R_INVALID 0x1fcd1u   // MacNativeGames' "invalid argument" result for nn::codec
#define OPUS_WORK_SIZE 0x9000u     // what the game is told to reserve: libopus stereo decoder state (about 26 KiB) plus 4 KiB
#define OPUS_MAX_FRAME_SAMPLES 5760

typedef struct OpusJob {
    uint8_t *pkt;
    size_t pkt_len;
    int16_t *pcm;
    int samples;
    struct OpusJob *next;
} OpusJob;

typedef struct OpusMgr {
    uint64_t guest;
    uint64_t event;
    int max_decoders, max_channels, max_packet_size, io_buffer_count, used_io_buffers;
    PlatMutex lock;
    struct OpusMgr *next;
} OpusMgr;

// One record per initialised decoder object (the guest's HardwareOpusDecoderType). The registry lock only guards the
// list; a decoder's own lock serialises its libopus state, so different streams decode in parallel. Lock order:
// registry, then decoder (a caller takes the decoder lock before it drops the registry lock, so Finalize cannot free a
// decoder another thread is using).
typedef struct Decoder {
    uint64_t guest;
    OpusDecoder *dec;
    int rate, channels;
    OpusMgr *mgr;
    OpusJob *wait_head, *wait_tail;
    int wait_count;
    OpusJob *done_head, *done_tail;
    int done_count;
    PlatMutex use;
    struct Decoder *next;
} Decoder;
static Decoder *decoders;
static OpusMgr *managers;
static PlatMutex lock = PLAT_MUTEX_INIT;

static void opus_job_free_list(OpusJob *j, OpusMgr *mgr) {
    int n = 0;
    while (j) {
        OpusJob *nx = j->next;
        free(j->pkt);
        free(j->pcm);
        free(j);
        j = nx;
        n++;
    }
    if (mgr && n) {
        plat_mutex_lock(&mgr->lock);
        mgr->used_io_buffers -= n;
        if (mgr->used_io_buffers < 0) mgr->used_io_buffers = 0;
        plat_mutex_unlock(&mgr->lock);
    }
}

static int valid_format(int rate, int channels) {
    return (rate == 48000 || rate == 24000 || rate == 16000 || rate == 12000 || rate == 8000) &&
           (channels == 1 || channels == 2);
}

// Returns the decoder with its use lock held.
static Decoder *acquire(uint64_t guest, const char *fn) {
    plat_mutex_lock_shared(&lock);
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) plat_mutex_lock(&d->use);
    plat_mutex_unlock_shared(&lock);
    if (!d) rt_fatal("nn::codec::%s: unknown hardware Opus decoder %#llx", fn, (unsigned long long)guest);
    return d;
}

static void release(Decoder *d) { plat_mutex_unlock(&d->use); }

static OpusDecoder *opus_new(int rate, int channels) {
    int err = 0;
    OpusDecoder *o = opus_decoder_create(rate, channels, &err);
    if (!o || err != OPUS_OK) rt_fatal("nn::codec: opus_decoder_create(%d, %d): %s", rate, channels, opus_strerror(err));
    return o;
}

// size_t GetHardwareOpusDecoderWorkBufferSize(int rate, int channels, int option)
static void ov_work_size(Cpu *c) {
    ret_x(c, valid_format((int)arg_x(c, 0), (int)arg_x(c, 1)) ? OPUS_WORK_SIZE : 0);
}

// Result InitializeHardwareOpusDecoder(HardwareOpusDecoderType *d, int rate, int channels, void *work, size_t size,
// int option). The state lives in libopus on the host; the guest's work buffer is reserved but unused. Initialising
// the same object again without Finalize replaces its decoder.
static void ov_initialize(Cpu *c) {
    uint64_t guest = arg_x(c, 0), size = arg_x(c, 4);
    int rate = (int)arg_x(c, 1), channels = (int)arg_x(c, 2);
    if (!guest || !valid_format(rate, channels) || size < OPUS_WORK_SIZE) { ret_x(c, CODEC_R_INVALID); return; }
    OpusDecoder *fresh = opus_new(rate, channels);
    plat_mutex_lock(&lock);
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) {
        plat_mutex_lock(&d->use);   // waits for a decode in flight
        OpusDecoder *old = d->dec;
        d->dec = fresh;
        d->rate = rate;
        d->channels = channels;
        plat_mutex_unlock(&d->use);
        plat_mutex_unlock(&lock);
        opus_decoder_destroy(old);
    } else {
        d = calloc(1, sizeof *d);
        if (!d) rt_fatal("nn::codec: decoder allocation");
        plat_mutex_init(&d->use);
        d->guest = guest;
        d->dec = fresh;
        d->rate = rate;
        d->channels = channels;
        d->next = decoders;
        decoders = d;
        plat_mutex_unlock(&lock);
    }
    ret_x(c, 0);
}

// void FinalizeHardwareOpusDecoder(HardwareOpusDecoderType *d)
static void ov_finalize(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    plat_mutex_lock(&lock);
    Decoder **p = &decoders;
    while (*p && (*p)->guest != guest) p = &(*p)->next;
    if (!*p) rt_fatal("nn::codec::FinalizeHardwareOpusDecoder: unknown decoder %#llx", (unsigned long long)guest);
    Decoder *d = *p;
    plat_mutex_lock(&d->use);   // a decode in flight finishes first; the registry lock keeps new ones out
    *p = d->next;
    plat_mutex_unlock(&lock);
    opus_job_free_list(d->wait_head, d->mgr);
    opus_job_free_list(d->done_head, d->mgr);
    opus_decoder_destroy(d->dec);
    free(d);
}

// int GetHardwareOpusDecoderSampleRate / GetHardwareOpusDecoderChannelCount(const HardwareOpusDecoderType *d)
static void ov_sample_rate(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderSampleRate");
    int v = d->rate;
    release(d);
    ret_x(c, (uint64_t)v);
}

static void ov_channel_count(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderChannelCount");
    int v = d->channels;
    release(d);
    ret_x(c, (uint64_t)v);
}

// Result ResetHardwareOpusDecoder(HardwareOpusDecoderType *d)
static void ov_reset(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "ResetHardwareOpusDecoder");
    int ok = opus_decoder_ctl(d->dec, OPUS_RESET_STATE);
    opus_job_free_list(d->wait_head, d->mgr);
    opus_job_free_list(d->done_head, d->mgr);
    d->wait_head = d->wait_tail = NULL;
    d->done_head = d->done_tail = NULL;
    d->wait_count = d->done_count = 0;
    release(d);
    ret_x(c, ok == OPUS_OK ? 0 : CODEC_R_INVALID);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Result DecodeOpusInterleavedWithHardware(HardwareOpusDecoderType *d, size_t *consumed, int *samples, int16_t *out,
// size_t out_size, const void *in, size_t in_size). The input is Nintendo's frame: an 8-byte big-endian header
// {payload size, final range} and the Opus packet. A header range of 0 is not a recorded value (some encoders write 0
// into every packet), so it is not checked. A range mismatch is logged once and the samples are kept.
static void ov_decode(Cpu *c) {
    static int32_t warned;
    uint64_t guest = arg_x(c, 0), out_size = arg_x(c, 4), in_size = arg_x(c, 6);
    size_t *consumed = arg_p(c, 1);
    int *samples = arg_p(c, 2);
    int16_t *out = arg_p(c, 3);
    const uint8_t *in = arg_p(c, 5);
    if (!consumed || !samples || !out || !in || in_size < 8) { ret_x(c, CODEC_R_INVALID); return; }
    uint32_t n = be32(in), range = be32(in + 4);
    if (n > in_size - 8 || n > INT_MAX) { ret_x(c, CODEC_R_INVALID); return; }
    Decoder *d = acquire(guest, "DecodeOpusInterleavedWithHardware");
    size_t capacity = out_size / (sizeof(int16_t) * (size_t)d->channels);
    if (capacity > INT_MAX) { release(d); ret_x(c, CODEC_R_INVALID); return; }
    int got = opus_decode(d->dec, in + 8, (opus_int32)n, out, (int)capacity, 0);
    opus_uint32 final = 0;
    if (got >= 0) opus_decoder_ctl(d->dec, OPUS_GET_FINAL_RANGE(&final));
    release(d);
    if (got < 0) {
        rt_log("nn::codec: opus_decode: %s", opus_strerror(got));
        ret_x(c, CODEC_R_INVALID);
        return;
    }
    if (n && range && final != range && !plat_xchg32(&warned, 1))
        rt_log("nn::codec: Opus final range %#x != stream %#x (logged once)", (unsigned)final, (unsigned)range);
    *consumed = 8 + n;
    *samples = got;
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii", ov_work_size);
HOST_OVERRIDE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi", ov_initialize);
HOST_OVERRIDE("_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", ov_finalize);
HOST_OVERRIDE("_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE", ov_sample_rate);
HOST_OVERRIDE("_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE", ov_channel_count);
HOST_OVERRIDE("_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", ov_reset);
HOST_OVERRIDE("_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", ov_decode);

// ---- Switch 2 async HardwareOpusDecoderManager (libopus on host)
static OpusMgr *mgr_find(uint64_t guest) {
    plat_mutex_lock_shared(&lock);
    OpusMgr *m = managers;
    while (m && m->guest != guest) m = m->next;
    plat_mutex_unlock_shared(&lock);
    return m;
}

// size_t GetHardwareOpusDecoderManagerWorkBufferSize(int maxDec, int maxCh, TimeSpan maxDur, int maxPkt, int ioBufs)
static void ov_mgr_work_size(Cpu *c) {
    int max_dec = (int)arg_x(c, 0);
    int io_bufs = (int)arg_x(c, 4);
    if (max_dec <= 0) max_dec = 1;
    if (io_bufs <= 0) io_bufs = 1;
    ret_x(c, 0x10000ull + (uint64_t)max_dec * 0x4000ull + (uint64_t)io_bufs * 0x1000ull);
}

// Result InitializeHardwareOpusDecoderManager(mgr, event, maxDec, maxCh, maxDur, maxPkt, ioBufs, workBuf, workSize)
static void ov_mgr_init(Cpu *c) {
    uint64_t guest = arg_x(c, 0), ev = arg_x(c, 1);
    int max_dec = (int)arg_x(c, 2), max_ch = (int)arg_x(c, 3);
    int max_pkt = (int)arg_x(c, 5), io_bufs = (int)arg_x(c, 6);
    if (!guest) { ret_x(c, CODEC_R_INVALID); return; }
    if (ev) kern_bind_system_event(c, ev, 1);
    OpusMgr *m = calloc(1, sizeof *m);
    if (!m) rt_fatal("nn::codec: opus manager allocation");
    plat_mutex_init(&m->lock);
    m->guest = guest;
    m->event = ev;
    m->max_decoders = max_dec > 0 ? max_dec : 64;
    m->max_channels = max_ch > 0 ? max_ch : 2;
    m->max_packet_size = max_pkt > 0 ? max_pkt : 1500;
    m->io_buffer_count = io_bufs > 0 ? io_bufs : 64;
    plat_mutex_lock(&lock);
    m->next = managers;
    managers = m;
    plat_mutex_unlock(&lock);
    rt_log("nn::codec: HardwareOpusDecoderManager init max_dec=%d max_ch=%d io_bufs=%d",
           m->max_decoders, m->max_channels, m->io_buffer_count);
    ret_x(c, 0);
}

static void ov_mgr_finalize(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    plat_mutex_lock(&lock);
    OpusMgr **p = &managers;
    while (*p && (*p)->guest != guest) p = &(*p)->next;
    OpusMgr *m = *p;
    if (m) *p = m->next;
    plat_mutex_unlock(&lock);
    free(m);
}

// Result InitializeHardwareOpusDecoder(dec, rate, channels, frameDuration, mgr)
static void ov_init_managed(Cpu *c) {
    uint64_t guest = arg_x(c, 0), mgr_guest = arg_x(c, 4);
    int rate = (int)arg_x(c, 1), channels = (int)arg_x(c, 2);
    OpusMgr *m = mgr_find(mgr_guest);
    if (!guest || !m || !valid_format(rate, channels)) { ret_x(c, CODEC_R_INVALID); return; }
    OpusDecoder *fresh = opus_new(rate, channels);
    plat_mutex_lock(&lock);
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) {
        plat_mutex_lock(&d->use);
        OpusDecoder *old = d->dec;
        opus_job_free_list(d->wait_head, d->mgr);
        opus_job_free_list(d->done_head, d->mgr);
        d->wait_head = d->wait_tail = NULL;
        d->done_head = d->done_tail = NULL;
        d->wait_count = d->done_count = 0;
        d->dec = fresh;
        d->rate = rate;
        d->channels = channels;
        d->mgr = m;
        plat_mutex_unlock(&d->use);
        plat_mutex_unlock(&lock);
        opus_decoder_destroy(old);
    } else {
        d = calloc(1, sizeof *d);
        if (!d) rt_fatal("nn::codec: managed decoder allocation");
        plat_mutex_init(&d->use);
        d->guest = guest;
        d->dec = fresh;
        d->rate = rate;
        d->channels = channels;
        d->mgr = m;
        d->next = decoders;
        decoders = d;
        plat_mutex_unlock(&lock);
    }
    ret_x(c, 0);
}

// Result EnqueueOpusDataToHardwareOpusDecoder(dec, *outConsumed, data, dataSize)
static void ov_enqueue_opus(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    size_t *consumed = arg_p(c, 1);
    const uint8_t *in = arg_p(c, 2);
    size_t in_size = (size_t)arg_x(c, 3);
    if (!consumed || !in || in_size < 8) { ret_x(c, CODEC_R_INVALID); return; }
    uint32_t n = be32(in);
    const uint8_t *payload = in + 8;
    size_t used = 8 + (size_t)n;
    if (n > in_size - 8) {
        // Fallback if caller passed raw unframed Opus bytes
        n = (uint32_t)in_size;
        payload = in;
        used = in_size;
    }
    Decoder *d = acquire(guest, "EnqueueOpusDataToHardwareOpusDecoder");
    OpusJob *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("nn::codec: opus job allocation");
    if (n) {
        j->pkt = malloc(n);
        if (!j->pkt) rt_fatal("nn::codec: opus packet allocation");
        memcpy(j->pkt, payload, n);
        j->pkt_len = n;
    }
    if (d->wait_tail) d->wait_tail->next = j;
    else d->wait_head = j;
    d->wait_tail = j;
    d->wait_count++;
    if (d->mgr) {
        plat_mutex_lock(&d->mgr->lock);
        d->mgr->used_io_buffers++;
        plat_mutex_unlock(&d->mgr->lock);
    }
    release(d);
    *consumed = used;
    ret_x(c, 0);
}

// Result ExecuteHardwareOpusDecoderDecoding(mgr)
static void ov_mgr_execute(Cpu *c) {
    OpusMgr *m = mgr_find(arg_x(c, 0));
    if (!m) { ret_x(c, CODEC_R_INVALID); return; }
    plat_mutex_lock_shared(&lock);
    for (Decoder *d = decoders; d; d = d->next) {
        if (d->mgr != m) continue;
        plat_mutex_lock(&d->use);
        while (d->wait_head) {
            OpusJob *j = d->wait_head;
            d->wait_head = j->next;
            if (!d->wait_head) d->wait_tail = NULL;
            d->wait_count--;
            j->next = NULL;
            j->pcm = malloc((size_t)OPUS_MAX_FRAME_SAMPLES * (size_t)d->channels * sizeof(int16_t));
            if (!j->pcm) rt_fatal("nn::codec: opus pcm allocation");
            int got = opus_decode(d->dec, j->pkt, (opus_int32)j->pkt_len, j->pcm, OPUS_MAX_FRAME_SAMPLES, 0);
            free(j->pkt);
            j->pkt = NULL;
            j->pkt_len = 0;
            j->samples = got > 0 ? got : 0;
            if (d->done_tail) d->done_tail->next = j;
            else d->done_head = j;
            d->done_tail = j;
            d->done_count++;
        }
        plat_mutex_unlock(&d->use);
    }
    plat_mutex_unlock_shared(&lock);
    if (m->event) kern_signal_system_event(m->event);
    ret_x(c, 0);
}

// Result DequeuePcmInterleavedDataFromHardwareOpusDecoder(dec, *outSamples, pcmBuf, pcmBufSize)
static void ov_dequeue_pcm(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    int *out_samples = arg_p(c, 1);
    int16_t *pcm = arg_p(c, 2);
    size_t pcm_size = (size_t)arg_x(c, 3);
    if (!out_samples || !pcm) { ret_x(c, CODEC_R_INVALID); return; }
    Decoder *d = acquire(guest, "DequeuePcmInterleavedDataFromHardwareOpusDecoder");
    OpusJob *j = d->done_head;
    if (!j) {
        release(d);
        *out_samples = 0;
        ret_x(c, CODEC_R_INVALID);
        return;
    }
    d->done_head = j->next;
    if (!d->done_head) d->done_tail = NULL;
    d->done_count--;
    int ch = d->channels;
    OpusMgr *m = d->mgr;
    release(d);
    size_t max_samples = pcm_size / (sizeof(int16_t) * (size_t)ch);
    int copy_samples = j->samples;
    if ((size_t)copy_samples > max_samples) copy_samples = (int)max_samples;
    if (copy_samples > 0 && j->pcm)
        memcpy(pcm, j->pcm, (size_t)copy_samples * (size_t)ch * sizeof(int16_t));
    *out_samples = copy_samples;
    free(j->pcm);
    free(j);
    if (m) {
        plat_mutex_lock(&m->lock);
        if (m->used_io_buffers > 0) m->used_io_buffers--;
        plat_mutex_unlock(&m->lock);
    }
    ret_x(c, 0);
}

static void ov_mgr_free_io_count(Cpu *c) {
    OpusMgr *m = mgr_find(arg_x(c, 0));
    if (!m) { ret_x(c, 0); return; }
    plat_mutex_lock(&m->lock);
    int free_n = m->io_buffer_count - m->used_io_buffers;
    plat_mutex_unlock(&m->lock);
    ret_x(c, free_n > 0 ? (uint64_t)free_n : 0);
}

static void ov_wait_count(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderWaitingDecodeRequestCount");
    int n = d->wait_count;
    release(d);
    ret_x(c, (uint64_t)n);
}

static void ov_exec_count(Cpu *c) {
    (void)c;
    ret_x(c, 0);
}

static void ov_done_count(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderCompletedDecodeRequestCount");
    int n = d->done_count;
    release(d);
    ret_x(c, (uint64_t)n);
}

HOST_OVERRIDE("_ZN2nn5codec43GetHardwareOpusDecoderManagerWorkBufferSizeEiiNS_8TimeSpanEii", ov_mgr_work_size);
HOST_OVERRIDE("_ZN2nn5codec36InitializeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeEPNS_2os15SystemEventTypeEiiNS_8TimeSpanEiiPvm", ov_mgr_init);
HOST_OVERRIDE("_ZN2nn5codec34FinalizeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_finalize);
HOST_OVERRIDE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiNS_8TimeSpanEPNS0_30HardwareOpusDecoderManagerTypeE", ov_init_managed);
HOST_OVERRIDE("_ZN2nn5codec36EnqueueOpusDataToHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPmPKvm", ov_enqueue_opus);
HOST_OVERRIDE("_ZN2nn5codec34ExecuteHardwareOpusDecoderDecodingEPNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_execute);
HOST_OVERRIDE("_ZN2nn5codec48DequeuePcmInterleavedDataFromHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPiPsm", ov_dequeue_pcm);
HOST_OVERRIDE("_ZN2nn5codec46GetHardwareOpusDecoderManagerFreeIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_free_io_count);
HOST_OVERRIDE("_ZN2nn5codec47GetHardwareOpusDecoderWaitingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", ov_wait_count);
HOST_OVERRIDE("_ZN2nn5codec49GetHardwareOpusDecoderExecutingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", ov_exec_count);
HOST_OVERRIDE("_ZN2nn5codec49GetHardwareOpusDecoderCompletedDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", ov_done_count);
