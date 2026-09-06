#include "display_renderer.h"
#include <android/log.h>
#include <cstring>
#include <algorithm>

#define LOGD_DISP(...) __android_log_print(ANDROID_LOG_DEBUG, DISPLAY_LOG_TAG, __VA_ARGS__)
#define LOGI_DISP(...) __android_log_print(ANDROID_LOG_INFO,  DISPLAY_LOG_TAG, __VA_ARGS__)
#define LOGW_DISP(...) __android_log_print(ANDROID_LOG_WARN,  DISPLAY_LOG_TAG, __VA_ARGS__)
#define LOGE_DISP(...) __android_log_print(ANDROID_LOG_ERROR, DISPLAY_LOG_TAG, __VA_ARGS__)

#define EXPORT_SYMBOL __attribute__((visibility("default")))

namespace engine::display {

AHardwareBuffer* allocate_guest_buffer(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        LOGE_DISP("allocate_guest_buffer: invalid resolution %ux%u", width, height);
        return nullptr;
    }

    AHardwareBuffer_Desc desc{};
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                 AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                 AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    desc.stride = 0;
    desc.rfu0 = 0;
    desc.rfu1 = 0;

    AHardwareBuffer* buffer = nullptr;
    int res = AHardwareBuffer_allocate(&desc, &buffer);
    if (res != 0 || buffer == nullptr) {
        LOGE_DISP("AHardwareBuffer_allocate failed with error code: %d", res);
        return nullptr;
    }

    AHardwareBuffer_Desc actualDesc{};
    AHardwareBuffer_describe(buffer, &actualDesc);

    LOGI_DISP("AHardwareBuffer allocated successfully: %ux%u (stride=%u, format=R8G8B8A8, ptr=%p)",
              actualDesc.width, actualDesc.height, actualDesc.stride, buffer);
    return buffer;
}

void release_guest_buffer(AHardwareBuffer* buffer) {
    if (buffer != nullptr) {
        AHardwareBuffer_release(buffer);
    }
}

void acquire_guest_buffer(AHardwareBuffer* buffer) {
    if (buffer != nullptr) {
        AHardwareBuffer_acquire(buffer);
    }
}

int send_buffer_handle_to_socket(const AHardwareBuffer* buffer, int socket_fd) {
    if (!buffer || socket_fd < 0) {
        LOGE_DISP("send_buffer_handle: invalid buffer (%p) or socket (%d)", buffer, socket_fd);
        return -1;
    }

    int res = AHardwareBuffer_sendHandleToUnixSocket(buffer, socket_fd);
    if (res != 0) {
        LOGE_DISP("AHardwareBuffer_sendHandleToUnixSocket failed: %d", res);
        return res;
    }

    LOGD_DISP("Buffer handle successfully sent over Unix socket (fd=%d)", socket_fd);
    return 0;
}

int receive_buffer_handle_from_socket(int socket_fd, AHardwareBuffer** out_buffer) {
    if (socket_fd < 0 || !out_buffer) {
        LOGE_DISP("receive_buffer_handle: invalid socket (%d) or out_buffer (%p)", socket_fd, out_buffer);
        return -1;
    }

    int res = AHardwareBuffer_recvHandleFromUnixSocket(socket_fd, out_buffer);
    if (res != 0 || *out_buffer == nullptr) {
        LOGE_DISP("AHardwareBuffer_recvHandleFromUnixSocket failed: %d", res);
        *out_buffer = nullptr;
        return res;
    }

    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(*out_buffer, &desc);
    LOGD_DISP("Buffer handle successfully received from Unix socket: %ux%u (stride=%u)",
              desc.width, desc.height, desc.stride);
    return 0;
}

