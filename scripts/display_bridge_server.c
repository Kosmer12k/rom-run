/*
 * display_bridge_server.c
 * Compiled as ARM64 static binary, runs inside Android guest.
 * Listens on abstract UNIX socket @display_bridge_sock,
 * captures frames via /dev/graphics/fb0 or screencap pipe,
 * sends FrameHeader + raw RGBA pixels to connected host.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#define SOCKET_NAME     "display_bridge_sock"
#define FRAME_MAGIC     0x00F4A3EDU  /* frame magic sentinel */
#define TARGET_FPS      25
#define FRAME_INTERVAL_NS (1000000000L / TARGET_FPS)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t format; /* 1=RGBA, 2=BGRA */
} FrameHeader;

static void sleep_ns(long ns) {
    struct timespec ts = { ns / 1000000000L, ns % 1000000000L };
    nanosleep(&ts, NULL);
}

static int create_server_socket() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, SOCKET_NAME, sizeof(addr.sun_path) - 2);
    socklen_t len = sizeof(sa_family_t) + 1 + strlen(SOCKET_NAME);

    if (bind(fd, (struct sockaddr*)&addr, len) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

static int send_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p   += n;
        len -= n;
    }
    return 0;
}

/*
 * Capture one frame using screencap piped to us.
 * screencap output: [4B width][4B height][4B format][pixels RGBA]
 * Returns 0 on success, sets *out_w, *out_h, allocates *pixels (caller frees).
 */
static int capture_via_screencap(uint8_t** pixels, uint32_t* out_w, uint32_t* out_h) {
    FILE* f = popen("/system/bin/screencap 2>/dev/null", "r");
    if (!f) return -1;

    uint32_t hdr[3]; /* width, height, format */
    if (fread(hdr, 4, 3, f) != 3) { pclose(f); return -1; }

    uint32_t w = hdr[0], h = hdr[1];
    if (w == 0 || h == 0 || w > 4096 || h > 4096) { pclose(f); return -1; }

    size_t nbytes = (size_t)w * h * 4;
    uint8_t* buf = (uint8_t*)malloc(nbytes);
    if (!buf) { pclose(f); return -1; }

    size_t got = fread(buf, 1, nbytes, f);
    pclose(f);

    if (got < nbytes) { free(buf); return -1; }

    *pixels = buf;
    *out_w  = w;
    *out_h  = h;
    return 0;
}

int main(void) {
    printf("[DisplayBridge] Starting on @%s at %dfps\n", SOCKET_NAME, TARGET_FPS);
    fflush(stdout);

    int srv = create_server_socket();
    if (srv < 0) {
        fprintf(stderr, "[DisplayBridge] Failed to create server socket\n");
        return 1;
    }
    printf("[DisplayBridge] Listening on @%s\n", SOCKET_NAME);
    fflush(stdout);

    for (;;) {
        printf("[DisplayBridge] Waiting for host...\n");
        fflush(stdout);

        int conn = accept(srv, NULL, NULL);
        if (conn < 0) { sleep(1); continue; }

        printf("[DisplayBridge] Host connected!\n");
        fflush(stdout);

        int frame_idx = 0;
        int ok = 1;
        while (ok) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);

            uint8_t*  pixels = NULL;
            uint32_t  w, h;
            if (capture_via_screencap(&pixels, &w, &h) < 0) {
                fprintf(stderr, "[DisplayBridge] screencap failed, retrying...\n");
                sleep_ns(500000000L); /* 0.5s */
                continue;
            }

            FrameHeader fhdr = {
                .magic  = FRAME_MAGIC,
                .width  = w,
                .height = h,
                .format = 1  /* RGBA_8888 */
            };

            if (send_all(conn, &fhdr, sizeof(fhdr)) < 0 ||
                send_all(conn, pixels, (size_t)w * h * 4) < 0) {
                printf("[DisplayBridge] Host disconnected\n");
                ok = 0;
            }

            free(pixels);
            frame_idx++;
            if (frame_idx % TARGET_FPS == 0) {
                printf("[DisplayBridge] Sent %d frames (%ux%u)\n", frame_idx, w, h);
                fflush(stdout);
            }

            clock_gettime(CLOCK_MONOTONIC, &t1);
            long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L
                         + (t1.tv_nsec - t0.tv_nsec);
            long sleep_t = FRAME_INTERVAL_NS - elapsed;
            if (sleep_t > 0) sleep_ns(sleep_t);
        }

        close(conn);
    }

    close(srv);
    return 0;
}
