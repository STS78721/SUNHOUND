// Horizon / Switch 2 audio output service (audout:u and aud:u cmd 1 IAudioOutManager / IAudioOut)
// backed by SDL3 audio stream playback and real-time buffer release pacing.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "audren.h"
#include "cfg.h"
#include "host.h"
#include "ipc.h"
#include "platform/platform.h"

#define AUDOUT_MAX_QUEUED 32u
#define AUDOUT_MAX_RELEASED 64u

typedef struct {
    uint64_t tag;
    uint64_t data_addr;
    uint64_t data_size;
} AudoutBuf;

typedef struct {
    PlatMutex lock;
    PlatThread *thread;
    volatile int32_t alive;
    uint32_t sample_rate, channels, format;
    volatile uint32_t state;   // 0 = Started, 1 = Stopped
    float volume;
    uint32_t event;
    uint64_t played_samples;
    AudoutBuf queued[AUDOUT_MAX_QUEUED];
    uint32_t q_head, q_count;
    uint64_t released[AUDOUT_MAX_RELEASED];
    uint32_t r_head, r_count;
    SDL_AudioStream *stream;
} AudioOut;

static IpcBuffer pick_buf(const IpcBuffer *a, unsigned na, const IpcBuffer *b, unsigned nb, unsigned i) {
    IpcBuffer x = i < na ? a[i] : (IpcBuffer){0}, y = i < nb ? b[i] : (IpcBuffer){0};
    return x.size ? x : y;
}

static void mark_auto_out(const IpcRequest *rq, IpcResponse *rs) {
    for (unsigned i = 0; i < rq->nc && rs->nxout < IPC_MAX_BUF; i++) {
        if (rq->c[i].size && (i >= rq->nb || !rq->b[i].size))
            rs->xout[rs->nxout++] = (IpcBuffer){rq->c[i].addr, rq->c[i].size, i};
    }
}

static void push_released_locked(AudioOut *o, uint64_t tag) {
    if (o->r_count >= AUDOUT_MAX_RELEASED) {
        o->r_head = (o->r_head + 1) % AUDOUT_MAX_RELEASED;
        o->r_count--;
    }
    o->released[(o->r_head + o->r_count) % AUDOUT_MAX_RELEASED] = tag;
    o->r_count++;
}

static int drain_queued_to_released_locked(AudioOut *o) {
    int moved = 0;
    while (o->q_count > 0) {
        AudoutBuf b = o->queued[o->q_head];
        o->q_head = (o->q_head + 1) % AUDOUT_MAX_QUEUED;
        o->q_count--;
        push_released_locked(o, b.tag);
        moved++;
    }
    return moved;
}

static void open_sdl_stream(AudioOut *o) {
    const char *mode = cfg_get("TOTK_AUDIO");
    if ((mode && !strcmp(mode, "none")) || cfg_get("TOTK_AUDIO_OFF")) {
        rt_log("audout: TOTK_AUDIO=none: running without audio device");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        rt_log("audout: SDL audio init failed (%s): running silently", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_S16, (int)o->channels, (int)o->sample_rate};
    o->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!o->stream) {
        rt_log("audout: no output device (%s): running silently", SDL_GetError());
        return;
    }
    SDL_SetAudioStreamGain(o->stream, o->volume);
}

