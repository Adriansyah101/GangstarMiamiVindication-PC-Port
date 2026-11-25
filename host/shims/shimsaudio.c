#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"

typedef int (*au_render_cb_t)(void *inRefCon, uint32_t *ioActionFlags, const void *inTimeStamp,
                              uint32_t inBusNumber, uint32_t inNumberFrames, void *ioData);

struct au_buffer     { uint32_t mNumberChannels; uint32_t mDataByteSize; void *mData; };
struct au_bufferlist { uint32_t mNumberBuffers;  struct au_buffer mBuffers[1]; };
struct au_timestamp  { unsigned char raw[64]; };

#define AU_RENDER_OUTPUT_IS_SILENCE 0x10
#define AU_TS_SAMPLETIME_VALID      0x01

#define NB_MAX 32
#define FRAMES_DEFAULT 256                  /* 0x5c2dc0 */

static int      g_snd_ok;
static SDL_AtomicInt g_started;
static SDL_AtomicInt g_done;
static SDL_Thread *g_thread;
static SDL_Semaphore *g_wake;
static volatile int g_stop;
static double   g_sample_time;
static volatile int g_underruns;

static SDL_AudioFormat snd_format(uint32_t bits)
{
    return bits == 8 ? SDL_AUDIO_U8 : bits == 32 ? SDL_AUDIO_S32LE : SDL_AUDIO_S16LE;
}

void gmv_snd_init(void)
{
    SDL_SetAppMetadata(GMV_GAME_TITLE, NULL, NULL);
    g_snd_ok = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (g_snd_ok) {
        int v = SDL_GetVersion();
        gmv_log("[audio] SDL %d.%d.%d, driver %s\n", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
                SDL_VERSIONNUM_MICRO(v), SDL_GetCurrentAudioDriver());
    } else {
        gmv_log("[audio] SDL audio failed (%s); no sound\n", SDL_GetError());
    }
}

void *gmv_snd_open(uint32_t rate, uint32_t chans, uint32_t bits)
{
    SDL_AudioSpec spec = { snd_format(bits), (int)chans, (int)rate };
    SDL_AudioStream *st = g_snd_ok ? SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL) : NULL;
    if (st)
        SDL_ResumeAudioStreamDevice(st);
    return st;
}

void gmv_snd_write(void *st, const void *pcm, size_t len)
{
    SDL_PutAudioStreamData(st, pcm, (int)len);
}

int gmv_snd_queued(void *st)
{
    return SDL_GetAudioStreamQueued(st);
}

void gmv_snd_close(void *st)
{
    SDL_DestroyAudioStream(st);
}

#if GMV_DEV
static FILE    *g_dump;
static uint32_t g_dump_bytes;
static uint16_t g_dump_ch, g_dump_bits;
static uint32_t g_dump_rate;

static void dump_wav_open(uint32_t rate, uint16_t ch, uint16_t bits)
{
    const char *e = gmv_dbgenv("GMV_AUDIO_DUMP");
    if (!(gmv_env_on(e)))
        return;
    char def[1024];
    snprintf(def, sizeof def, "%sgmv_audio_dump.wav", SDL_GetBasePath());
    const char *path = (strcmp(e, "1") == 0) ? def : e;
    g_dump = fopen(path, "wb");
    if (!g_dump) { gmv_log("[audio] dump open failed: %s\n", path); return; }
    g_dump_ch = ch; g_dump_bits = bits; g_dump_rate = rate; g_dump_bytes = 0;
    unsigned char h[44];
    memset(h, 0, sizeof h);
    memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t sixteen = 16; uint16_t pcm = 1;
    uint32_t byterate = rate * ch * (bits / 8); uint16_t align = (uint16_t)(ch * (bits / 8));
    memcpy(h + 16, &sixteen, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);    memcpy(h + 28, &byterate, 4);
    memcpy(h + 32, &align, 2);   memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4);
    fwrite(h, 1, 44, g_dump);
    gmv_log("[audio] dumping mixed PCM to %s (%u Hz %u ch %u-bit)\n", path, rate, ch, bits);
}

static void dump_wav_sizes(void)
{
    uint32_t riff = 36 + g_dump_bytes, data = g_dump_bytes;
    fseek(g_dump, 4, SEEK_SET);  fwrite(&riff, 4, 1, g_dump);
    fseek(g_dump, 40, SEEK_SET); fwrite(&data, 4, 1, g_dump);
    fseek(g_dump, 0, SEEK_END);
    fflush(g_dump);
}

