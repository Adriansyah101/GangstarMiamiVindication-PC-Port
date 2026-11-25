#ifdef _WIN32
#define COBJMACROS
#endif
#include "compat.h"
#ifdef _WIN32
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#else
#include <dirent.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include "platform.h"
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "log.h"
#include "shims.h"
#include "gangstar.h"

#define CR_MM_PP        0x0063e030u
#define CR_AUDIO_DEVICE 0x0064d5e0u   /* +0x178 0x14 */
#define CR_MUSIC_GROUP  1

#define CR_RING   (1u << 17)
#define CR_MAXSONGS 4096
#define CR_GAP_MS 600

#ifdef _WIN32
typedef wchar_t cr_ch;
#define CR_DIRMAX MAX_PATH
#define cr_strdup _wcsdup
static const wchar_t *const cr_exts[] = { L".mp3", L".ogg", L".m4a", L".aac", L".wma", L".wav", L".flac" };
#else
typedef char cr_ch;
#define CR_DIRMAX 4096
#define cr_strdup strdup
static const char *const cr_exts[] = { ".mp3", ".ogg", ".m4a", ".aac", ".wma", ".wav", ".flac" };
#endif

static CRITICAL_SECTION cr_lock;
static cr_ch    cr_dir[CR_DIRMAX];
static cr_ch   *cr_songs[CR_MAXSONGS];
static int      cr_nsongs;
static int      cr_order[CR_MAXSONGS], cr_next;
static int      cr_last = -1;
static uint32_t cr_rng;

static volatile LONG cr_on;
static volatile LONG cr_inited, cr_thread_up;
static volatile LONG cr_skip;
static volatile LONG cr_rate;
static volatile float cr_user_vol = 1.0f;

static int16_t  cr_ring[CR_RING * 2];
static volatile LONG cr_wr, cr_rd;

static float    cr_gain;
static int      cr_heard;

static uint32_t cr_rand(void)
{
    cr_rng ^= cr_rng << 13; cr_rng ^= cr_rng >> 17; cr_rng ^= cr_rng << 5;
    return cr_rng;
}

#ifdef _WIN32
static void cr_init(void)
{
    if (InterlockedExchange(&cr_inited, 1))
        return;
    InitializeCriticalSection(&cr_lock);
    cr_rng = GetTickCount() | 1;
    DWORD n = GetModuleFileNameW(NULL, cr_dir, MAX_PATH);
    wchar_t *slash = n ? wcsrchr(cr_dir, L'\\') : NULL;
    if (slash) slash[1] = 0; else cr_dir[0] = 0;
    wcsncat(cr_dir, L"custom radio", MAX_PATH - wcslen(cr_dir) - 1);
}

static int cr_is_song(const wchar_t *name)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    if (!dot)
        return 0;
    for (size_t i = 0; i < sizeof cr_exts / sizeof *cr_exts; i++)
        if (!_wcsicmp(dot, cr_exts[i]))
            return 1;
    return 0;
}

static void cr_utf8(const wchar_t *w, char *out, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL))
        snprintf(out, (size_t)cap, "?");
}
#else
static void cr_init(void)
{
    if (InterlockedExchange(&cr_inited, 1))
        return;
    InitializeCriticalSection(&cr_lock);
    cr_rng = GetTickCount() | 1;
    gmv_exe_dir(cr_dir, sizeof cr_dir - 16);
    strcat(cr_dir, "/custom radio");
}

static int cr_is_song(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return 0;
    for (size_t i = 0; i < sizeof cr_exts / sizeof *cr_exts; i++)
        if (!strcasecmp(dot, cr_exts[i]))
            return 1;
    return 0;
}

static void cr_utf8(const char *s, char *out, int cap)
{
    snprintf(out, (size_t)cap, "%s", s);
}
#endif

static void cr_shuffle(void)
{
    for (int i = 0; i < cr_nsongs; i++)
        cr_order[i] = i;
    for (int i = cr_nsongs - 1; i > 0; i--) {
        int j = (int)(cr_rand() % (uint32_t)(i + 1)), t = cr_order[i];
        cr_order[i] = cr_order[j]; cr_order[j] = t;
    }
    if (cr_nsongs > 1 && cr_order[0] == cr_last) {
        int t = cr_order[0]; cr_order[0] = cr_order[1]; cr_order[1] = t;
    }
    cr_next = 0;
}

#ifndef _WIN32
static int cr_have_ffmpeg(void);