static void audout_thread(void *arg) {
    AudioOut *o = arg;
    while (__atomic_load_n(&o->alive, __ATOMIC_ACQUIRE)) {
        AudoutBuf b = {0};
        int have = 0;
        uint32_t rate = 48000, ch = 2;
        SDL_AudioStream *stream = NULL;

        plat_mutex_lock(&o->lock);
        if (__atomic_load_n(&o->state, __ATOMIC_ACQUIRE) == 0 && o->q_count > 0) {
            b = o->queued[o->q_head];
            o->q_head = (o->q_head + 1) % AUDOUT_MAX_QUEUED;
            o->q_count--;
            rate = o->sample_rate;
            ch = o->channels;
            stream = o->stream;
            have = 1;
        }
        plat_mutex_unlock(&o->lock);

        if (!have) {
            plat_sleep_ms(2);
            continue;
        }

        if (stream && b.data_size > 0 && b.data_size <= (16ull << 20) &&
            plat_mem_readable((uintptr_t)b.data_addr, (size_t)b.data_size)) {
            SDL_PutAudioStreamData(stream, (const void *)(uintptr_t)b.data_addr, (int)b.data_size);
        }

        uint64_t bpf = (uint64_t)ch * 2u;
        uint64_t frames = bpf ? b.data_size / bpf : 0;
        if (stream) {
            uint64_t limit = b.data_size * 2u;
            while (__atomic_load_n(&o->alive, __ATOMIC_ACQUIRE) &&
                   __atomic_load_n(&o->state, __ATOMIC_ACQUIRE) == 0 &&
                   (uint64_t)SDL_GetAudioStreamQueued(stream) > limit) {
                plat_sleep_ms(2);
            }
        } else {
            uint64_t dur_ms = (frames && rate) ? (frames * 1000ull + rate - 1) / rate : 1;
            if (dur_ms > 200) dur_ms = 200;
            for (uint64_t elapsed = 0;
                 elapsed < dur_ms &&
                 __atomic_load_n(&o->alive, __ATOMIC_ACQUIRE) &&
                 __atomic_load_n(&o->state, __ATOMIC_ACQUIRE) == 0;
                 elapsed += 5) {
                uint64_t step = dur_ms - elapsed;
                plat_sleep_ms((uint32_t)(step > 5 ? 5 : step));
            }
        }

        plat_mutex_lock(&o->lock);
        o->played_samples += frames;
        push_released_locked(o, b.tag);
        plat_mutex_unlock(&o->lock);
        kern_event_signal(o->event);
    }
}

static void audout_close(void *arg) {
    AudioOut *o = arg;
    if (!o) return;
    plat_xchg32(&o->alive, 0);
    int away = kern_core_away();
    if (o->thread) plat_thread_join(o->thread);
    kern_core_back(&away);
    if (o->stream) SDL_DestroyAudioStream(o->stream);
    if (o->event) kern_handle_close(o->event);
    free(o);
}

