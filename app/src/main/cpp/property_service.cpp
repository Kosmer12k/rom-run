#include "property_service.h"
#include <android/log.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <pthread.h>

#define LOGD_PROP(...) __android_log_print(ANDROID_LOG_DEBUG, PROPERTY_LOG_TAG, __VA_ARGS__)
#define LOGI_PROP(...) __android_log_print(ANDROID_LOG_INFO,  PROPERTY_LOG_TAG, __VA_ARGS__)
#define LOGW_PROP(...) __android_log_print(ANDROID_LOG_WARN,  PROPERTY_LOG_TAG, __VA_ARGS__)
#define LOGE_PROP(...) __android_log_print(ANDROID_LOG_ERROR, PROPERTY_LOG_TAG, __VA_ARGS__)

#define EXPORT_SYMBOL __attribute__((visibility("default")))

namespace {

#define MAX_STATIC_PROPS 256
static prop_info g_prop_store[MAX_STATIC_PROPS];
static size_t g_prop_count = 0;
static pthread_mutex_t g_prop_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_fallbacks_initialized = false;

#ifdef VFS_HOOK_SHARED
using sys_prop_get_fn_t = int (*)(const char*, char*);
using sys_prop_set_fn_t = int (*)(const char*, const char*);
using sys_prop_find_fn_t = const prop_info* (*)(const char*);

static sys_prop_get_fn_t real_system_property_get = nullptr;
static sys_prop_set_fn_t real_system_property_set = nullptr;
static sys_prop_find_fn_t real_system_property_find = nullptr;
static pthread_once_t g_init_real_prop_once = PTHREAD_ONCE_INIT;

static void init_real_prop_service() {
    real_system_property_get = reinterpret_cast<sys_prop_get_fn_t>(dlsym(RTLD_NEXT, "__system_property_get"));
    real_system_property_set = reinterpret_cast<sys_prop_set_fn_t>(dlsym(RTLD_NEXT, "__system_property_set"));
    real_system_property_find = reinterpret_cast<sys_prop_find_fn_t>(dlsym(RTLD_NEXT, "__system_property_find"));
}

static inline void ensure_real_prop_initialized() {
    pthread_once(&g_init_real_prop_once, init_real_prop_service);
}
#endif // VFS_HOOK_SHARED

static prop_info* find_prop_locked(const char* name) {
    for (size_t i = 0; i < g_prop_count; ++i) {
        if (strcmp(g_prop_store[i].name, name) == 0) {
            return &g_prop_store[i];
        }
    }
    return nullptr;
}

static void add_or_update_prop_locked(const char* name, const char* value) {
    prop_info* existing = find_prop_locked(name);
    if (existing) {
        strncpy(existing->value, value, PROP_VALUE_MAX - 1);
        existing->value[PROP_VALUE_MAX - 1] = '\0';
        existing->serial++;
    } else if (g_prop_count < MAX_STATIC_PROPS) {
        prop_info* slot = &g_prop_store[g_prop_count++];
        slot->serial = 1;
        strncpy(slot->name, name, PROP_NAME_MAX - 1);
        slot->name[PROP_NAME_MAX - 1] = '\0';
        strncpy(slot->value, value, PROP_VALUE_MAX - 1);
        slot->value[PROP_VALUE_MAX - 1] = '\0';
    }
}

} // anonymous namespace

