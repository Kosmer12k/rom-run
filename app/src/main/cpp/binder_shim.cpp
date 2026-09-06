#include "binder_shim.h"
#include <android/log.h>
#include <dlfcn.h>
#include <mutex>
#include <condition_variable>
#include <chrono>

#define LOGD_BINDER(...) __android_log_print(ANDROID_LOG_DEBUG, BINDER_SHIM_LOG_TAG, __VA_ARGS__)
#define LOGI_BINDER(...) __android_log_print(ANDROID_LOG_INFO,  BINDER_SHIM_LOG_TAG, __VA_ARGS__)
#define LOGW_BINDER(...) __android_log_print(ANDROID_LOG_WARN,  BINDER_SHIM_LOG_TAG, __VA_ARGS__)
#define LOGE_BINDER(...) __android_log_print(ANDROID_LOG_ERROR, BINDER_SHIM_LOG_TAG, __VA_ARGS__)

#define EXPORT_SYMBOL __attribute__((visibility("default")))

namespace {

// Function pointer for dynamically resolving AParcel_create across API levels
using AParcel_create_fn = AParcel* (*)();
AParcel_create_fn g_fn_AParcel_create = nullptr;
std::once_flag g_init_parcel_create_once;

void init_parcel_create_fn() {
    g_fn_AParcel_create = reinterpret_cast<AParcel_create_fn>(
        dlsym(RTLD_DEFAULT, "AParcel_create")
    );
}

// Global thread-safe state for Guest ServiceManager reference
std::mutex g_sm_mutex;
std::condition_variable g_sm_cv;
AIBinder* g_guest_sm_binder = nullptr;

} // anonymous namespace

namespace engine::binder {

binder_status_t pack_guest_binder(AIBinder* binder, AParcel** out_parcel) {
    if (!out_parcel) {
        LOGE_BINDER("pack_guest_binder: out_parcel pointer is null");
        return STATUS_BAD_VALUE;
    }

    if (!binder) {
        LOGW_BINDER("pack_guest_binder: packing null AIBinder reference");
    }

    std::call_once(g_init_parcel_create_once, init_parcel_create_fn);

    AParcel* parcel = nullptr;
    if (g_fn_AParcel_create) {
        parcel = g_fn_AParcel_create();
    } else {
        // Fallback for API 29-30 if AParcel_create symbol is unavailable:
        // Transactions are prepared through AIBinder_prepareTransaction
        if (binder) {
            binder_status_t prep_status = AIBinder_prepareTransaction(binder, &parcel);
            if (prep_status != STATUS_OK) {
                LOGE_BINDER("pack_guest_binder: AIBinder_prepareTransaction failed: %d", prep_status);
                *out_parcel = nullptr;
                return prep_status;
            }
        }
    }

    if (!parcel) {
        LOGE_BINDER("pack_guest_binder: failed to allocate AParcel");
        *out_parcel = nullptr;
        return STATUS_NO_MEMORY;
    }

    binder_status_t status = AParcel_writeStrongBinder(parcel, binder);
    if (status != STATUS_OK) {
        LOGE_BINDER("pack_guest_binder: AParcel_writeStrongBinder error: %d", status);
        AParcel_delete(parcel);
        *out_parcel = nullptr;
        return status;
    }

    *out_parcel = parcel;
    LOGD_BINDER("pack_guest_binder: successfully packed AIBinder [%p] into AParcel [%p]",
                binder, parcel);
    return STATUS_OK;
}

binder_status_t unpack_guest_binder(const AParcel* parcel, AIBinder** out_binder) {
    if (!parcel || !out_binder) {
        LOGE_BINDER("unpack_guest_binder: invalid arguments (parcel=%p, out_binder=%p)",
                    parcel, out_binder);
        return STATUS_BAD_VALUE;
    }

    binder_status_t status = AParcel_readStrongBinder(parcel, out_binder);
    if (status != STATUS_OK) {
        LOGE_BINDER("unpack_guest_binder: AParcel_readStrongBinder error: %d", status);
        *out_binder = nullptr;
        return status;
    }

    LOGD_BINDER("unpack_guest_binder: successfully unpacked AIBinder [%p] from AParcel [%p]",
                *out_binder, parcel);
    return STATUS_OK;
}

void register_guest_servicemanager(AIBinder* binder) {
    std::lock_guard<std::mutex> lock(g_sm_mutex);

    if (g_guest_sm_binder != nullptr) {
        LOGW_BINDER("Replacing existing guest ServiceManager [%p] with [%p]",
                    g_guest_sm_binder, binder);
        AIBinder_decStrong(g_guest_sm_binder);
        g_guest_sm_binder = nullptr;
    }

    if (binder != nullptr) {
        AIBinder_incStrong(binder);
        g_guest_sm_binder = binder;
        LOGI_BINDER("Guest ServiceManager registered successfully [%p]", g_guest_sm_binder);
    } else {
        LOGW_BINDER("Guest ServiceManager registered as nullptr");
    }

    g_sm_cv.notify_all();
}

AIBinder* get_guest_servicemanager(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(g_sm_mutex);

    if (g_guest_sm_binder == nullptr) {
        LOGI_BINDER("Waiting up to %u ms for guest ServiceManager to be registered...", timeout_ms);
        bool ready = g_sm_cv.wait_for(
            lock,
            std::chrono::milliseconds(timeout_ms),
            []() { return g_guest_sm_binder != nullptr; }
        );

        if (!ready || g_guest_sm_binder == nullptr) {
            LOGW_BINDER("Timeout waiting for guest ServiceManager (%u ms elapsed)", timeout_ms);
            return nullptr;
        }
    }

    // Acquire an extra strong ref for the caller
    AIBinder_incStrong(g_guest_sm_binder);
    LOGD_BINDER("get_guest_servicemanager: Returning strong ref to [%p]", g_guest_sm_binder);
    return g_guest_sm_binder;
}

bool is_guest_servicemanager_ready() {
    std::lock_guard<std::mutex> lock(g_sm_mutex);
    return g_guest_sm_binder != nullptr;
}

void reset_guest_servicemanager() {
    std::lock_guard<std::mutex> lock(g_sm_mutex);
    if (g_guest_sm_binder != nullptr) {
        LOGI_BINDER("Resetting guest ServiceManager [%p]", g_guest_sm_binder);
        AIBinder_decStrong(g_guest_sm_binder);
        g_guest_sm_binder = nullptr;
    }
}

} // namespace engine::binder

// ============================================================================
// C-Linkage Exported Wrappers
// ============================================================================

extern "C" {

EXPORT_SYMBOL binder_status_t shim_pack_guest_binder(AIBinder* binder, AParcel** out_parcel) {
    return engine::binder::pack_guest_binder(binder, out_parcel);
}

EXPORT_SYMBOL binder_status_t shim_unpack_guest_binder(const AParcel* parcel, AIBinder** out_binder) {
    return engine::binder::unpack_guest_binder(parcel, out_binder);
}

EXPORT_SYMBOL AIBinder* shim_get_guest_servicemanager() {
    return engine::binder::get_guest_servicemanager(10000);
}

EXPORT_SYMBOL void shim_register_guest_servicemanager(AIBinder* binder) {
    engine::binder::register_guest_servicemanager(binder);
}

} // extern "C"
