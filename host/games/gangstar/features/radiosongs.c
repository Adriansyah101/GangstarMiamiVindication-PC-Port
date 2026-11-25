#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "addon_io.h"

#define RS_RES_PACK  "file000482.dat"
#define RS_ST_PACK   "file000338.dat"
#define RS_RES_MAC   1728
#define RS_STATIONS  5

static const struct { const char *file; int station; } rs_songs[] = {
    { "m_radio_electro_stompbox.wav",   1 },
    { "m_radio_electro_jerkit.wav",     1 },
    { "m_radio_hiphop_bump.wav",        2 },
    { "m_radio_hiphop_volcano.wav",     2 },
    { "m_radio_hiphop_jerkit.wav",      2 },
    { "m_radio_hiphop_boombapcats.wav", 2 },
    { "m_radio_rock_policesign.wav",    4 },
    { "m_radio_rock_bigbadwolf.wav",    4 },
    { "m_radio_rock_notime.wav",        4 },
    { "m_radio_rock_java_02.wav",       4 },
    { "m_radio_rock_java_03.wav",       4 },
};
#define RS_NSONGS (int)(sizeof rs_songs / sizeof *rs_songs)

static char rs_dir[MAX_PATH * 2], rs_cache[MAX_PATH * 2];
static int rs_have[RS_NSONGS], rs_found;
static int rs_id[RS_NSONGS];
static int rs_built;
static int rs_mac[RS_STATIONS], rs_all[RS_STATIONS];
static CRITICAL_SECTION rs_cs;

int gmv_rs_on(void)
{
    const char *e = gmv_config_live("GMV_RADIO_SONGS");
    return gmv_env_on(e);
}

int gmv_rs_init(void)
{
    char exe[MAX_PATH], pak[MAX_PATH * 2];
    int want = gmv_rs_on();
    InitializeCriticalSection(&rs_cs);
    if (!GetModuleFileNameA(NULL, exe, sizeof exe))
        return 0;
    *strrchr(exe, GMV_DIRSEP) = 0;
    snprintf(rs_cache, sizeof rs_cache, "%s" GMV_SEP "cache" GMV_SEP "radio", exe);
    snprintf(pak, sizeof pak, "%s" GMV_SEP "add-on" GMV_SEP "radio.pak", exe);
    if (GetFileAttributesA(pak) != INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryA(rs_cache, NULL);
        snprintf(rs_dir, sizeof rs_dir, "%s" GMV_SEP "cache" GMV_SEP "radio" GMV_SEP "songs", exe);
        if (!gmv_pak_unpack(pak, rs_dir, "radio songs"))
            return 0;
    } else
        snprintf(rs_dir, sizeof rs_dir, "%s" GMV_SEP "add-on" GMV_SEP "radio", exe);
    for (int i = 0; i < RS_NSONGS; i++) {
        char p[MAX_PATH * 3];
        snprintf(p, sizeof p, "%s" GMV_SEP "%s", rs_dir, rs_songs[i].file);
        rs_have[i] = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
        rs_found += rs_have[i];
        if (!rs_have[i] && want)
            gmv_log("[radio songs] missing %s\n", p);
    }
    if (rs_found) {
        CreateDirectoryA(rs_cache, NULL);
        gmv_log("[radio songs] add-on found: %s (%d of %d songs)\n", rs_dir, rs_found, RS_NSONGS);
    } else if (want)
        gmv_log("[radio songs] RADIO_SONGS=1 but %s holds none of the songs; the radio stays the Mac's\n", rs_dir);
    return rs_found > 0;
}

static int build_res(const char *path, const char *dst)
{
    char via[MAX_PATH * 2];
    const char *src = gmv_uc_redirect(path, via, sizeof via);
    size_t n = 0;
    unsigned char *d = read_all(src ? src : path, &n, 0), *out = NULL;
    int ok = 0;
    if (!d || n < 4)
        goto done;
    unhide(d);
    uint32_t cnt = rdbe(d);
    size_t o = 4;
    for (uint32_t k = 0; k < 2 * cnt && o + 4 <= n; k++)
        o += 4 + rdbe(d + o);
    if (cnt < RS_RES_MAC || o != n) {
        gmv_log("[radio songs] %s: %u sounds, %u of %u bytes parsed; not the Mac's list, left as it is\n",
                src ? src : path, cnt, (unsigned)o, (unsigned)n);
        goto done;
    }
    out = malloc(n + RS_NSONGS * 128);
    if (!out)
        goto done;
    memcpy(out, d, n);
    o = n;
    uint32_t id = cnt;
    for (int i = 0; i < RS_NSONGS; i++) {
        if (!rs_have[i])
            continue;
        const char *f = rs_songs[i].file;
        char name[64];
        size_t l = strlen(f);
        snprintf(name, sizeof name, "%.*s_wav", (int)(l - 4), f);
        wrbe(out + o, (uint32_t)l), memcpy(out + o + 4, f, l), o += 4 + l;
        l = strlen(name);
        wrbe(out + o, (uint32_t)l), memcpy(out + o + 4, name, l), o += 4 + l;
        rs_id[i] = (int)id++;
    }
    wrbe(out, id);
    hide(out);
    ok = write_all(dst, out, o);
    if (ok) {
        gmv_log("[radio songs] sounds %u..%u: the songs\n", cnt, id - 1);
    } else
        gmv_log("[radio songs] could not write %s\n", dst);
done:
    free(d), free(out);
    return ok;
}