static int cr_scan(void)
{
    cr_init();
    char *found[CR_MAXSONGS];
    int n = 0;
    DIR *d = opendir(cr_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < CR_MAXSONGS) {
            if (!cr_is_song(e->d_name) || (!gmv_song_builtin(e->d_name) && !cr_have_ffmpeg()))
                continue;
            size_t len = strlen(cr_dir) + 1 + strlen(e->d_name) + 1;
            char *p = malloc(len);
            if (!p)
                break;
            snprintf(p, len, "%s/%s", cr_dir, e->d_name);
            struct stat st;
            if (stat(p, &st) || !S_ISREG(st.st_mode)) {
                free(p);
                continue;
            }
            found[n++] = p;
        }
        closedir(d);
    }
    EnterCriticalSection(&cr_lock);
    for (int i = 0; i < cr_nsongs; i++)
        free(cr_songs[i]);
    memcpy(cr_songs, found, (size_t)n * sizeof *found);
    cr_nsongs = n;
    cr_last = -1;
    cr_shuffle();
    LeaveCriticalSection(&cr_lock);
    return n;
}
#else
static int cr_scan(void)
{
    cr_init();
    wchar_t pat[MAX_PATH + 4];
    _snwprintf(pat, MAX_PATH + 3, L"%ls\\*", cr_dir);
    pat[MAX_PATH + 3] = 0;
    wchar_t *found[CR_MAXSONGS];
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !cr_is_song(fd.cFileName) || n >= CR_MAXSONGS)
                continue;
            size_t len = wcslen(cr_dir) + 1 + wcslen(fd.cFileName) + 1;
            wchar_t *p = malloc(len * sizeof *p);
            if (!p)
                break;
            _snwprintf(p, len, L"%ls\\%ls", cr_dir, fd.cFileName);
            p[len - 1] = 0;
            found[n++] = p;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    EnterCriticalSection(&cr_lock);
    for (int i = 0; i < cr_nsongs; i++)
        free(cr_songs[i]);
    memcpy(cr_songs, found, (size_t)n * sizeof *found);
    cr_nsongs = n;
    cr_last = -1;
    cr_shuffle();
    LeaveCriticalSection(&cr_lock);
    return n;
}
#endif

static cr_ch *cr_take_next(void)
{
    if (cr_next >= cr_nsongs)
        cr_scan();
    EnterCriticalSection(&cr_lock);
    cr_ch *p = NULL;
    if (cr_next < cr_nsongs) {
        cr_last = cr_order[cr_next++];
        p = cr_strdup(cr_songs[cr_last]);
    }
    LeaveCriticalSection(&cr_lock);
    return p;
}

static uint32_t cr_fill(void)
{
    return (uint32_t)(__atomic_load_n(&cr_wr, __ATOMIC_ACQUIRE) - __atomic_load_n(&cr_rd, __ATOMIC_ACQUIRE));
}

static int cr_put(const int16_t *f, uint32_t n)
{
    while (n) {
        if (!cr_on || cr_skip)
            return 0;
        uint32_t room = CR_RING - cr_fill();
        if (!room) {
            Sleep(10);
            continue;
        }
        uint32_t k = n < room ? n : room, w = (uint32_t)cr_wr;
        for (uint32_t i = 0; i < k; i++) {
            uint32_t at = ((w + i) & (CR_RING - 1)) * 2;
            if (f) { cr_ring[at] = f[2 * i]; cr_ring[at + 1] = f[2 * i + 1]; }
            else   { cr_ring[at] = 0;        cr_ring[at + 1] = 0; }
        }
        __atomic_store_n(&cr_wr, (LONG)(w + k), __ATOMIC_RELEASE);
        if (f) f += 2 * k;
        n -= k;
    }
    return 1;
}

#if GMV_DEV
#define CR_NAME(name) (name)
#else
#define CR_NAME(name) "a song"
#endif

typedef struct {
    double step, pos;
    int16_t last[2];
    int primed;
} cr_rs;

