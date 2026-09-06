#ifndef ENGINE_PROPERTY_SERVICE_H
#define ENGINE_PROPERTY_SERVICE_H

#include <sys/types.h>
#include <string>
#include <unordered_map>

#ifndef PROP_NAME_MAX
#define PROP_NAME_MAX 32
#endif

#ifndef PROP_VALUE_MAX
#define PROP_VALUE_MAX 92
#endif

#define PROPERTY_LOG_TAG "Engine-PropService"

// Opaque / transparent prop_info structure for Bionic compatibility
// In Bionic libc, serial is at offset 0, followed by value, then name.
struct prop_info {
    uint32_t serial{0};
    char value[PROP_VALUE_MAX]{0};
    char name[PROP_NAME_MAX]{0};
};

namespace engine::properties {

// Parse guest build.prop file into in-memory key-value store
bool load_guest_properties(const char* build_prop_path);
inline bool load_guest_properties(const std::string& p) { return load_guest_properties(p.c_str()); }

// Set property key-value pair in memory
void set_property(const char* key, const char* value);
inline void set_property(const std::string& k, const std::string& v) { set_property(k.c_str(), v.c_str()); }

// Get property value by key with optional fallback
const char* get_property_cstr(const char* key, const char* fallback = "");
inline std::string get_property(const std::string& key, const std::string& fallback = "") {
    return std::string(get_property_cstr(key.c_str(), fallback.c_str()));
}

// Seed mandatory default AOSP fallbacks
void init_default_fallbacks();

// Retrieve total number of loaded properties
size_t get_property_count();

} // namespace engine::properties

// ============================================================================
// Libc Bionic System Properties Hook Interface (Exported only in libvfs_hook.so)
// ============================================================================
#ifdef VFS_HOOK_SHARED
#ifdef __cplusplus
extern "C" {
#endif

// Explicit Hooked Functions as specified
int hooked_system_property_get(const char* name, char* value);
int hooked_system_property_read(const prop_info* pi, char* name, char* value);

// Standard Bionic libc System Property symbols (intercepted for guest processes)
int __system_property_get(const char* name, char* value);
int __system_property_set(const char* key, const char* value);
const prop_info* __system_property_find(const char* name);
int __system_property_read(const prop_info* pi, char* name, char* value);
void __system_property_read_callback(
    const prop_info* pi,
    void (*callback)(void* cookie, const char* name, const char* value, uint32_t serial),
    void* cookie
);

#ifdef __cplusplus
}
#endif
#endif // VFS_HOOK_SHARED

#endif // ENGINE_PROPERTY_SERVICE_H
