#ifndef ENGINE_DISPLAY_RENDERER_H
#define ENGINE_DISPLAY_RENDERER_H

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <cstdint>

#define DISPLAY_LOG_TAG "Engine-Display"

namespace engine::display {

/**
 * Allocates a physical DMA-BUF GPU memory buffer via AHardwareBuffer.
 * Configured for RGBA_8888, GPU sampled, GPU framebuffer, and CPU read/write access.
 * Returns pointer to AHardwareBuffer on success, or nullptr on failure.
 */
AHardwareBuffer* allocate_guest_buffer(uint32_t width, uint32_t height);

/**
 * Releases reference to an AHardwareBuffer.
 */
void release_guest_buffer(AHardwareBuffer* buffer);

/**
 * Increments strong reference count on an AHardwareBuffer.
 */
void acquire_guest_buffer(AHardwareBuffer* buffer);

/**
 * Sends the hardware buffer file descriptors over a Unix domain socket (zero-copy IPC).
 * Returns 0 on success, or negative error code.
 */
int send_buffer_handle_to_socket(const AHardwareBuffer* buffer, int socket_fd);

/**
 * Receives the hardware buffer file descriptors from a Unix domain socket (zero-copy IPC).
 * Returns 0 on success, or negative error code.
 */
int receive_buffer_handle_from_socket(int socket_fd, AHardwareBuffer** out_buffer);

/**
 * Renders raw pixel data from AHardwareBuffer into host ANativeWindow.
 * Enforces stride alignment calculation (dst_stride vs src_stride)
 * to avoid memory tearing, pixel misalignment, and diagonal skews.
 * Returns 0 on success, or negative error code.
 */
int render_to_surface(
    AHardwareBuffer* buffer,
    ANativeWindow* window,
    uint32_t width,
    uint32_t height
);

} // namespace engine::display

// ============================================================================
// C-Linkage Exported Prototypes
// ============================================================================
#ifdef __cplusplus
extern "C" {
#endif

AHardwareBuffer* display_allocate_guest_buffer(uint32_t width, uint32_t height);
void display_release_guest_buffer(AHardwareBuffer* buffer);
int display_send_buffer_handle(const AHardwareBuffer* buffer, int socket_fd);
int display_receive_buffer_handle(int socket_fd, AHardwareBuffer** out_buffer);
int display_render_to_surface(
    AHardwareBuffer* buffer,
    ANativeWindow* window,
    uint32_t width,
    uint32_t height
);

#ifdef __cplusplus
}
#endif

#endif // ENGINE_DISPLAY_RENDERER_H