/* 0x95610 0x60ac8 0x5f58e */
static int build_stations(const char *path, const char *dst)
{
    size_t n = 0;
    unsigned char *d = read_all(path, &n, 0), *out = NULL;
    int ok = 0, st = 0;
    size_t o = 4, q = 4;
    if (!d || n < 4)
        goto done;
    unhide(d);
    if (rd32(d) != RS_STATIONS)
        goto bad;
    out = malloc(n + RS_NSONGS * 4);
    if (!out)
        goto done;
    wr32(out, RS_STATIONS);
    for (st = 0; st < RS_STATIONS; st++) {
        if (o + 4 > n)
            goto bad;
        uint32_t na = rd32(d + o);
        if (na > (n - o - 4) / 4)
            goto bad;
        memcpy(out + q, d + o, 4 + na * 4);
        o += 4 + na * 4, q += 4 + na * 4;
        if (o + 4 > n)
            goto bad;
        uint32_t nb = rd32(d + o);
        if (nb > (n - o - 4) / 4 || (n - o - 4 - nb * 4) < 4)
            goto bad;
        size_t at = q;
        memcpy(out + q + 4, d + o + 4, nb * 4);
        o += 4 + nb * 4, q += 4 + nb * 4;
        rs_mac[st] = (int)nb;
        for (int i = 0; i < RS_NSONGS; i++)
            if (rs_have[i] && rs_songs[i].station == st) {
                wr32(out + q, (uint32_t)rs_id[i]), q += 4;
                nb++;
            }
        wr32(out + at, nb);
        rs_all[st] = (int)nb;
        memcpy(out + q, d + o, 4);
        o += 4, q += 4;
    }
    if (n - o > 3)
        goto bad;
    memcpy(out + q, d + o, n - o);
    q += n - o;
    hide(out);
    ok = write_all(dst, out, q);
    if (ok) {
        rs_built = 1;
        gmv_log("[radio songs] songs per station: %d/%d %d/%d %d/%d %d/%d %d/%d (the Mac's / with the add-on)\n",
                rs_mac[0], rs_all[0], rs_mac[1], rs_all[1], rs_mac[2], rs_all[2], rs_mac[3], rs_all[3], rs_mac[4], rs_all[4]);
    } else
        gmv_log("[radio songs] could not write %s\n", dst);
    goto done;
bad:
    gmv_log("[radio songs] %s is not the Mac's station list (station %d, %u of %u bytes); left as it is\n", path, st,
            (unsigned)o, (unsigned)n);
done:
    free(d), free(out);
    return ok;
}

static int res_ready(const char *path, char *dst, size_t n)
{
    static int built, good;
    char src[MAX_PATH * 2];
    snprintf(src, sizeof src, "%.*s%s", (int)(base_name(path) - path), path, RS_RES_PACK);
    snprintf(dst, n, "%s" GMV_SEP "%s", rs_cache, RS_RES_PACK);
    if (!built) {
        built = 1;
        good = build_res(src, dst);
    }
    return good;
}

/* 0x53c530 */
const char *gmv_rs_redirect(const char *path, char *buf, size_t n)
{
    if (!rs_found || !path || !strstr(path, "_packMac"))
        return NULL;
    const char *b = base_name(path);
    if (!_strnicmp(b, "m_radio_", 8)) {
        static volatile LONG seen;
        for (int i = 0; i < RS_NSONGS; i++)
            if (rs_have[i] && !_stricmp(b, rs_songs[i].file)) {
                snprintf(buf, n, "%s" GMV_SEP "%s", rs_dir, rs_songs[i].file);
                if (!(InterlockedOr(&seen, 1L << i) & (1L << i)))
                    gmv_log("[radio songs] %s from the add-on\n", rs_songs[i].file);
                return buf;
            }
        return NULL;
    }
    int res = !_stricmp(b, RS_RES_PACK), st = !_stricmp(b, RS_ST_PACK);
    if (!res && !st)
        return NULL;
    static int st_tried;
    EnterCriticalSection(&rs_cs);
    int ok = res_ready(path, buf, n);
    if (st && ok) {
        snprintf(buf, n, "%s" GMV_SEP "%s", rs_cache, RS_ST_PACK);
        if (!st_tried) {
            st_tried = 1;
            build_stations(path, buf);
        }
        ok = rs_built;
    }
    LeaveCriticalSection(&rs_cs);
    return ok ? buf : NULL;
}

int gmv_rs_have(void)
{
    return rs_found > 0;
}

int gmv_rs_counts(int st, int *mac, int *all)
{
    if (!rs_built || st < 0 || st >= RS_STATIONS)
        return 0;
    *mac = rs_mac[st];
    *all = rs_all[st];
    return 1;
}

int gmv_rs_is_song(int id)
{
    for (int i = 0; i < RS_NSONGS; i++)
        if (rs_built && rs_have[i] && rs_id[i] == id)
            return 1;
    return 0;
}

#ifdef RS_TEST
void rs_test_set(const char *dir, const char *cache)
{
    InitializeCriticalSection(&rs_cs);
    snprintf(rs_dir, sizeof rs_dir, "%s", dir);
    snprintf(rs_cache, sizeof rs_cache, "%s", cache);
    for (int i = 0; i < RS_NSONGS; i++) {
        char p[MAX_PATH * 3];
        snprintf(p, sizeof p, "%s" GMV_SEP "%s", rs_dir, rs_songs[i].file);
        rs_have[i] = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
        rs_found += rs_have[i];
    }
}
#endif