namespace engine::properties {

void init_default_fallbacks() {
    pthread_mutex_lock(&g_prop_mutex);
    if (g_fallbacks_initialized) {
        pthread_mutex_unlock(&g_prop_mutex);
        return;
    }

    static const char* const kMandatoryDefaults[][2] = {
        {"ro.build.version.sdk", "33"},
        {"ro.build.version.release", "13"},
        {"ro.product.cpu.abilist", "arm64-v8a"},
        {"ro.product.cpu.abilist64", "arm64-v8a"},
        {"ro.product.cpu.abilist32", "armeabi-v7a,armeabi"},
        {"ro.product.cpu.abi", "arm64-v8a"},
        {"persist.sys.timezone", "UTC"},
        {"ro.debuggable", "1"},
        {"ro.sf.lcd_density", "420"},
        {"ro.boot.hardware", "generic_arm64"},
        {"ro.hardware", "generic_arm64"},
        {"ro.arch", "arm64"},
        {"ro.build.type", "userdebug"},
        {"ro.dalvik.vm.native.bridge", "0"},
        {"dalvik.vm.isa.arm64.features", "default"},
        {"dalvik.vm.usejit", "true"},
        {"dalvik.vm.dex2oat64.enabled", "true"},
        {"ro.config.low_ram", "false"},
        {"ro.control_privapp_permissions", "enforce"},
        {"sys.boot_completed", "1"},
        {nullptr, nullptr}
    };

    for (size_t i = 0; kMandatoryDefaults[i][0] != nullptr; ++i) {
        if (!find_prop_locked(kMandatoryDefaults[i][0])) {
            add_or_update_prop_locked(kMandatoryDefaults[i][0], kMandatoryDefaults[i][1]);
        }
    }

    g_fallbacks_initialized = true;
    pthread_mutex_unlock(&g_prop_mutex);
    LOGI_PROP("Initialized default AOSP fallbacks (count: %zu)", g_prop_count);
}

bool load_guest_properties(const char* build_prop_path) {
    if (!build_prop_path) return false;
    LOGI_PROP("Loading guest build.prop from: %s", build_prop_path);
    FILE* fp = fopen(build_prop_path, "r");
    if (!fp) {
        LOGW_PROP("Failed to open %s. Using default fallbacks.", build_prop_path);
        init_default_fallbacks();
        return false;
    }

    char line[512];
    size_t loaded = 0;
    pthread_mutex_lock(&g_prop_mutex);

    while (fgets(line, sizeof(line), fp)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') continue;

        char* eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char* val = eq + 1;

        // Trim value newline
        char* end = val + strlen(val) - 1;
        while (end >= val && (*end == '\r' || *end == '\n' || *end == ' ')) {
            *end = '\0';
            end--;
        }

        if (p[0] != '\0') {
            add_or_update_prop_locked(p, val);
            loaded++;
        }
    }

    fclose(fp);
    pthread_mutex_unlock(&g_prop_mutex);
    LOGI_PROP("Loaded %zu properties from build.prop", loaded);

    init_default_fallbacks();
    return true;
}

void set_property(const char* key, const char* value) {
    if (!key) return;
    pthread_mutex_lock(&g_prop_mutex);
    add_or_update_prop_locked(key, value ? value : "");
    pthread_mutex_unlock(&g_prop_mutex);
}

const char* get_property_cstr(const char* key, const char* fallback) {
    if (!key) return fallback;
    pthread_mutex_lock(&g_prop_mutex);
    prop_info* pi = find_prop_locked(key);
    const char* result = pi ? pi->value : fallback;
    pthread_mutex_unlock(&g_prop_mutex);
    return result;
}

size_t get_property_count() {
    pthread_mutex_lock(&g_prop_mutex);
    size_t count = g_prop_count;
    pthread_mutex_unlock(&g_prop_mutex);
    return count;
}

} // namespace engine::properties

// ============================================================================
// Libc Bionic System Properties Hook Interface (Exported only in libvfs_hook.so)
// ============================================================================

#ifdef VFS_HOOK_SHARED
extern "C" {

EXPORT_SYMBOL int hooked_system_property_get(const char* name, char* value) {
    if (!name || !value) return 0;

    engine::properties::init_default_fallbacks();

    pthread_mutex_lock(&g_prop_mutex);
    prop_info* pi = find_prop_locked(name);
    if (pi) {
        size_t len = strlen(pi->value);
        if (len >= PROP_VALUE_MAX) len = PROP_VALUE_MAX - 1;
        memcpy(value, pi->value, len);
        value[len] = '\0';
        pthread_mutex_unlock(&g_prop_mutex);
        return static_cast<int>(len);
    }
    pthread_mutex_unlock(&g_prop_mutex);

    ensure_real_prop_initialized();
    if (real_system_property_get) {
        return real_system_property_get(name, value);
    }

    value[0] = '\0';
    return 0;
}

EXPORT_SYMBOL int hooked_system_property_read(const prop_info* pi, char* name, char* value) {
    if (!pi) return -1;
    if (name) {
        strncpy(name, pi->name, PROP_NAME_MAX - 1);
        name[PROP_NAME_MAX - 1] = '\0';
    }
    if (value) {
        strncpy(value, pi->value, PROP_VALUE_MAX - 1);
        value[PROP_VALUE_MAX - 1] = '\0';
    }
    return static_cast<int>(strlen(pi->value));
}

EXPORT_SYMBOL int __system_property_get(const char* name, char* value) {
    return hooked_system_property_get(name, value);
}

EXPORT_SYMBOL int __system_property_set(const char* key, const char* value) {
    if (!key) return -1;
    engine::properties::set_property(key, value ? value : "");
    return 0;
}

EXPORT_SYMBOL const prop_info* __system_property_find(const char* name) {
    if (!name) return nullptr;

    engine::properties::init_default_fallbacks();

    pthread_mutex_lock(&g_prop_mutex);
    prop_info* pi = find_prop_locked(name);
    pthread_mutex_unlock(&g_prop_mutex);
    if (pi) return pi;

    ensure_real_prop_initialized();
    if (real_system_property_find) {
        return real_system_property_find(name);
    }
    return nullptr;
}

EXPORT_SYMBOL int __system_property_read(const prop_info* pi, char* name, char* value) {
    return hooked_system_property_read(pi, name, value);
}

EXPORT_SYMBOL void __system_property_read_callback(
    const prop_info* pi,
    void (*callback)(void* cookie, const char* name, const char* value, uint32_t serial),
    void* cookie
) {
    if (!pi || !callback) return;
    callback(cookie, pi->name, pi->value, pi->serial);
}

} // extern "C"
#endif // VFS_HOOK_SHARED