static void audout_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioOut *o = s->state;
    switch (rq->cmd) {
    case 0: {   // GetAudioOutState
        uint32_t st = __atomic_load_n(&o->state, __ATOMIC_ACQUIRE);
        IPC_OUT(rs, st);
        return;
    }
    case 1:   // StartAudioOut
        plat_mutex_lock(&o->lock);
        __atomic_store_n(&o->state, 0u, __ATOMIC_RELEASE);
        if (o->stream) SDL_ResumeAudioStreamDevice(o->stream);
        plat_mutex_unlock(&o->lock);
        return;
    case 2: {   // StopAudioOut
        int moved;
        plat_mutex_lock(&o->lock);
        __atomic_store_n(&o->state, 1u, __ATOMIC_RELEASE);
        if (o->stream) {
            SDL_PauseAudioStreamDevice(o->stream);
            SDL_ClearAudioStream(o->stream);
        }
        moved = drain_queued_to_released_locked(o);
        plat_mutex_unlock(&o->lock);
        if (moved) kern_event_signal(o->event);
        return;
    }
    case 3: case 7: {   // AppendAudioOutBuffer / AppendAudioOutBufferAuto
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        IpcBuffer in = pick_buf(rq->a, rq->na, rq->x, rq->nx, 0);
        if (in.size < 40) { rs->result = 0x299u; return; }
        const uint64_t *d = ipc_guest(in.addr, 40);
        uint64_t data_addr = d[1] + d[4];
        uint64_t data_size = d[3];
        if (data_addr < d[1]) { rs->result = 0x299u; return; }
        plat_mutex_lock(&o->lock);
        if (o->q_count >= AUDOUT_MAX_QUEUED) {
            plat_mutex_unlock(&o->lock);
            rs->result = 0x299u;
            return;
        }
        o->queued[(o->q_head + o->q_count) % AUDOUT_MAX_QUEUED] = (AudoutBuf){tag, data_addr, data_size};
        o->q_count++;
        plat_mutex_unlock(&o->lock);
        return;
    }
    case 4:   // RegisterBufferEvent
        rs->copy[rs->ncopy++] = kern_event_readable(o->event);
        return;
    case 5: case 8: {   // GetReleasedAudioOutBuffers / GetReleasedAudioOutBuffersAuto
        IpcBuffer out = pick_buf(rq->b, rq->nb, rq->c, rq->nc, 0);
        uint64_t *dst = out.size >= 8 ? ipc_guest(out.addr, out.size) : NULL;
        uint32_t max_tags = dst ? (uint32_t)(out.size / 8) : 0;
        uint32_t count = 0;
        plat_mutex_lock(&o->lock);
        while (count < max_tags && o->r_count > 0) {
            dst[count++] = o->released[o->r_head];
            o->r_head = (o->r_head + 1) % AUDOUT_MAX_RELEASED;
            o->r_count--;
        }
        plat_mutex_unlock(&o->lock);
        if (rq->cmd == 8) mark_auto_out(rq, rs);
        IPC_OUT(rs, count);
        return;
    }
    case 6: {   // ContainsAudioOutBuffer
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        uint8_t found = 0;
        plat_mutex_lock(&o->lock);
        for (uint32_t i = 0; i < o->q_count; i++) {
            if (o->queued[(o->q_head + i) % AUDOUT_MAX_QUEUED].tag == tag) {
                found = 1;
                break;
            }
        }
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, found);
        return;
    }
    case 9: {   // GetAudioOutBufferCount
        plat_mutex_lock(&o->lock);
        uint32_t cnt = o->q_count;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, cnt);
        return;
    }
    case 10: {   // GetAudioOutPlayedSampleCount
        plat_mutex_lock(&o->lock);
        uint64_t played = o->played_samples;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, played);
        return;
    }
    case 11: {   // FlushAudioOutBuffers
        int moved;
        plat_mutex_lock(&o->lock);
        if (o->stream) SDL_ClearAudioStream(o->stream);
        moved = drain_queued_to_released_locked(o);
        plat_mutex_unlock(&o->lock);
        if (moved) kern_event_signal(o->event);
        uint8_t flushed = moved > 0 ? 1 : 0;
        IPC_OUT(rs, flushed);
        return;
    }
    case 12: {   // SetAudioOutVolume
        float vol = IPC_ARG(rq, float, 0);
        plat_mutex_lock(&o->lock);
        o->volume = vol;
        if (o->stream) SDL_SetAudioStreamGain(o->stream, vol);
        plat_mutex_unlock(&o->lock);
        return;
    }
    case 13: {   // GetAudioOutVolume
        plat_mutex_lock(&o->lock);
        float vol = o->volume;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, vol);
        return;
    }
    default:
        ipc_unimplemented(s, rq);
    }
}

void audout_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    static const char dev_name[] = "DeviceOut";
    switch (rq->cmd) {
    case 0: case 2: {   // ListAudioOuts / ListAudioOutsAuto
        IpcBuffer out = pick_buf(rq->b, rq->nb, rq->c, rq->nc, 0);
        if (out.size > 0) {
            uint64_t n = out.size < 0x100 ? out.size : 0x100;
            uint8_t *dst = ipc_guest(out.addr, n);
            memset(dst, 0, (size_t)n);
            size_t cp = sizeof(dev_name) < (size_t)n ? sizeof(dev_name) : (size_t)n;
            memcpy(dst, dev_name, cp);
            dst[cp - 1] = '\0';
        }
        if (rq->cmd == 2) mark_auto_out(rq, rs);
        uint32_t count = 1;
        IPC_OUT(rs, count);
        return;
    }
    case 1: case 3: {   // OpenAudioOut / OpenAudioOutAuto
        uint32_t rate = rq->data_size >= 4 ? IPC_ARG(rq, uint32_t, 0) : 0;
        uint16_t ch = rq->data_size >= 6 ? IPC_ARG(rq, uint16_t, 4) : 0;
        rate = rate ? rate : 48000;
        ch = (ch >= 1 && ch <= 6) ? ch : 2;

        IpcBuffer out = pick_buf(rq->b, rq->nb, rq->c, rq->nc, 0);
        if (out.size > 0) {
            uint64_t n = out.size < 0x100 ? out.size : 0x100;
            uint8_t *dst = ipc_guest(out.addr, n);
            memset(dst, 0, (size_t)n);
            size_t cp = sizeof(dev_name) < (size_t)n ? sizeof(dev_name) : (size_t)n;
            memcpy(dst, dev_name, cp);
            dst[cp - 1] = '\0';
        }
        if (rq->cmd == 3) mark_auto_out(rq, rs);

        AudioOut *o = calloc(1, sizeof *o);
        if (!o) rt_fatal("audout: out of memory");
        o->lock = (PlatMutex)PLAT_MUTEX_INIT;
        o->alive = 1;
        o->sample_rate = rate;
        o->channels = ch;
        o->format = 2;
        o->state = 1;
        o->volume = 1.0f;
        o->event = kern_event_new();
        open_sdl_stream(o);
        o->thread = plat_thread_new(audout_thread, o, "audout");
        if (!o->thread) rt_fatal("audout: cannot start playback thread");

        rt_log("audout: opened 'DeviceOut' %u Hz %u ch PCM16", o->sample_rate, o->channels);
        struct { uint32_t rate, ch, fmt, state; } out_p = {o->sample_rate, o->channels, 2u, 1u};
        IPC_OUT(rs, out_p);
        rs->move[rs->nmove++] = ipc_session_new("IAudioOut", audout_handler, o, audout_close);
        return;
    }
    default:
        ipc_unimplemented(s, rq);
    }
}