static int cr_rs_put(cr_rs *rs, const int16_t *in, uint32_t n)
{
    if (!n)
        return 1;
    if (rs->step == 1.0)
        return cr_put(in, n);
    if (!rs->primed) {
        rs->last[0] = in[0]; rs->last[1] = in[1];
        rs->pos = 1.0;
        rs->primed = 1;
    }
    int16_t out[2 * 1024];
    uint32_t o = 0;
    while (rs->pos < (double)n) {
        uint32_t idx = (uint32_t)rs->pos;
        double f = rs->pos - idx;
        const int16_t *a = idx ? in + 2 * (idx - 1) : rs->last, *b = in + 2 * idx;
        out[2 * o]     = (int16_t)lrint(a[0] + (b[0] - a[0]) * f);
        out[2 * o + 1] = (int16_t)lrint(a[1] + (b[1] - a[1]) * f);
        rs->pos += rs->step;
        if (++o == 1024) {
            if (!cr_put(out, o))
                return 0;
            o = 0;
        }
    }
    rs->pos -= n;
    rs->last[0] = in[2 * (n - 1)]; rs->last[1] = in[2 * (n - 1) + 1];
    return cr_put(out, o);
}

static int cr_play_builtin(const cr_ch *path, const char *name)
{
    gmv_song *s = gmv_song_open(path);
    if (!s)
        return 0;
    uint32_t out = (uint32_t)cr_rate, rate = gmv_song_rate(s);
    static int16_t buf[2 * 4096];
    cr_rs rs = { (double)rate / out, 0, {0, 0}, 0 };
    uint64_t frames = 0;
    size_t n;
    while ((n = gmv_song_read(s, buf, 4096)) != 0) {
        if (!frames)
            gmv_trace("[radio] custom station now playing \"%s\" (%s, %u Hz, %u ch%s)\n", name, gmv_song_decoder(s), rate,
                      gmv_song_channels(s), rate != out ? ", resampled" : "");
        frames += n;
        if (!cr_rs_put(&rs, buf, (uint32_t)n))
            break;
    }
    gmv_song_close(s);
    return frames != 0;
}

#ifdef _WIN32
static int cr_mf_ok;

static HRESULT cr_set_pcm(IMFSourceReader *r, UINT32 rate)
{
    IMFMediaType *t = NULL;
    HRESULT hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr) && rate) {
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, 2);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 4);
    }
    if (SUCCEEDED(hr)) hr = IMFSourceReader_SetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, t);
    if (t) IMFMediaType_Release(t);
    return hr;
}

static int cr_play_mf(const wchar_t *path, const char *name)
{
    IMFSourceReader *r = NULL;
    HRESULT hr = MFCreateSourceReaderFromURL(path, NULL, &r);
    if (FAILED(hr)) {
        gmv_log("[radio] custom station: cannot open \"%s\" (0x%08lx); next song\n", CR_NAME(name), (unsigned long)hr);
        return 0;
    }
    IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    UINT32 out = (UINT32)cr_rate;
    if (FAILED(cr_set_pcm(r, out)) && FAILED(hr = cr_set_pcm(r, 0))) {
        gmv_log("[radio] custom station: \"%s\" has no audio Windows can decode (0x%08lx); next song\n", CR_NAME(name),
                (unsigned long)hr);
        IMFSourceReader_Release(r);
        return 0;
    }
    UINT32 ch = 2, rate = out, bits = 16;
    IMFMediaType *t = NULL;
    if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &t)) && t) {
        IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
        IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        IMFMediaType_Release(t);
    }
    if (bits != 16 || !ch || !rate) {
        gmv_log("[radio] custom station: \"%s\" decodes to %u-bit %u ch %u Hz, not usable; next song\n", CR_NAME(name), bits,
                ch, rate);
        IMFSourceReader_Release(r);
        return 0;
    }
    gmv_trace("[radio] custom station now playing \"%s\" (%u Hz, %u ch%s)\n", name, rate, ch,
            rate != out ? ", resampled" : "");

    cr_rs rs = { (double)rate / out, 0, {0, 0}, 0 };
    int16_t *st = NULL;
    size_t stcap = 0;
    int go = 1;
    while (go) {
        DWORD flags = 0;
        IMFSample *s = NULL;
        hr = IMFSourceReader_ReadSample(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &s);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
            if (s) IMFSample_Release(s);
            break;
        }
        if (!s)
            continue;
        IMFMediaBuffer *mb = NULL;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(s, &mb)) && mb) {
            BYTE *p = NULL;
            DWORD maxlen = 0, len = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &p, &maxlen, &len)) && p) {
                uint32_t n = len / (2 * ch);
                const int16_t *in = (const int16_t *)p;
                if (ch != 2) {
                    if (stcap < (size_t)n * 2) {
                        free(st);
                        stcap = (size_t)n * 2;
                        st = malloc(stcap * sizeof *st);
                    }
                    if (st) {
                        for (uint32_t i = 0; i < n; i++) {
                            st[2 * i]     = in[i * ch];
                            st[2 * i + 1] = in[i * ch + (ch > 1)];
                        }
                    }
                    in = st;
                }
                if (in)
                    go = cr_rs_put(&rs, in, n);
                IMFMediaBuffer_Unlock(mb);
            }
            IMFMediaBuffer_Release(mb);
        }
        IMFSample_Release(s);
    }
    free(st);
    IMFSourceReader_Release(r);
    return 1;
}

