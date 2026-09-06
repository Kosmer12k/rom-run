#define _GNU_SOURCE
#include "frame_streamer.h"
#include <android/log.h>
#include <android/native_window.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>

#define TAG "FrameStreamer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

#define SOCKET_NAME     "display_bridge_sock"
#define TARGET_FPS      30
#define FRAME_INTERVAL_NS (1000000000L / TARGET_FPS)
#define MAX_FRAME_BYTES (1920 * 1080 * 4)  // max 1080p RGBA

// ============================================================================
// State
// ============================================================================

static pthread_t        g_thread;
static atomic_bool      g_running   = ATOMIC_VAR_INIT(false);
static atomic_bool      g_active    = ATOMIC_VAR_INIT(false);
static ANativeWindow*   g_window    = NULL;
static float            g_fps       = 0.0f;
static uint8_t*         g_framebuf  = NULL;

// ============================================================================
// Helpers
// ============================================================================

static int connect_to_bridge() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    // Abstract namespace socket
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, SOCKET_NAME, sizeof(addr.sun_path) - 2);
    socklen_t addrlen = sizeof(sa_family_t) + 1 + strlen(SOCKET_NAME);

    if (connect(fd, (struct sockaddr*)&addr, addrlen) < 0) {
        LOGE("connect(@%s) failed: %s", SOCKET_NAME, strerror(errno));
        close(fd);
        return -1;
    }

    LOGI("Connected to display bridge @%s", SOCKET_NAME);
    return fd;
}

static int read_exact(int fd, void* buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = recv(fd, (uint8_t*)buf + done, len - done, 0);
        if (n <= 0) return -1;
        done += n;
    }
    return 0;
}

static int blit_rgba_to_window(ANativeWindow* win,
                                const uint8_t* pixels,
                                uint32_t w, uint32_t h,
                                bool swap_rb) {
    ANativeWindow_setBuffersGeometry(win, (int)w, (int)h,
                                     0 /* keep existing format */);

    ANativeWindow_Buffer buf;
    ARect dirty = {0, 0, (int32_t)w, (int32_t)h};
    if (ANativeWindow_lock(win, &buf, &dirty) != 0) {
        LOGE("ANativeWindow_lock failed");
        return -1;
    }

    const size_t src_stride = w * 4;
    const size_t dst_stride = (size_t)buf.stride * 4;
    const size_t copy_w     = (size_t)buf.width  < w ? (size_t)buf.width  : w;
    const size_t copy_h     = (size_t)buf.height < h ? (size_t)buf.height : h;
    const size_t row_bytes  = copy_w * 4;

    uint8_t* dst = (uint8_t*)buf.bits;
    const uint8_t* src = pixels;

    if (!swap_rb) {
        // RGBA → RGBA (direct blit)
        for (size_t y = 0; y < copy_h; y++) {
            memcpy(dst + y * dst_stride, src + y * src_stride, row_bytes);
        }
    } else {
        // BGRA → RGBA (swap R and B per pixel)
        for (size_t y = 0; y < copy_h; y++) {
            const uint8_t* s = src + y * src_stride;
            uint8_t*       d = dst + y * dst_stride;
            for (size_t x = 0; x < copy_w; x++) {
                d[0] = s[2]; // R ← B
                d[1] = s[1]; // G
                d[2] = s[0]; // B ← R
                d[3] = s[3]; // A
                s += 4; d += 4;
            }
        }
    }

    ANativeWindow_unlockAndPost(win);
    return 0;
}

// ============================================================================
// Streamer Thread
// ============================================================================