// ---- Switch 2 nn::audio OutputStream / InputStream / VirtualDevice host overrides (backed by SDL3)
// OutputStreamState: 0 = Closed, 1 = Stopped, 2 = Started
enum { OSTREAM_CLOSED = 0, OSTREAM_STOPPED = 1, OSTREAM_STARTED = 2 };

typedef struct {
    int32_t sample_format;   // 1=S8, 2=S16, 3=S24, 4=S32, 5=F32
    int32_t channel_count;
    int32_t sample_rate;
} NnStreamFormat;

typedef struct OutStream {
    uint64_t guest;
    uint64_t event;          // optional nn::os::SystemEvent (SystemEventType at +0)
    int sample_format, channels, sample_rate, bytes_per_frame;
    int cap_frames, threshold_frames;
    volatile int32_t state;
    volatile int32_t alive;
    float gain;
    PlatMutex lock;
    PlatThread *thread;
    SDL_AudioStream *sdl;
    int64_t sim_queued_frames;
    uint64_t last_ns;
    struct OutStream *next;
} OutStream;

static OutStream *g_ostreams;
static PlatMutex g_ostream_lock = PLAT_MUTEX_INIT;

static OutStream *ostream_find(uint64_t guest) {
    plat_mutex_lock_shared(&g_ostream_lock);
    OutStream *s = g_ostreams;
    while (s && s->guest != guest) s = s->next;
    plat_mutex_unlock_shared(&g_ostream_lock);
    return s;
}

static int ostream_filled_locked(OutStream *s) {
    if (s->sdl) {
        int qbytes = SDL_GetAudioStreamQueued(s->sdl);
        if (qbytes < 0) qbytes = 0;
        int frames = s->bytes_per_frame ? qbytes / s->bytes_per_frame : 0;
        return frames > s->cap_frames ? s->cap_frames : frames;
    }
    uint64_t now = plat_now_ns();
    if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == OSTREAM_STARTED && s->last_ns && now > s->last_ns) {
        uint64_t dt_ns = now - s->last_ns;
        int64_t drained = (int64_t)((dt_ns * (uint64_t)s->sample_rate) / 1000000000ull);
        if (drained > 0) {
            s->sim_queued_frames -= drained;
            if (s->sim_queued_frames < 0) s->sim_queued_frames = 0;
            s->last_ns = now;
        }
    } else {
        s->last_ns = now;
    }
    return (int)s->sim_queued_frames;
}

static void ostream_pacer(void *arg) {
    OutStream *s = arg;
    while (__atomic_load_n(&s->alive, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == OSTREAM_STARTED && s->event) {
            plat_mutex_lock(&s->lock);
            int filled = ostream_filled_locked(s);
            int free_f = s->cap_frames - filled;
            int thresh = s->threshold_frames;
            plat_mutex_unlock(&s->lock);
            if (free_f >= thresh) kern_signal_system_event(s->event);
        }
        plat_sleep_ms(5);
    }
}

