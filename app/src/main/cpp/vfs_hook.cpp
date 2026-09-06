#include "vfs_hook.h"
#include <android/log.h>
#include <dlfcn.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <pthread.h>

#define LOGD_VFS(...) __android_log_print(ANDROID_LOG_DEBUG, VFS_LOG_TAG, __VA_ARGS__)
#define LOGI_VFS(...) __android_log_print(ANDROID_LOG_INFO,  VFS_LOG_TAG, __VA_ARGS__)
#define LOGW_VFS(...) __android_log_print(ANDROID_LOG_WARN,  VFS_LOG_TAG, __VA_ARGS__)
#define LOGE_VFS(...) __android_log_print(ANDROID_LOG_ERROR, VFS_LOG_TAG, __VA_ARGS__)

#define EXPORT_SYMBOL __attribute__((visibility("default")))

namespace {

static pthread_mutex_t g_path_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_rootfs_base[512] = "/data/user/0/com.engine.container/files/rootfs";
static size_t g_rootfs_base_len = 46;
static bool g_rootfs_initialized = false;

static void ensure_rootfs_initialized() {
    if (__builtin_expect(g_rootfs_initialized, 1)) return;

    pthread_mutex_lock(&g_path_mutex);
    if (!g_rootfs_initialized) {
        const char* env_root = getenv("CONTAINER_ROOTFS");
        if (env_root && env_root[0] != '\0') {
            strncpy(g_rootfs_base, env_root, sizeof(g_rootfs_base) - 1);
            g_rootfs_base[sizeof(g_rootfs_base) - 1] = '\0';
        }
        g_rootfs_base_len = strlen(g_rootfs_base);
        while (g_rootfs_base_len > 0 && g_rootfs_base[g_rootfs_base_len - 1] == '/') {
            g_rootfs_base[--g_rootfs_base_len] = '\0';
        }
        g_rootfs_initialized = true;
    }
    pthread_mutex_unlock(&g_path_mutex);
}

#ifdef VFS_HOOK_SHARED
using openat_fn_t = int (*)(int, const char*, int, mode_t);
using stat_fn_t = int (*)(const char*, struct stat*);
using lstat_fn_t = int (*)(const char*, struct stat*);
using fstatat_fn_t = int (*)(int, const char*, struct stat*, int);
using readlinkat_fn_t = ssize_t (*)(int, const char*, char*, size_t);
using access_fn_t = int (*)(const char*, int);
using faccessat_fn_t = int (*)(int, const char*, int, int);

static openat_fn_t real_openat = nullptr;
static stat_fn_t real_stat = nullptr;
static lstat_fn_t real_lstat = nullptr;
static fstatat_fn_t real_fstatat = nullptr;
static readlinkat_fn_t real_readlinkat = nullptr;
static access_fn_t real_access = nullptr;
static faccessat_fn_t real_faccessat = nullptr;

static pthread_once_t g_init_libc_once = PTHREAD_ONCE_INIT;

static void init_real_libc() {
    real_openat = reinterpret_cast<openat_fn_t>(dlsym(RTLD_NEXT, "openat"));
    real_stat = reinterpret_cast<stat_fn_t>(dlsym(RTLD_NEXT, "stat"));
    real_lstat = reinterpret_cast<lstat_fn_t>(dlsym(RTLD_NEXT, "lstat"));
    real_fstatat = reinterpret_cast<fstatat_fn_t>(dlsym(RTLD_NEXT, "fstatat"));
    real_readlinkat = reinterpret_cast<readlinkat_fn_t>(dlsym(RTLD_NEXT, "readlinkat"));
    real_access = reinterpret_cast<access_fn_t>(dlsym(RTLD_NEXT, "access"));
    real_faccessat = reinterpret_cast<faccessat_fn_t>(dlsym(RTLD_NEXT, "faccessat"));

    if (!real_fstatat) {
        real_fstatat = reinterpret_cast<fstatat_fn_t>(dlsym(RTLD_NEXT, "fstatat64"));
    }
    ensure_rootfs_initialized();
}

static inline void ensure_libc_initialized() {
    pthread_once(&g_init_libc_once, init_real_libc);
}
#endif // VFS_HOOK_SHARED

} // anonymous namespace

