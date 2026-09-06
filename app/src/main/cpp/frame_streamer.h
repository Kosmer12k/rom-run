#ifndef ENGINE_FRAME_STREAMER_H
#define ENGINE_FRAME_STREAMER_H

#include <android/native_window.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Frame header sent over the socket before each raw pixel frame.
 * Total size: 16 bytes.
 */
typedef struct {
    uint32_t magic;    // 0xF4AMED (frame magic)
    uint32_t width;
    uint32_t height;
    uint32_t format;   // 1 = RGBA_8888, 2 = BGRA_8888
} FrameHeader;

#define FRAME_MAGIC 0x00F4A3EDU

/**
 * Start the frame streamer background thread.
 * Connects to abstract UNIX socket @display_bridge_sock,
 * reads FrameHeader + raw pixels, blits to ANativeWindow.
 * Returns true if thread started.
 */
bool frame_streamer_start(ANativeWindow* window);

/**
 * Stop the frame streamer thread and disconnect.
 */
void frame_streamer_stop();

/**
 * Get current FPS (frames rendered in last second).
 */
float frame_streamer_get_fps();

/**
 * Returns true if the streamer is actively receiving frames.
 */
bool frame_streamer_is_active();

#ifdef __cplusplus
}
#endif

#endif // ENGINE_FRAME_STREAMER_H
