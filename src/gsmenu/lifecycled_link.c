#include "lifecycled_link.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LIFECYCLED_HOST      "127.0.0.1"
#define LIFECYCLED_PORT      8899
#define POLL_INTERVAL_US     (500 * 1000)
#define IO_TIMEOUT_MS        400
#define STALE_AFTER_MS       2000

static atomic_bool     g_connected;
static atomic_llong    g_last_answer_ms; /* monotonic ms of the last good answer, 0 = none */
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One GET /api/v1/link. Returns 1 connected, 0 not connected, -1 no usable
 * answer (lifecycled down, timeout, garbage). */
static int poll_once(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = IO_TIMEOUT_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(LIFECYCLED_PORT) };
    inet_pton(AF_INET, LIFECYCLED_HOST, &addr.sin_addr);

    int  result = -1;
    char buf[2048];
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
                result = strstr(buf, "\"state\":\"connected\"") ? 1 : 0;
            }
        }
    }
    close(fd);
    return result;
}

static void *poll_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int r = poll_once();
        if (r >= 0) {
            atomic_store(&g_connected, r == 1);
            atomic_store(&g_last_answer_ms, now_ms());
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