namespace engine::vfs {

void set_guest_rootfs_base(const char* base_path) {
    if (!base_path) return;
    pthread_mutex_lock(&g_path_mutex);
    strncpy(g_rootfs_base, base_path, sizeof(g_rootfs_base) - 1);
    g_rootfs_base[sizeof(g_rootfs_base) - 1] = '\0';
    g_rootfs_base_len = strlen(g_rootfs_base);
    while (g_rootfs_base_len > 0 && g_rootfs_base[g_rootfs_base_len - 1] == '/') {
        g_rootfs_base[--g_rootfs_base_len] = '\0';
    }
    g_rootfs_initialized = true;
    pthread_mutex_unlock(&g_path_mutex);
    LOGI_VFS("Guest rootfs base configured: %s", g_rootfs_base);
}

const char* get_guest_rootfs_base_cstr() {
    ensure_rootfs_initialized();
    return g_rootfs_base;
}

bool is_whitelisted_host_path(const char* path) {
    if (!path) return false;

    static const char* const kExactWhitelist[] = {
        "/dev/null",
        "/dev/zero",
        "/dev/random",
        "/dev/urandom",
        "/dev/ashmem",
        "/dev/ion",
        "/dev/mali",
        "/dev/mali0",
        "/dev/kgsl-3d0",
        "/dev/pmsg0",
        nullptr
    };

    for (size_t i = 0; kExactWhitelist[i] != nullptr; ++i) {
        if (strcmp(path, kExactWhitelist[i]) == 0) {
            return true;
        }
    }

    if (strncmp(path, "/dev/socket/", 12) == 0) return true;
    if (strncmp(path, "/dev/binder", 11) == 0) return true;
    if (strncmp(path, "/proc/self/", 11) == 0) return true;
    if (strncmp(path, "/sys/kernel/", 12) == 0) return true;

    return false;
}

const char* resolve_guest_path_buf(const char* original_path, char* out_buf, size_t out_len) {
    if (!original_path || original_path[0] == '\0') {
        return original_path;
    }

    if (is_whitelisted_host_path(original_path)) {
        return original_path;
    }

    ensure_rootfs_initialized();

    // If already starts with rootfs base, do not redirect
    if (strncmp(original_path, g_rootfs_base, g_rootfs_base_len) == 0) {
        return original_path;
    }

    // Check standard partitions
    if (strncmp(original_path, "/system", 7) == 0 && (original_path[7] == '/' || original_path[7] == '\0')) {
        snprintf(out_buf, out_len, "%s%s", g_rootfs_base, original_path);
        return out_buf;
    }
    if (strncmp(original_path, "/vendor", 7) == 0 && (original_path[7] == '/' || original_path[7] == '\0')) {
        snprintf(out_buf, out_len, "%s%s", g_rootfs_base, original_path);
        return out_buf;
    }
    if (strncmp(original_path, "/apex", 5) == 0 && (original_path[5] == '/' || original_path[5] == '\0')) {
        snprintf(out_buf, out_len, "%s%s", g_rootfs_base, original_path);
        return out_buf;
    }
    if (strncmp(original_path, "/data", 5) == 0 && (original_path[5] == '/' || original_path[5] == '\0')) {
        snprintf(out_buf, out_len, "%s%s", g_rootfs_base, original_path);
        return out_buf;
    }

    // Compatibility redirects
    if (strncmp(original_path, "/bin", 4) == 0 && (original_path[4] == '/' || original_path[4] == '\0')) {
        snprintf(out_buf, out_len, "%s/system%s", g_rootfs_base, original_path);
        return out_buf;
    }
    if (strncmp(original_path, "/etc", 4) == 0 && (original_path[4] == '/' || original_path[4] == '\0')) {
        snprintf(out_buf, out_len, "%s/system%s", g_rootfs_base, original_path);
        return out_buf;
    }
    if (strcmp(original_path, "/default.prop") == 0 || strcmp(original_path, "/build.prop") == 0) {
        snprintf(out_buf, out_len, "%s/system/build.prop", g_rootfs_base);
        return out_buf;
    }

    return original_path;
}

#ifndef VFS_HOOK_SHARED
std::string resolve_guest_path(const char* original_path) {
    if (!original_path || original_path[0] == '\0') {
        return "";
    }
    char buf[1024];
    const char* resolved = resolve_guest_path_buf(original_path, buf, sizeof(buf));
    return std::string(resolved);
}
#endif

} // namespace engine::vfs