static uint32_t ostream_open_common(Cpu *c, uint64_t guest, uint64_t ev, const NnStreamFormat *fmt,
                                    int cap_frames, int thresh_frames) {
    if (!guest || !fmt || fmt->sample_rate <= 0 || fmt->channel_count <= 0 || fmt->channel_count > 8)
        return 0x299u;
    SDL_AudioFormat sdl_fmt = SDL_AUDIO_S16;
    int bps = 2;
    switch (fmt->sample_format) {
    case 1: sdl_fmt = SDL_AUDIO_S8; bps = 1; break;
    case 2: sdl_fmt = SDL_AUDIO_S16; bps = 2; break;
    case 4: sdl_fmt = SDL_AUDIO_S32; bps = 4; break;
    case 5: sdl_fmt = SDL_AUDIO_F32; bps = 4; break;
    default: sdl_fmt = SDL_AUDIO_S16; bps = 2; break;
    }
    if (ev) kern_bind_system_event(c, ev, 1);

    OutStream *s = calloc(1, sizeof *s);
    if (!s) rt_fatal("audout: OutStream allocation");
    plat_mutex_init(&s->lock);
    s->guest = guest;
    s->event = ev;
    s->sample_format = fmt->sample_format;
    s->channels = fmt->channel_count;
    s->sample_rate = fmt->sample_rate;
    s->bytes_per_frame = bps * s->channels;
    s->cap_frames = cap_frames > 0 ? cap_frames : 4096;
    s->threshold_frames = (thresh_frames > 0 && thresh_frames <= s->cap_frames) ? thresh_frames : s->cap_frames / 4;
    s->state = OSTREAM_STOPPED;
    s->alive = 1;
    s->gain = 1.0f;
    s->last_ns = plat_now_ns();

    const char *mode = cfg_get("TOTK_AUDIO");
    if (!(mode && !strcmp(mode, "none")) && !cfg_get("TOTK_AUDIO_OFF")) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_AudioSpec spec = {sdl_fmt, s->channels, s->sample_rate};
            s->sdl = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            if (s->sdl) {
                SDL_SetAudioStreamGain(s->sdl, s->gain);
                SDL_PauseAudioStreamDevice(s->sdl);
            }
        }
    }
    s->thread = plat_thread_new(ostream_pacer, s, "audstream");
    plat_mutex_lock(&g_ostream_lock);
    s->next = g_ostreams;
    g_ostreams = s;
    plat_mutex_unlock(&g_ostream_lock);
    rt_log("audstream: opened %d Hz %d ch fmt=%d cap=%d thresh=%d sdl=%d",
           s->sample_rate, s->channels, s->sample_format, s->cap_frames, s->threshold_frames, s->sdl != NULL);
    return 0;
}