static void* streamer_thread(void* arg) {
    (void)arg;
    LOGI("Streamer thread started");

    g_framebuf = (uint8_t*)malloc(MAX_FRAME_BYTES);
    if (!g_framebuf) {
        LOGE("Failed to allocate frame buffer (%d bytes)", MAX_FRAME_BYTES);
        return NULL;
    }

    int   frame_count   = 0;
    long  fps_window_ns = 0;
    struct timespec ts_prev, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_prev);

    while (atomic_load(&g_running)) {
        int fd = connect_to_bridge();
        if (fd < 0) {
            LOGI("Bridge not ready, retrying in 1s...");
            sleep(1);
            continue;
        }

        atomic_store(&g_active, true);
        LOGI("Frame stream active");

        while (atomic_load(&g_running)) {
            struct timespec frame_start;
            clock_gettime(CLOCK_MONOTONIC, &frame_start);

            // Read frame header
            FrameHeader hdr;
            if (read_exact(fd, &hdr, sizeof(hdr)) < 0) {
                LOGE("Failed reading frame header, reconnecting...");
                break;
            }

            if (hdr.magic != FRAME_MAGIC) {
                LOGE("Bad frame magic: 0x%08X (expected 0x%08X)", hdr.magic, FRAME_MAGIC);
                break;
            }

            uint32_t w = hdr.width;
            uint32_t h = hdr.height;
            size_t frame_bytes = (size_t)w * h * 4;

            if (w == 0 || h == 0 || frame_bytes > MAX_FRAME_BYTES) {
                LOGE("Invalid frame dimensions %ux%u", w, h);
                break;
            }

            // Read pixel data
            if (read_exact(fd, g_framebuf, frame_bytes) < 0) {
                LOGE("Failed reading frame pixels, reconnecting...");
                break;
            }

            // Blit to SurfaceView
            ANativeWindow* win = g_window;
            if (win) {
                bool swap_rb = (hdr.format == 2); // 2 = BGRA needs swap
                blit_rgba_to_window(win, g_framebuf, w, h, swap_rb);
            }

            // FPS tracking
            frame_count++;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            long elapsed_ns = (ts_now.tv_sec  - ts_prev.tv_sec)  * 1000000000L
                            + (ts_now.tv_nsec - ts_prev.tv_nsec);
            fps_window_ns += elapsed_ns;
            ts_prev = ts_now;

            if (fps_window_ns >= 1000000000L) {
                g_fps = (float)frame_count * 1e9f / (float)fps_window_ns;
                LOGI("FPS: %.1f  (%ux%u)", g_fps, w, h);
                frame_count   = 0;
                fps_window_ns = 0;
            }

            // Frame rate limiter
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            long frame_ns = (ts_now.tv_sec  - frame_start.tv_sec)  * 1000000000L
                          + (ts_now.tv_nsec - frame_start.tv_nsec);
            long sleep_ns = FRAME_INTERVAL_NS - frame_ns;
            if (sleep_ns > 0) {
                struct timespec ts_sleep = {0, sleep_ns};
                nanosleep(&ts_sleep, NULL);
            }
        }

        atomic_store(&g_active, false);
        close(fd);
        LOGI("Disconnected, will reconnect...");
    }

    free(g_framebuf);
    g_framebuf = NULL;
    LOGI("Streamer thread exiting");
    return NULL;
}

// ============================================================================
// Public API
// ============================================================================

bool frame_streamer_start(ANativeWindow* window) {
    if (atomic_load(&g_running)) {
        LOGI("Streamer already running, updating window");
        g_window = window;
        if (window) ANativeWindow_acquire(window);
        return true;
    }

    g_window = window;
    if (window) ANativeWindow_acquire(window);

    atomic_store(&g_running, true);

    int ret = pthread_create(&g_thread, NULL, streamer_thread, NULL);
    if (ret != 0) {
        LOGE("pthread_create failed: %d", ret);
        atomic_store(&g_running, false);
        g_window = NULL;
        return false;
    }

    LOGI("Frame streamer started");
    return true;
}

void frame_streamer_stop() {
    if (!atomic_load(&g_running)) return;

    atomic_store(&g_running, false);
    pthread_join(g_thread, NULL);

    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = NULL;
    }

    atomic_store(&g_active, false);
    g_fps = 0.0f;
    LOGI("Frame streamer stopped");
}

float frame_streamer_get_fps() {
    return g_fps;
}

bool frame_streamer_is_active() {
    return atomic_load(&g_active);
}