static void dump_wav_close(void)
{
    if (!g_dump) return;
    uint32_t data = g_dump_bytes;
    dump_wav_sizes();
    fclose(g_dump); g_dump = NULL;
    gmv_log("[audio] dump closed: %u bytes PCM\n", data);
}
#else
#define dump_wav_open(rate, ch, bits) ((void)0)
#define dump_wav_close() ((void)0)
#endif

/* force_align_arg_pointer esp SSE */
__attribute__((noinline, force_align_arg_pointer))
static int call_render(au_render_cb_t cb, void *refcon, uint32_t *flags,
                       const void *ts, uint32_t frames, void *abl)
{
    /* 0x5ea3a0 vtable[0x4c] */
    static int trace;
    if (trace == 0) {
        const char *e = gmv_dbgenv("GMV_CARSND_TRACE");
        trace = (gmv_env_on(e)) ? 1 : -1;
        if (trace == 1) {
            void **vtbl = refcon ? *(void ***)refcon : NULL;
            void *mix4c = vtbl ? vtbl[0x4c / 4] : NULL;
            gmv_log("[carsnd] audio: render_cb=%p refcon=%p vtbl=%p vtbl[0x4c]=%p (per-group mix)\n",
                    (void *)(uintptr_t)cb, refcon, (void *)vtbl, mix4c);
        }
    }
    return cb(refcon, flags, ts, 0, frames, abl);
}

/* 0x5ea9f7 -> 0x5bf320 DAT_0064d5f8 */
uint32_t gmv_audio_frames(void)
{
    uint32_t frames = FRAMES_DEFAULT;
    const char *e = gmv_getenv("GMV_AUDIO_FRAMES");
    if (e && *e) { long v = atol(e); if (v >= 32 && v <= 4096) frames = (uint32_t)v; }
    return frames;
}

static void SDLCALL stream_pulled(void *user, SDL_AudioStream *st, int additional, int total)
{
    (void)user; (void)st; (void)additional; (void)total;
    SDL_SignalSemaphore(g_wake);
}