static void ov_audio_open_ostream_ev(Cpu *c) {
    uint64_t guest = arg_x(c, 0), ev = arg_x(c, 1);
    const NnStreamFormat *fmt = arg_p(c, 5);
    int cap = (int)arg_x(c, 6), thresh = (int)arg_x(c, 7);
    ret_x(c, ostream_open_common(c, guest, ev, fmt, cap, thresh));
}
static void ov_audio_open_ostream_noev(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    const NnStreamFormat *fmt = arg_p(c, 4);
    int cap = (int)arg_x(c, 5);
    ret_x(c, ostream_open_common(c, guest, 0, fmt, cap, cap / 4));
}
static void ostream_start_one(uint64_t guest) {
    OutStream *s = ostream_find(guest);
    if (!s) return;
    plat_mutex_lock(&s->lock);
    s->last_ns = plat_now_ns();
    __atomic_store_n(&s->state, OSTREAM_STARTED, __ATOMIC_RELEASE);
    if (s->sdl) SDL_ResumeAudioStreamDevice(s->sdl);
    plat_mutex_unlock(&s->lock);
    if (s->event) kern_signal_system_event(s->event);
}
static void ov_audio_start_ostream(Cpu *c) {
    ostream_start_one(arg_x(c, 0));
    ret_x(c, 0);
}
static void ov_audio_start_ostreams(Cpu *c) {
    const uint64_t *arr = arg_p(c, 0);
    int count = (int)arg_x(c, 1);
    for (int i = 0; arr && i < count; i++) ostream_start_one(arr[i]);
    ret_x(c, 0);
}
static void ov_audio_stop_ostream(Cpu *c) {
    OutStream *s = ostream_find(arg_x(c, 0));
    if (s) {
        plat_mutex_lock(&s->lock);
        __atomic_store_n(&s->state, OSTREAM_STOPPED, __ATOMIC_RELEASE);
        if (s->sdl) SDL_PauseAudioStreamDevice(s->sdl);
        plat_mutex_unlock(&s->lock);
    }
    ret_x(c, 0);
}
static void ov_audio_purge_ostream(Cpu *c) {
    OutStream *s = ostream_find(arg_x(c, 0));
    if (s) {
        plat_mutex_lock(&s->lock);
        if (s->sdl) SDL_ClearAudioStream(s->sdl);
        s->sim_queued_frames = 0;
        plat_mutex_unlock(&s->lock);
    }
    ret_x(c, 0);
}
static void ov_audio_close_ostream(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    plat_mutex_lock(&g_ostream_lock);
    OutStream **p = &g_ostreams;
    while (*p && (*p)->guest != guest) p = &(*p)->next;
    OutStream *s = *p;
    if (s) *p = s->next;
    plat_mutex_unlock(&g_ostream_lock);
    if (!s) return;
    plat_xchg32(&s->alive, 0);
    int away = kern_core_away();
    if (s->thread) plat_thread_join(s->thread);
    kern_core_back(&away);
    if (s->sdl) SDL_DestroyAudioStream(s->sdl);
    free(s);
}
static void ov_audio_write_ostream(Cpu *c) {
    OutStream *s = ostream_find(arg_x(c, 0));
    const void *data = arg_p(c, 1);
    int frames = (int)arg_x(c, 2);
    if (!s || !data || frames <= 0) { ret_x(c, 0); return; }
    plat_mutex_lock(&s->lock);
    int filled = ostream_filled_locked(s);
    int free_f = s->cap_frames - filled;
    int take = frames < free_f ? frames : free_f;
    if (take > 0) {
        if (s->sdl) SDL_PutAudioStreamData(s->sdl, data, take * s->bytes_per_frame);
        else s->sim_queued_frames += take;
    }
    plat_mutex_unlock(&s->lock);
    ret_x(c, (uint64_t)(take > 0 ? take : 0));
}
static void ov_audio_ostream_free(Cpu *c) {
    OutStream *s = ostream_find(arg_x(c, 0));
    if (!s) { ret_x(c, 0); return; }
    plat_mutex_lock(&s->lock);
    int free_f = s->cap_frames - ostream_filled_locked(s);
    plat_mutex_unlock(&s->lock);
    ret_x(c, (uint64_t)(free_f > 0 ? free_f : 0));
}
static void ov_audio_ostream_filled(Cpu *c) {
    OutStream *s = ostream_find(arg_x(c, 0));
    if (!s) { ret_x(c, 0); return; }
    plat_mutex_lock(&s->lock);
    int filled = ostream_filled_locked(s);
    plat_mutex_unlock(&s->lock);
    ret_x(c, (uint64_t)(filled > 0 ? filled : 0));
}
static void ov_audio_ostream_state(Cpu *c) {
    OutStream *s = ostream_find(arg_x(c, 0));
    ret_x(c, s ? (uint64_t)__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) : OSTREAM_CLOSED);
}
static void ov_audio_ostream_gain(Cpu *c) {
    OutStream *s = ostream_find(arg_x(c, 0));
    float g = 1.0f;
    memcpy(&g, &c->v[0], sizeof g);
    if (s) {
        plat_mutex_lock(&s->lock);
        s->gain = g;
        if (s->sdl) SDL_SetAudioStreamGain(s->sdl, g);
        plat_mutex_unlock(&s->lock);
    }
}
static void ov_audio_ostream_stats(Cpu *c) {
    void *out = (void *)(uintptr_t)c->x[8];
    if (out) memset(out, 0, 24);
}
static void ov_audio_default_dev_name(Cpu *c) {
    char *dst = arg_p(c, 0);
    if (dst) {
        memset(dst, 0, 256);
        strcpy(dst, "DefaultOutput");
    }
    ret_x(c, 0);
}
static void ov_audio_active_channels(Cpu *c) { ret_x(c, 2); }
static void ov_audio_notify_output(Cpu *c) {
    uint64_t ev = arg_x(c, 0);
    if (ev) kern_bind_system_event(c, ev, 0);
    ret_x(c, 0);
}
static void ov_audio_noop(Cpu *c) { (void)c; }
static void ov_audio_zero(Cpu *c) { ret_x(c, 0); }
static void ov_audio_input_fail(Cpu *c) { ret_x(c, 0x299u); }
static void ov_audio_vinput_props(Cpu *c) {
    void *out = arg_p(c, 0);
    if (out) memset(out, 0, 64);
    ret_x(c, 0x299u);
}

