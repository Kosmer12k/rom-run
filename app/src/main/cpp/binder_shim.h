#ifndef ENGINE_BINDER_SHIM_H
#define ENGINE_BINDER_SHIM_H

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <cstdint>
#include <chrono>

#define BINDER_SHIM_LOG_TAG "Engine-BinderShim"

namespace engine::binder {

/**
 * Packs a strong AIBinder reference into an allocated native AParcel.
 * Caller takes ownership of *out_parcel and must free it via AParcel_delete().
 */
binder_status_t pack_guest_binder(AIBinder* binder, AParcel** out_parcel);

/**
 * Unpacks a strong AIBinder reference from a native AParcel.
 * Caller takes ownership of *out_binder and must release it via AIBinder_decStrong().
 */
binder_status_t unpack_guest_binder(const AParcel* parcel, AIBinder** out_binder);

/**
 * Registers the guest ServiceManager AIBinder instance into the native shim.
 * Increments the strong reference on the binder and notifies any waiting IPC threads.
 */
void register_guest_servicemanager(AIBinder* binder);

/**
 * Polls and waits for the guest ServiceManager AIBinder to become valid.
 * Blocks until registered or until the specified timeout expires.
 * Returns a strong reference (AIBinder*) on success, or nullptr on timeout.
 * Caller must call AIBinder_decStrong() when finished.
 */
AIBinder* get_guest_servicemanager(uint32_t timeout_ms = 10000);

/**
 * Checks if the guest ServiceManager has been successfully registered.
 */
bool is_guest_servicemanager_ready();

/**
 * Resets the registered ServiceManager token (e.g., during container shutdown).
 */
void reset_guest_servicemanager();

} // namespace engine::binder

// ============================================================================
// C-Linkage Exported API for Inter-module and JNI usage
// ============================================================================
#ifdef __cplusplus
extern "C" {
#endif

binder_status_t shim_pack_guest_binder(AIBinder* binder, AParcel** out_parcel);
binder_status_t shim_unpack_guest_binder(const AParcel* parcel, AIBinder** out_binder);
AIBinder* shim_get_guest_servicemanager();
void shim_register_guest_servicemanager(AIBinder* binder);

#ifdef __cplusplus
}
#endif

#endif // ENGINE_BINDER_SHIM_H
