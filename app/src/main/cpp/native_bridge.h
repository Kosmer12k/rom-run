#ifndef ENGINE_NATIVE_BRIDGE_H
#define ENGINE_NATIVE_BRIDGE_H

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <mutex>
#include <atomic>
#include <cstdint>

#define LOG_TAG "Engine-Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Surface Lifecycle JNI Methods
// ============================================================================

JNIEXPORT void JNICALL
Java_com_engine_container_MainActivity_nativeOnSurfaceCreated(
    JNIEnv* env,
    jobject thiz,
    jobject surface
);

JNIEXPORT void JNICALL
Java_com_engine_container_MainActivity_nativeOnSurfaceChanged(
    JNIEnv* env,
    jobject thiz,
    jint width,
    jint height
);

JNIEXPORT void JNICALL
Java_com_engine_container_MainActivity_nativeOnSurfaceDestroyed(
    JNIEnv* env,
    jobject thiz
);

// ============================================================================
// Boot Orchestration & Container Lifecycle JNI Methods
// ============================================================================

JNIEXPORT jint JNICALL
Java_com_engine_container_MainActivity_startContainer(
    JNIEnv* env,
    jobject thiz,
    jstring jRootfsPath,
    jstring jHookSoPath
);

JNIEXPORT void JNICALL
Java_com_engine_container_MainActivity_stopContainer(
    JNIEnv* env,
    jobject thiz
);

JNIEXPORT jboolean JNICALL
Java_com_engine_container_MainActivity_isContainerRunning(
    JNIEnv* env,
    jobject thiz
);

#ifdef __cplusplus
}
#endif

namespace engine {

// Thread-safe native window management
class NativeWindowManager {
public:
    static NativeWindowManager& getInstance();

    void setSurface(JNIEnv* env, jobject surface);
    void updateDimensions(int32_t width, int32_t height);
    void releaseSurface();

    ANativeWindow* acquireActiveWindow();

    bool isSurfaceReady() const;
    int32_t getWidth() const;
    int32_t getHeight() const;

private:
    NativeWindowManager() = default;
    ~NativeWindowManager();
    NativeWindowManager(const NativeWindowManager&) = delete;
    NativeWindowManager& operator=(const NativeWindowManager&) = delete;

    mutable std::mutex m_mutex;
    ANativeWindow* m_window{nullptr};
    std::atomic<int32_t> m_width{0};
    std::atomic<int32_t> m_height{0};
    std::atomic<bool> m_isReady{false};
};

} // namespace engine

#endif // ENGINE_NATIVE_BRIDGE_H