HOST_OVERRIDE("_ZN2nn5audio29GetDefaultVirtualOutputDeviceEPNS0_15AudioDeviceNameE", ov_audio_default_dev_name);
HOST_OVERRIDE("_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", ov_audio_default_dev_name);
HOST_OVERRIDE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", ov_audio_default_dev_name);
HOST_OVERRIDE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", ov_audio_active_channels);
HOST_OVERRIDE("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", ov_audio_notify_output);
HOST_OVERRIDE("_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb", ov_audio_noop);
HOST_OVERRIDE("_ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi", ov_audio_zero);
HOST_OVERRIDE("_ZN2nn5audio31GetVirtualInputDevicePropertiesEPNS0_23VirtualDevicePropertiesERKNS0_15AudioDeviceNameE", ov_audio_vinput_props);
HOST_OVERRIDE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE", ov_audio_open_ostream_ev);
HOST_OVERRIDE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEii", ov_audio_open_ostream_ev);
HOST_OVERRIDE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE", ov_audio_open_ostream_noev);
HOST_OVERRIDE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi", ov_audio_open_ostream_noev);
HOST_OVERRIDE("_ZN2nn5audio17StartOutputStreamEPNS0_16OutputStreamTypeE", ov_audio_start_ostream);
HOST_OVERRIDE("_ZN2nn5audio18StartOutputStreamsEPKPNS0_16OutputStreamTypeEi", ov_audio_start_ostreams);
HOST_OVERRIDE("_ZN2nn5audio16StopOutputStreamEPNS0_16OutputStreamTypeE", ov_audio_stop_ostream);
HOST_OVERRIDE("_ZN2nn5audio17PurgeOutputStreamEPNS0_16OutputStreamTypeE", ov_audio_purge_ostream);
HOST_OVERRIDE("_ZN2nn5audio17CloseOutputStreamEPNS0_16OutputStreamTypeE", ov_audio_close_ostream);
HOST_OVERRIDE("_ZN2nn5audio17WriteOutputStreamEPNS0_16OutputStreamTypeEPKvi", ov_audio_write_ostream);
HOST_OVERRIDE("_ZN2nn5audio29GetOutputStreamFreeFrameCountEPKNS0_16OutputStreamTypeE", ov_audio_ostream_free);
HOST_OVERRIDE("_ZN2nn5audio31GetOutputStreamFilledFrameCountEPKNS0_16OutputStreamTypeE", ov_audio_ostream_filled);
HOST_OVERRIDE("_ZN2nn5audio20GetOutputStreamStateEPKNS0_16OutputStreamTypeE", ov_audio_ostream_state);
HOST_OVERRIDE("_ZN2nn5audio19SetOutputStreamGainEPNS0_16OutputStreamTypeEfNS_8TimeSpanE", ov_audio_ostream_gain);
HOST_OVERRIDE("_ZN2nn5audio25GetOutputStreamStatisticsEPNS0_16OutputStreamTypeEb", ov_audio_ostream_stats);
HOST_OVERRIDE("_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi", ov_audio_input_fail);
HOST_OVERRIDE("_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE", ov_audio_noop);
HOST_OVERRIDE("_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE", ov_audio_input_fail);
HOST_OVERRIDE("_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE", ov_audio_zero);
HOST_OVERRIDE("_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi", ov_audio_zero);
HOST_OVERRIDE("_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE", ov_audio_zero);
HOST_OVERRIDE("_ZN2nn5audio20GetInputStreamFormatEPKNS0_15InputStreamTypeE", ov_audio_ostream_stats);
