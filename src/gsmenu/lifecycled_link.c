#include "lifecycled_link.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../menu.h" /* RXMODE */
#ifndef USE_SIMULATOR
#include "../osd.h"  /* osd.cpp isn't in the sim build */
#endif

#define LIFECYCLED_HOST      "127.0.0.1"
#define LIFECYCLED_PORT      8899
#define POLL_INTERVAL_US     (500 * 1000)
#define IO_TIMEOUT_MS        400
#define STALE_AFTER_MS       2000

static atomic_bool     g_connected;
static atomic_llong    g_last_answer_ms; /* monotonic ms of the last good answer, 0 = none */
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;

/* One side of /api/v1/link's "quality" object (lifecycled's BB_GET_1V1_INFO). */
typedef struct {
    bool   present;
    double snr_db;
    bool   has_snr;   /* snr_db is null when the chip doesn't report it (e.g. the air's peer) */
    long   ldpc_err;
    long   gain_a, gain_b;
    long   tx_mcs, tx_power, tx_freq_khz;
} link_side_t;

typedef struct {
    int         connected; /* 1 connected, 0 not */
    bool        is_dev;    /* lifecycled's role: ground = dev, so self = ground */
    bool        has_quality;
    long        signal_level;
    link_side_t self, peer;
} link_info_t;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Minimal lookups in lifecycled's own, flat JSON (no nested objects inside
 * the ones searched). Each returns false if the key is missing or null. */
static const char *find_key(const char *obj, const char *end, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(obj, pat);
    return (p && (!end || p < end)) ? p + strlen(pat) : NULL;
}

static bool get_num(const char *obj, const char *end, const char *key, double *out)
{
    const char *p = find_key(obj, end, key);
    return p && sscanf(p, "%lf", out) == 1;
}

static bool parse_side(const char *buf, const char *key, link_side_t *s)
{
    memset(s, 0, sizeof(*s));
    const char *obj = find_key(buf, NULL, key);
    if (!obj || *obj != '{') {
        return false;
    }
    const char *end = strchr(obj, '}');
    double v;
    s->has_snr = get_num(obj, end, "snr_db", &s->snr_db);
    if (get_num(obj, end, "ldpc_err", &v))    s->ldpc_err = (long)v;
    if (get_num(obj, end, "tx_mcs", &v))      s->tx_mcs = (long)v;
    if (get_num(obj, end, "tx_power", &v))    s->tx_power = (long)v;
    if (get_num(obj, end, "tx_freq_khz", &v)) s->tx_freq_khz = (long)v;
    const char *g = find_key(obj, end, "gain");
    if (g) {
        sscanf(g, "[%ld,%ld]", &s->gain_a, &s->gain_b);
    }
    s->present = true;
    return true;
}

/* One GET /api/v1/link. Returns false without a usable answer (lifecycled
 * down, timeout, garbage). */
static bool poll_once(link_info_t *info)
{
    memset(info, 0, sizeof(*info));
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = IO_TIMEOUT_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(LIFECYCLED_PORT) };
    inet_pton(AF_INET, LIFECYCLED_HOST, &addr.sin_addr);

    bool   ok = false;
    char   buf[4096];
    size_t len = 0;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        static const char req[] = "GET /api/v1/link HTTP/1.0\r\nHost: " LIFECYCLED_HOST "\r\n\r\n";
        if (send(fd, req, sizeof(req) - 1, MSG_NOSIGNAL) == (ssize_t)(sizeof(req) - 1)) {
            ssize_t n;
            while (len < sizeof(buf) - 1 && (n = recv(fd, buf + len, sizeof(buf) - 1 - len, 0)) > 0) {
                len += (size_t)n;
            }
            buf[len] = '\0';
            if (strstr(buf, "\"ok\":true")) {
                ok              = true;
                info->connected = strstr(buf, "\"state\":\"connected\"") != NULL;
                info->is_dev    = strstr(buf, "\"role\":\"dev\"") != NULL;
                const char *q   = find_key(buf, NULL, "quality");
                double      lvl;
                if (q && *q == '{' && get_num(q, NULL, "signal_level", &lvl)) {
                    info->has_quality  = true;
                    info->signal_level = (long)lvl;
                    parse_side(q, "self", &info->self);
                    parse_side(q, "peer", &info->peer);
                }
            }
        }
    }
    close(fd);
    return ok;
}

#ifndef USE_SIMULATOR
static void add_side_facts(void *batch, const link_side_t *s, const char *side)
{
    if (!s->present) {
        return;
    }
    osd_tag tags[2];
    strcpy(tags[0].key, "side");
    strcpy(tags[0].val, side);
    if (s->has_snr) {
        /* the chip only reports rx figures where it measured them: the air
         * doesn't get the ground's back */
        osd_add_double_fact(batch, "ar8030.rx.snr_db", tags, 1, s->snr_db);
        osd_add_int_fact(batch, "ar8030.rx.ldpc_err", tags, 1, s->ldpc_err);
        strcpy(tags[1].key, "path");
        strcpy(tags[1].val, "a");
        osd_add_int_fact(batch, "ar8030.rx.gain", tags, 2, s->gain_a);
        strcpy(tags[1].val, "b");
        osd_add_int_fact(batch, "ar8030.rx.gain", tags, 2, s->gain_b);
    }
    osd_add_int_fact(batch, "ar8030.tx.mcs", tags, 1, s->tx_mcs);
    osd_add_int_fact(batch, "ar8030.tx.freq_mhz", tags, 1, s->tx_freq_khz / 1000);
    osd_add_int_fact(batch, "ar8030.tx.power", tags, 1, s->tx_power);
}

/* Publishes ar8030.* OSD facts (see README "Artosyn (AR8030) link facts").
 * Without a link only signal_level (0) remains; the per-side facts are
 * flushed once so widgets show no value instead of the last reading. */
static void publish_facts(const link_info_t *info)
{
    static bool had_quality = false;
    const char *self_side = info->is_dev ? "ground" : "air";
    const char *peer_side = info->is_dev ? "air" : "ground";

    void *batch = osd_batch_init(16);
    osd_add_int_fact(batch, "ar8030.signal_level", NULL, 0, info->has_quality ? info->signal_level : 0);
    if (info->has_quality) {
        add_side_facts(batch, &info->self, self_side);
        add_side_facts(batch, &info->peer, peer_side);
    }
    osd_publish_batch(batch);

    if (had_quality && !info->has_quality) {
        static const char *const prefixes[] = { "ar8030.rx.", "ar8030.tx." };
        osd_flush_facts(prefixes, 2);
    }
    had_quality = info->has_quality;
}
#endif

static void *poll_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (RXMODE == ARTOSYN) {
            link_info_t info;
            if (poll_once(&info)) {
                atomic_store(&g_connected, info.connected == 1);
                atomic_store(&g_last_answer_ms, now_ms());
            }
#ifndef USE_SIMULATOR
            publish_facts(&info); /* zeroed without an answer: level 0, per-side facts flushed */
#endif
        }
        usleep(POLL_INTERVAL_US);
    }
    return NULL;
}

static void start_thread(void)
{
    pthread_t th;
    if (pthread_create(&th, NULL, poll_thread, NULL) == 0) {
        pthread_detach(th);
    }
}

bool lifecycled_link_connected(void)
{
    pthread_once(&g_once, start_thread);
    long long last = atomic_load(&g_last_answer_ms);
    if (last == 0 || now_ms() - last > STALE_AFTER_MS) {
        return false;
    }
    return atomic_load(&g_connected);
}
