#ifndef ENGINE_VFS_HOOK_H
#define ENGINE_VFS_HOOK_H

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/capability.h>
#include <string>

#define VFS_LOG_TAG "Engine-VFS"

namespace engine::vfs {

// Configure rootfs base path in memory
void set_guest_rootfs_base(const char* base_path);
const char* get_guest_rootfs_base_cstr();

inline void set_guest_rootfs_base(const std::string& base_path) { set_guest_rootfs_base(base_path.c_str()); }
inline std::string get_guest_rootfs_base() { return std::string(get_guest_rootfs_base_cstr()); }

// Path redirection core logic
const char* resolve_guest_path_buf(const char* original_path, char* out_buf, size_t out_len);
std::string resolve_guest_path(const char* original_path);

// Check if a path is on the host kernel whitelist (e.g. /dev/null, /dev/urandom)
bool is_whitelisted_host_path(const char* path);

} // namespace engine::vfs

// ============================================================================
// Low-Level Libc I/O Wrappers (Exported only in libvfs_hook.so)
// ============================================================================
#ifdef VFS_HOOK_SHARED
#ifdef __cplusplus
extern "C" {
#endif

// Explicit Hooked I/O Prototypes as specified
int hooked_openat(int dirfd, const char* pathname, int flags, mode_t mode);
int hooked_stat(const char* pathname, struct stat* statbuf);
int hooked_fstatat(int dirfd, const char* pathname, struct stat* statbuf, int flags);
ssize_t hooked_readlinkat(int dirfd, const char* pathname, char* buf, size_t bufsiz);

// Standard Libc Interposition Symbols (used when loaded via LD_PRELOAD or dynamic linker)
int open(const char* pathname, int flags, ...);
int openat(int dirfd, const char* pathname, int flags, ...);
int stat(const char* pathname, struct stat* statbuf);
int lstat(const char* pathname, struct stat* statbuf);
int fstatat(int dirfd, const char* pathname, struct stat* statbuf, int flags);
ssize_t readlink(const char* pathname, char* buf, size_t bufsiz);
ssize_t readlinkat(int dirfd, const char* pathname, char* buf, size_t bufsiz);
int access(const char* pathname, int mode);
int faccessat(int dirfd, const char* pathname, int mode, int flags);

// ============================================================================
// Fake Security Primitives (Anti-Crash Zygote Bypass)
// All return 0 (Success) to prevent Zygote abort() during privilege drops
// ============================================================================
int chown(const char* path, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
int fchownat(int dirfd, const char* pathname, uid_t owner, gid_t group, int flags);
int setuid(uid_t uid);
int setgid(gid_t gid);
int setresuid(uid_t ruid, uid_t euid, uid_t suid);
int setresgid(gid_t rgid, gid_t egid, gid_t sgid);
int setgroups(size_t size, const gid_t* list);
int capset(cap_user_header_t hdrp, const cap_user_data_t datap);
int setcon(const char* con);
int setfilecon(const char* path, const char* con);
int setexeccon(const char* con);

#ifdef __cplusplus
}
#endif
#endif // VFS_HOOK_SHARED

#endif // ENGINE_VFS_HOOK_H
