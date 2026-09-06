#include "native_bridge.h"
#include "vfs_hook.h"
#include "property_service.h"
#include "binder_shim.h"
#include "display_renderer.h"
#include "micro_init.h"
#include "input_forwarder.h"

#define EXPORT_SYMBOL __attribute__((visibility("default")))

namespace engine {

NativeWindowManager& NativeWindowManager::getInstance() {
    static NativeWindowManager instance;
    return instance;
}

NativeWindowManager::~NativeWindowManager() {
    releaseSurface();
}

void NativeWindowManager::setSurface(JNIEnv* env, jobject surface) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_window != nullptr) {
        LOGW("Releasing previous active ANativeWindow [%p]", m_window);
        ANativeWindow_release(m_window);
        m_window = nullptr;
        m_isReady.store(false, std::memory_order_release);
    }

    if (surface == nullptr) {
        LOGE("Passed null jobject surface to setSurface");
        return;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) {
        LOGE("Failed to obtain ANativeWindow from Surface jobject");
        return;
    }

    ANativeWindow_acquire(window);
    m_window = window;

    int32_t width = ANativeWindow_getWidth(window);
    int32_t height = ANativeWindow_getHeight(window);

    m_width.store(width, std::memory_order_relaxed);
    m_height.store(height, std::memory_order_relaxed);

    int32_t setRes = ANativeWindow_setBuffersGeometry(window, width, height, WINDOW_FORMAT_RGBA_8888);
    if (setRes != 0) {
        LOGW("ANativeWindow_setBuffersGeometry returned status: %d", setRes);
    }

    m_isReady.store(true, std::memory_order_release);
    LOGI("ANativeWindow initialized successfully: ptr=%p, resolution=%dx%d, format=RGBA_8888",
         m_window, width, height);
}

void NativeWindowManager::updateDimensions(int32_t width, int32_t height) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_width.store(width, std::memory_order_relaxed);
    m_height.store(height, std::memory_order_relaxed);

    if (m_window != nullptr) {
        int32_t res = ANativeWindow_setBuffersGeometry(m_window, width, height, WINDOW_FORMAT_RGBA_8888);
        LOGI("ANativeWindow dimensions updated: %dx%d (setBuffersGeometry: %d)", width, height, res);
    }
}

void NativeWindowManager::releaseSurface() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_isReady.store(false, std::memory_order_release);

    if (m_window != nullptr) {
        LOGI("Releasing active ANativeWindow [%p]", m_window);
        ANativeWindow_release(m_window);
        m_window = nullptr;
    }
    m_width.store(0, std::memory_order_relaxed);
    m_height.store(0, std::memory_order_relaxed);
}

ANativeWindow* NativeWindowManager::acquireActiveWindow() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_window != nullptr) {
        ANativeWindow_acquire(m_window);
        return m_window;
    }
    return nullptr;
}

bool NativeWindowManager::isSurfaceReady() const {
    return m_isReady.load(std::memory_order_acquire);
}

int32_t NativeWindowManager::getWidth() const {
    return m_width.load(std::memory_order_relaxed);
}

int32_t NativeWindowManager::getHeight() const {
    return m_height.load(std::memory_order_relaxed);
}

} // namespace engine

// ============================================================================
// JNI Exports
// ============================================================================