int render_to_surface(
    AHardwareBuffer* buffer,
    ANativeWindow* window,
    uint32_t width,
    uint32_t height
) {
    if (!buffer) {
        LOGE_DISP("render_to_surface: buffer is null");
        return -1;
    }
    if (!window) {
        LOGE_DISP("render_to_surface: target ANativeWindow is null");
        return -2;
    }

    // 1. Inquire buffer geometry and hardware stride
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(buffer, &desc);

    // 2. Lock AHardwareBuffer for CPU reading
    void* src_raw_pixels = nullptr;
    int lock_res = AHardwareBuffer_lock(
        buffer,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
        -1,       // Wait immediately for rendering fences
        nullptr,  // Lock entire buffer region
        &src_raw_pixels
    );

    if (lock_res != 0 || !src_raw_pixels) {
        LOGE_DISP("Failed to lock AHardwareBuffer: error=%d", lock_res);
        return lock_res;
    }

    // 3. Lock host ANativeWindow backbuffer
    ANativeWindow_Buffer window_buffer{};
    ARect dirty_bounds{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
    int win_lock_res = ANativeWindow_lock(window, &window_buffer, &dirty_bounds);

    if (win_lock_res != 0 || !window_buffer.bits) {
        LOGE_DISP("Failed to lock ANativeWindow: error=%d", win_lock_res);
        AHardwareBuffer_unlock(buffer, nullptr);
        return win_lock_res;
    }

    // 4. Stride-safe blit calculation (Row-by-Row copy)
    // Avoid single-block memcpy which causes diagonal shearing when strides differ
    constexpr size_t kBytesPerPixel = 4; // RGBA_8888
    const size_t copy_width = std::min({
        static_cast<size_t>(width),
        static_cast<size_t>(desc.width),
        static_cast<size_t>(window_buffer.width)
    });
    const size_t copy_height = std::min({
        static_cast<size_t>(height),
        static_cast<size_t>(desc.height),
        static_cast<size_t>(window_buffer.height)
    });

    const size_t copy_row_bytes = copy_width * kBytesPerPixel;
    const size_t src_stride_bytes = desc.stride * kBytesPerPixel;
    const size_t dst_stride_bytes = window_buffer.stride * kBytesPerPixel;

    const auto* src_bytes = static_cast<const uint8_t*>(src_raw_pixels);
    auto* dst_bytes = static_cast<uint8_t*>(window_buffer.bits);

    for (size_t y = 0; y < copy_height; ++y) {
        memcpy(dst_bytes + (y * dst_stride_bytes),
               src_bytes + (y * src_stride_bytes),
               copy_row_bytes);
    }

    // 5. Unlock AHardwareBuffer and commit ANativeWindow presentation
    AHardwareBuffer_unlock(buffer, nullptr);
    int post_res = ANativeWindow_unlockAndPost(window);

    if (post_res != 0) {
        LOGE_DISP("ANativeWindow_unlockAndPost returned error: %d", post_res);
        return post_res;
    }

    return 0;
}

} // namespace engine::display

// ============================================================================
// C-Linkage Exported Wrappers
// ============================================================================

extern "C" {

EXPORT_SYMBOL AHardwareBuffer* display_allocate_guest_buffer(uint32_t width, uint32_t height) {
    return engine::display::allocate_guest_buffer(width, height);
}

EXPORT_SYMBOL void display_release_guest_buffer(AHardwareBuffer* buffer) {
    engine::display::release_guest_buffer(buffer);
}

EXPORT_SYMBOL int display_send_buffer_handle(const AHardwareBuffer* buffer, int socket_fd) {
    return engine::display::send_buffer_handle_to_socket(buffer, socket_fd);
}

EXPORT_SYMBOL int display_receive_buffer_handle(int socket_fd, AHardwareBuffer** out_buffer) {
    return engine::display::receive_buffer_handle_from_socket(socket_fd, out_buffer);
}

EXPORT_SYMBOL int display_render_to_surface(
    AHardwareBuffer* buffer,
    ANativeWindow* window,
    uint32_t width,
    uint32_t height
) {
    return engine::display::render_to_surface(buffer, window, width, height);
}

} // extern "C"