static int SDLCALL audio_thread(void *param)
{
    (void)param;
    au_render_cb_t cb = (au_render_cb_t)(uintptr_t)gmv_au_render_cb;
    void *refcon      = gmv_au_render_refcon;
    uint32_t chans    = gmv_au_channels ? gmv_au_channels : 2;
    uint32_t bits     = gmv_au_bits ? gmv_au_bits : 16;
    uint32_t rate     = (uint32_t)(gmv_au_sample_rate > 0 ? gmv_au_sample_rate : 44100.0);
    uint32_t frame_sz = chans * (bits / 8);

    uint32_t frames = gmv_audio_frames();
    int nb = 4;
    { const char *e = gmv_getenv("GMV_AUDIO_NB");     if (e && *e) { long v = atol(e); if (v >= 2  && v <= NB_MAX) nb = (int)v; }
      else { nb = (int)(4096 / frames); if (nb < 4) nb = 4; if (nb > NB_MAX) nb = NB_MAX; } }

    SDL_AudioSpec spec = { snd_format(bits), (int)chans, (int)rate };
    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) {
        gmv_log("[audio] no audio stream (%s); no sound\n", SDL_GetError());
        SDL_SetAtomicInt(&g_done, 1);
        return 0;
    }
    gmv_log("[audio] stream open: %u Hz, %u ch, %u-bit; %d x %u-frame slices (~%.1f ms/slice)\n",
            rate, chans, bits, nb, frames, 1000.0 * frames / rate);
    dump_wav_open(rate, (uint16_t)chans, (uint16_t)bits);
    SDL_SetAudioStreamGetCallback(stream, stream_pulled, NULL);

    const uint32_t bufbytes = frames * frame_sz;
    const int target = nb * (int)bufbytes;
    unsigned char *buf = (unsigned char *)calloc(1, bufbytes);
    int playing = 0;
    while (!g_stop) {
        int queued = SDL_GetAudioStreamQueued(stream);
        if (queued >= target) {
            if (!playing) {
                SDL_ResumeAudioStreamDevice(stream);
                playing = 1;
            }
            SDL_WaitSemaphoreTimeout(g_wake, 100);
            continue;
        }
        if (playing && queued <= 0)
            g_underruns++;

        struct au_bufferlist abl;
        abl.mNumberBuffers          = 1;
        abl.mBuffers[0].mNumberChannels = chans;
        abl.mBuffers[0].mDataByteSize   = bufbytes;
        abl.mBuffers[0].mData           = buf;

        struct au_timestamp ts;
        memset(&ts, 0, sizeof ts);
        memcpy(ts.raw, &g_sample_time, 8);
        *(uint32_t *)(ts.raw + 56) = AU_TS_SAMPLETIME_VALID;

        uint32_t flags = 0;
        memset(buf, 0, bufbytes);
        int rc = cb ? call_render(cb, refcon, &flags, &ts, frames, &abl) : 0;
        if (rc != 0 || (flags & AU_RENDER_OUTPUT_IS_SILENCE))
            memset(buf, 0, bufbytes);
        if (bits == 16)
            gmv_game_audio_mix((int16_t *)buf, frames, chans, rate);

#if GMV_DEV
        {
            const int16_t *s = (const int16_t *)buf;
            int peak = 0, lrdiff = 0;
            for (uint32_t k = 0; k < bufbytes / 2; k++) {
                int v = s[k] < 0 ? -s[k] : s[k];
                if (v > peak) peak = v;
            }
            if (chans == 2) {
                for (uint32_t k = 0; k + 1 < bufbytes / 2; k += 2) {
                    int d = s[k] - s[k+1];
                    if (d < 0) d = -d;
                    if (d > lrdiff) lrdiff = d;
                }
            }
            static int diagn, wpeak, wcount, wlr, wlow = -1;
            if (diagn < 5) {
                diagn++;
                gmv_trace("[audio] render #%d rc=%d flags=0x%x peak=%d/32767 maxLR=%d\n",
                        diagn, rc, flags, peak, lrdiff);
            }
            if (peak > wpeak) wpeak = peak;
            if (lrdiff > wlr) wlr = lrdiff;
            if (playing && (wlow < 0 || queued < wlow)) wlow = queued;
            if (++wcount >= 256) {
                gmv_trace("[audio] level: peak=%d/32767 maxLR=%d over %d slices; underruns=%d; lowest queue %d frames\n",
                        wpeak, wlr, wcount, g_underruns, wlow < 0 ? -1 : wlow / (int)frame_sz);
                wpeak = 0; wcount = 0; wlr = 0; wlow = -1;
            }
        }

        if (g_dump) {
            uint32_t sec = g_dump_rate * g_dump_ch * (g_dump_bits / 8);
            fwrite(buf, 1, bufbytes, g_dump);
            g_dump_bytes += bufbytes;
            if (sec && g_dump_bytes / sec != (g_dump_bytes - bufbytes) / sec)
                dump_wav_sizes();
        }
#endif

        g_sample_time += frames;
        SDL_PutAudioStreamData(stream, buf, (int)bufbytes);
    }

    SDL_DestroyAudioStream(stream);
    free(buf);
    dump_wav_close();
    gmv_log("[audio] stream closed\n");
    SDL_SetAtomicInt(&g_done, 1);
    return 0;
}

void gmv_audio_start(void)
{
    const char *e = gmv_getenv("GMV_NO_AUDIO");
    if (gmv_env_on(e))
        return;
    if (!gmv_au_render_cb) {
        gmv_log("[audio] start requested but no render callback captured; staying silent\n");
        return;
    }
    if (!g_snd_ok)
        return;
    if (SDL_SetAtomicInt(&g_started, 1))
        return;
    g_stop = 0;
    SDL_SetAtomicInt(&g_done, 0);
    g_wake = SDL_CreateSemaphore(0);
    g_thread = SDL_CreateThread(audio_thread, "audio", NULL);
    if (!g_thread) {
        gmv_log("[audio] no audio thread (%s); no sound\n", SDL_GetError());
        SDL_SetAtomicInt(&g_started, 0);
    }
}

void gmv_audio_stop(void)
{
    if (!SDL_SetAtomicInt(&g_started, 0))
        return;
    g_stop = 1;
    if (g_wake)
        SDL_SignalSemaphore(g_wake);
    if (g_thread) {
        for (int i = 0; i < 200 && !SDL_GetAtomicInt(&g_done); i++)
            SDL_Delay(10);
        if (SDL_GetAtomicInt(&g_done))
            SDL_WaitThread(g_thread, NULL);
        else
            SDL_DetachThread(g_thread);
        g_thread = NULL;
    }
    if (g_wake && SDL_GetAtomicInt(&g_done)) {
        SDL_DestroySemaphore(g_wake);
        g_wake = NULL;
    }
}