// ============================================================================
// Low-Level Libc I/O Wrappers (hooked_* implementations)
// Only exported in dedicated LD_PRELOAD library (libvfs_hook.so)
// ============================================================================

#ifdef VFS_HOOK_SHARED
extern "C" {

EXPORT_SYMBOL int hooked_openat(int dirfd, const char* pathname, int flags, mode_t mode) {
    ensure_libc_initialized();
    if (!pathname) {
        errno = EFAULT;
        return -1;
    }

    char buf[1024];
    const char* target = engine::vfs::resolve_guest_path_buf(pathname, buf, sizeof(buf));

    if (real_openat) {
        return real_openat(dirfd, target, flags, mode);
    }
    return -1;
}

EXPORT_SYMBOL int hooked_stat(const char* pathname, struct stat* statbuf) {
    ensure_libc_initialized();
    if (!pathname || !statbuf) {
        errno = EFAULT;
        return -1;
    }

    char buf[1024];
    const char* target = engine::vfs::resolve_guest_path_buf(pathname, buf, sizeof(buf));

    if (real_stat) {
        return real_stat(target, statbuf);
    }
    if (real_fstatat) {
        return real_fstatat(AT_FDCWD, target, statbuf, 0);
    }
    return -1;
}

EXPORT_SYMBOL int hooked_fstatat(int dirfd, const char* pathname, struct stat* statbuf, int flags) {
    ensure_libc_initialized();
    if (!pathname || !statbuf) {
        errno = EFAULT;
        return -1;
    }

    char buf[1024];
    const char* target = engine::vfs::resolve_guest_path_buf(pathname, buf, sizeof(buf));

    if (real_fstatat) {
        return real_fstatat(dirfd, target, statbuf, flags);
    }
    return -1;
}

EXPORT_SYMBOL ssize_t hooked_readlinkat(int dirfd, const char* pathname, char* buf, size_t bufsiz) {
    ensure_libc_initialized();
    if (!pathname || !buf) {
        errno = EFAULT;
        return -1;
    }

    char local_buf[1024];
    const char* target = engine::vfs::resolve_guest_path_buf(pathname, local_buf, sizeof(local_buf));

    if (real_readlinkat) {
        return real_readlinkat(dirfd, target, buf, bufsiz);
    }
    return -1;
}

// ============================================================================
// Standard Libc Symbol Interposition
// ============================================================================

EXPORT_SYMBOL int open(const char* pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    return hooked_openat(AT_FDCWD, pathname, flags, mode);
}

EXPORT_SYMBOL int openat(int dirfd, const char* pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    return hooked_openat(dirfd, pathname, flags, mode);
}

EXPORT_SYMBOL int stat(const char* pathname, struct stat* statbuf) {
    return hooked_stat(pathname, statbuf);
}

EXPORT_SYMBOL int lstat(const char* pathname, struct stat* statbuf) {
    ensure_libc_initialized();
    if (!pathname || !statbuf) {
        errno = EFAULT;
        return -1;
    }

    char buf[1024];
    const char* target = engine::vfs::resolve_guest_path_buf(pathname, buf, sizeof(buf));

    if (real_lstat) {
        return real_lstat(target, statbuf);
    }
    if (real_fstatat) {
        return real_fstatat(AT_FDCWD, target, statbuf, AT_SYMLINK_NOFOLLOW);
    }
    return -1;
}

EXPORT_SYMBOL int fstatat(int dirfd, const char* pathname, struct stat* statbuf, int flags) {
    return hooked_fstatat(dirfd, pathname, statbuf, flags);
}

EXPORT_SYMBOL ssize_t readlink(const char* pathname, char* buf, size_t bufsiz) {
    return hooked_readlinkat(AT_FDCWD, pathname, buf, bufsiz);
}

EXPORT_SYMBOL ssize_t readlinkat(int dirfd, const char* pathname, char* buf, size_t bufsiz) {
    return hooked_readlinkat(dirfd, pathname, buf, bufsiz);
}

EXPORT_SYMBOL int access(const char* pathname, int mode) {
    ensure_libc_initialized();
    if (!pathname) {
        errno = EFAULT;
        return -1;
    }
    char buf[1024];
    const char* target = engine::vfs::resolve_guest_path_buf(pathname, buf, sizeof(buf));

    if (real_access) {
        return real_access(target, mode);
    }
    if (real_faccessat) {
        return real_faccessat(AT_FDCWD, target, mode, 0);
    }
    return -1;
}

EXPORT_SYMBOL int faccessat(int dirfd, const char* pathname, int mode, int flags) {
    ensure_libc_initialized();
    if (!pathname) {
        errno = EFAULT;
        return -1;
    }
    char buf[1024];
    const char* target = engine::vfs::resolve_guest_path_buf(pathname, buf, sizeof(buf));

    if (real_faccessat) {
        return real_faccessat(dirfd, target, mode, flags);
    }
    return -1;
}

// ============================================================================
// Fake Security Primitives (Anti-Crash Zygote Bypass)
// All return 0 (Success) to prevent Zygote abort() during privilege drops
// ============================================================================

EXPORT_SYMBOL int chown(const char* /*path*/, uid_t /*owner*/, gid_t /*group*/) {
    return 0;
}

EXPORT_SYMBOL int fchown(int /*fd*/, uid_t /*owner*/, gid_t /*group*/) {
    return 0;
}

EXPORT_SYMBOL int fchownat(int /*dirfd*/, const char* /*pathname*/, uid_t /*owner*/, gid_t /*group*/, int /*flags*/) {
    return 0;
}

EXPORT_SYMBOL int chmod(const char* /*path*/, mode_t /*mode*/) {
    return 0;
}

EXPORT_SYMBOL int fchmod(int /*fd*/, mode_t /*mode*/) {
    return 0;
}

EXPORT_SYMBOL int fchmodat(int /*dirfd*/, const char* /*pathname*/, mode_t /*mode*/, int /*flags*/) {
    return 0;
}

EXPORT_SYMBOL int setuid(uid_t /*uid*/) {
    return 0;
}

EXPORT_SYMBOL int setgid(gid_t /*gid*/) {
    return 0;
}

EXPORT_SYMBOL int setresuid(uid_t /*ruid*/, uid_t /*euid*/, uid_t /*suid*/) {
    return 0;
}

EXPORT_SYMBOL int setresgid(gid_t /*rgid*/, gid_t /*egid*/, gid_t /*sgid*/) {
    return 0;
}

EXPORT_SYMBOL int setgroups(size_t /*size*/, const gid_t* /*list*/) {
    return 0;
}

EXPORT_SYMBOL int capset(cap_user_header_t /*hdrp*/, const cap_user_data_t /*datap*/) {
    return 0;
}

EXPORT_SYMBOL int setcon(const char* /*con*/) {
    return 0;
}

EXPORT_SYMBOL int setfilecon(const char* /*path*/, const char* /*con*/) {
    return 0;
}

EXPORT_SYMBOL int setexeccon(const char* /*con*/) {
    return 0;
}

} // extern "C"
#endif // VFS_HOOK_SHARED