static int cr_play_file(const wchar_t *path)
{
    char name[MAX_PATH * 3];
    const wchar_t *base = wcsrchr(path, L'\\');
    cr_utf8(base ? base + 1 : path, name, sizeof name);
    if (gmv_song_builtin(path)) {
        if (cr_play_builtin(path, name))
            return 1;
        gmv_trace("[radio] custom station: the host's decoder will not play \"%s\"; trying Media Foundation\n", name);
    }
    if (!cr_mf_ok) {
        gmv_log("[radio] custom station: \"%s\" needs Media Foundation, which did not start; next song\n", CR_NAME(name));
        return 0;
    }
    return cr_play_mf(path, name);
}

static DWORD WINAPI cr_thread(void *param)
{
    (void)param;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    cr_mf_ok = SUCCEEDED(hr);
    if (!cr_mf_ok)
        gmv_log("[radio] custom station: MFStartup failed (0x%08lx); only mp3, ogg, flac and wav songs will play\n",
                (unsigned long)hr);
    int fails = 0;
    for (;;) {
        if (!cr_on || !cr_rate) {
            Sleep(100);
            continue;
        }
        InterlockedExchange(&cr_skip, 0);
        wchar_t *p = cr_take_next();
        if (!p) {
            Sleep(1000);
            continue;
        }
        int ok = cr_play_file(p);
        free(p);
        if (!ok) {
            if (++fails >= 8) { Sleep(2000); fails = 0; }
            continue;
        }
        fails = 0;
        cr_put(NULL, (uint32_t)((uint64_t)cr_rate * CR_GAP_MS / 1000));
    }
    return 0;
}
#else
static int cr_have_ffmpeg(void)
{
    static int known, have;
    if (!known) {
        const char *args[] = {"ffmpeg", "-version", NULL};
        char *out = gmv_ff_output(args);
        have = out != NULL;
        free(out);
        known = 1;
        if (!have)
            gmv_log("[radio] custom station: this system has no ffmpeg, which m4a, aac and wma songs need; they are "
                    "left out\n");
    }
    return have;
}

static int cr_play_file(const char *path)
{
    const char *base = strrchr(path, '/');
    const char *name = base ? base + 1 : path;
    if (gmv_song_builtin(path)) {
        if (cr_play_builtin(path, name))
            return 1;
        if (!cr_have_ffmpeg()) {
            gmv_log("[radio] custom station: \"%s\" will not play; next song\n", CR_NAME(name));
            return 0;
        }
        gmv_trace("[radio] custom station: the host's decoder will not play \"%s\"; trying ffmpeg\n", name);
    }
    char rate[16];
    snprintf(rate, sizeof rate, "%ld", (long)cr_rate);
    const char *args[] = {"ffmpeg", "-nostdin", "-v", "error", "-i", path, "-map", "0:a:0", "-vn", "-f", "s16le",
                          "-ac", "2", "-ar", rate, "pipe:1", NULL};
    int fd, pid = gmv_ff_start(args, &fd);
    if (pid < 0) {
        gmv_log("[radio] custom station: ffmpeg did not start (%s); next song\n", strerror(errno));
        return 0;
    }
    static int16_t buf[2 * 4096];
    uint64_t frames = 0;
    for (;;) {
        uint32_t n = (uint32_t)(gmv_ff_read(fd, buf, sizeof buf) / 4);
        if (n && !frames)
            gmv_trace("[radio] custom station now playing \"%s\" (by ffmpeg, %s Hz)\n", name, rate);
        frames += n;
        if (!n || !cr_put(buf, n) || n < 4096)
            break;
    }
    int code = gmv_ff_stop(pid, fd);
    if (!frames && code) {
        gmv_log("[radio] custom station: \"%s\" has no audio ffmpeg can decode; next song\n", CR_NAME(name));
        return 0;
    }
    return 1;
}