extern "C" {

EXPORT_SYMBOL void JNICALL
Java_com_engine_container_MainActivity_nativeOnSurfaceCreated(
    JNIEnv* env,
    jobject /* thiz */,
    jobject surface
) {
    LOGI("JNI: nativeOnSurfaceCreated invoked");
    engine::NativeWindowManager::getInstance().setSurface(env, surface);
}

EXPORT_SYMBOL void JNICALL
Java_com_engine_container_MainActivity_nativeOnSurfaceChanged(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jint width,
    jint height
) {
    LOGI("JNI: nativeOnSurfaceChanged invoked: %dx%d", width, height);
    engine::NativeWindowManager::getInstance().updateDimensions(width, height);
}

EXPORT_SYMBOL void JNICALL
Java_com_engine_container_MainActivity_nativeOnSurfaceDestroyed(
    JNIEnv* /* env */,
    jobject /* thiz */
) {
    LOGI("JNI: nativeOnSurfaceDestroyed invoked");
    engine::NativeWindowManager::getInstance().releaseSurface();
}

// ============================================================================
// Boot Orchestrator Integration (Tahap 8)
// ============================================================================

EXPORT_SYMBOL jint JNICALL
Java_com_engine_container_MainActivity_startContainer(
    JNIEnv* env,
    jobject /* thiz */,
    jstring jRootfsPath,
    jstring jHookSoPath
) {
    if (!jRootfsPath) {
        LOGE("startContainer: jRootfsPath argument is null");
        return -1;
    }

    const char* rootfs_chars = env->GetStringUTFChars(jRootfsPath, nullptr);
    if (!rootfs_chars) {
        LOGE("startContainer: Failed to extract rootfs path string");
        return -2;
    }
    std::string rootfs_path = rootfs_chars;
    env->ReleaseStringUTFChars(jRootfsPath, rootfs_chars);

    std::string hook_so_path = "";
    if (jHookSoPath) {
        const char* hook_chars = env->GetStringUTFChars(jHookSoPath, nullptr);
        if (hook_chars) {
            hook_so_path = hook_chars;
            env->ReleaseStringUTFChars(jHookSoPath, hook_chars);
        }
    }

    LOGI("=================================================================");
    LOGI("AOSP CONTAINER RUNTIME: BOOT ORCHESTRATION INITIATED");
    LOGI("Target Rootfs: %s", rootfs_path.c_str());
    LOGI("Hook Library:  %s", hook_so_path.c_str());
    LOGI("=================================================================");

    // Step 1: Initialize VFS Redirection & Whitelist Hooks
    engine::vfs::set_guest_rootfs_base(rootfs_path);
    LOGI("[BOOT 1/5] VFS path redirection active (pointing to %s)", rootfs_path.c_str());

    // Step 2: Initialize In-Memory Virtual System Properties
    std::string build_prop_file = rootfs_path + "/system/build.prop";
    bool prop_loaded = engine::properties::load_guest_properties(build_prop_file);
    LOGI("[BOOT 2/5] Virtual Property Service: %s (Total loaded keys: %zu)",
         prop_loaded ? "CUSTOM BUILD.PROP LOADED" : "FALLBACKS ENFORCED",
         engine::properties::get_property_count());

    // Step 3: Initialize Virtual Binder Shim State
    LOGI("[BOOT 3/5] Virtual Binder Shim ready for ServiceManager tokens");

    // Step 4: Display Pipeline & Abstract Input Socket
    engine::input::init_input_server("guest_input");
    LOGI("[BOOT 4/5] Abstract input server listening on '@guest_input'");

    // Step 5: Spawn Guest AOSP Zygote (app_process64)
    pid_t zygote_pid = engine::init::spawn_zygote(rootfs_path, hook_so_path);

    if (zygote_pid <= 0) {
        LOGE("[BOOT ABORTED] Failed to fork/execv guest Zygote process");
        return -3;
    }

    LOGI("[BOOT 5/5] GUEST AOSP ZYGOTE LAUNCHED SUCCESSFULLY (PID: %d)", zygote_pid);
    LOGI("Logcat pipe stream streaming to tag [GUEST_AOSP]");
    LOGI("=================================================================");

    return static_cast<jint>(zygote_pid);
}

EXPORT_SYMBOL void JNICALL
Java_com_engine_container_MainActivity_stopContainer(
    JNIEnv* /* env */,
    jobject /* thiz */
) {
    LOGI("Initiating container runtime shutdown sequence...");
    engine::init::terminate_zygote();
    engine::input::shutdown_input_server();
    engine::binder::reset_guest_servicemanager();
    LOGI("Container runtime shutdown complete");
}

EXPORT_SYMBOL jboolean JNICALL
Java_com_engine_container_MainActivity_isContainerRunning(
    JNIEnv* /* env */,
    jobject /* thiz */
) {
    return static_cast<jboolean>(engine::init::is_zygote_running());
}

} // extern "C"