static DWORD WINAPI cr_thread(void *param)
{
    (void)param;
    int fails = 0;
    for (;;) {
        if (!cr_on || !cr_rate) {
            Sleep(100);
            continue;
        }
        InterlockedExchange(&cr_skip, 0);
        char *p = cr_take_next();
        if (!p) {
            Sleep(1000);
            continue;
        }
        int ok = cr_play_file(p);
        free(p);
        if (!ok) {
            if (++fails >= 8) { Sleep(2000); fails = 0; }
            continue;
        }
        fails = 0;
        cr_put(NULL, (uint32_t)((uint64_t)cr_rate * CR_GAP_MS / 1000));
    }
    return 0;
}
#endif

void gmv_cr_set_volume(float v)
{
    cr_user_vol = v < 0 ? 0 : v > 1 ? 1 : v;
}

int gmv_cr_songs(void)
{
    return cr_inited ? cr_nsongs : 0;
}

int gmv_cr_enable(int on)
{
    cr_init();
    if (!on) {
        InterlockedExchange(&cr_on, 0);
        return cr_nsongs;
    }
    int n = cr_scan();
    char dir[CR_DIRMAX * 3];
    cr_utf8(cr_dir, dir, sizeof dir);
    if (n)
        gmv_log("[radio] custom station: %d song%s in \"%s\"\n", n, n == 1 ? "" : "s", dir);
    else
        gmv_log("[radio] custom station: no songs in \"%s\" (mp3, ogg, flac, wav, m4a, aac or wma); the radio keeps "
                "its five stations until there are\n", dir);
    InterlockedExchange(&cr_on, 1);
    if (!InterlockedExchange(&cr_thread_up, 1)) {
        HANDLE h = CreateThread(NULL, 0, cr_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        else { gmv_log("[radio] custom station: CreateThread failed (%lu)\n", GetLastError()); cr_thread_up = 0; }
    }
    return n;
}

/* [mm+0x218] [mm+0x220] [mm+0x270] */
static int cr_listening(void)
{
    unsigned char **mmp = *(unsigned char ***)(uintptr_t)CR_MM_PP;
    const unsigned char *mm = mmp ? *mmp : NULL;
    if (!mm)
        return 0;
    uint32_t fl = *(const volatile uint32_t *)(mm + 0x220);
    return (fl & 3) == 3 && !(fl & 0xb0) && *(const volatile int *)(mm + 0x21c) == GMV_CR_STATION &&
           *(const volatile int *)(mm + 0x218) == -1 && *(const volatile int *)(mm + 0x270) > GMV_CR_STATION;
}

static float cr_music_volume(void)
{
    const unsigned char *dev = *(unsigned char *const *)(uintptr_t)CR_AUDIO_DEVICE;
    if (!dev)
        return 0;
    const volatile float *g = (const volatile float *)(dev + 0x178 + CR_MUSIC_GROUP * 0x14);
    float start = g[0], target = g[1], t = g[2], dur = g[3];
    float v = (dur > 0 && t < dur) ? start + (target - start) * (t / dur) : target;
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

void gmv_cr_mix(int16_t *buf, uint32_t frames, uint32_t chans, uint32_t rate)
{
    if (!cr_thread_up || chans != 2)
        return;
    if (!cr_rate)
        InterlockedExchange(&cr_rate, (LONG)rate);
    int heard = cr_on && cr_listening();
    if (heard != cr_heard) {
        cr_heard = heard;
        if (GMV_DEV)
            gmv_logfile("[radio] custom station %s\n", heard ? "heard" : "not heard");
    }
    float target = heard ? cr_music_volume() * cr_user_vol : 0.0f;
    if (cr_gain <= 0 && target <= 0)
        return;
    float step = 1.0f / (0.04f * (float)rate);
    uint32_t have = cr_fill(), r = (uint32_t)cr_rd, used = 0;
    for (uint32_t i = 0; i < frames; i++) {
        if (cr_gain < target) { cr_gain += step; if (cr_gain > target) cr_gain = target; }
        else if (cr_gain > target) { cr_gain -= step; if (cr_gain < target) cr_gain = target; }
        if (used >= have)
            continue;
        uint32_t at = ((r + used) & (CR_RING - 1)) * 2;
        used++;
        for (int c = 0; c < 2; c++) {
            int v = buf[2 * i + c] + (int)lrintf(cr_ring[at + c] * cr_gain);
            buf[2 * i + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    __atomic_store_n(&cr_rd, (LONG)(r + used), __ATOMIC_RELEASE);
}
