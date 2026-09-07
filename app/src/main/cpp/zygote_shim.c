#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <linux/android/binder.h>
#include <android/log.h>

#define LOG_TAG "ZygoteShim"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define EXPORT __attribute__((visibility("default")))

static int (*real_getsockname)(int, struct sockaddr *, socklen_t *) = NULL;
static int (*real_connect)(int, const struct sockaddr *, socklen_t) = NULL;
static int (*real_getsockopt)(int, int, int, void *, socklen_t *) = NULL;
static int (*real_prctl)(int, unsigned long, unsigned long, unsigned long, unsigned long) = NULL;
static int (*real_ioctl)(int, int, ...) = NULL;

// In-memory registry for services registered with ServiceManager
#define MAX_SERVICES 128
typedef struct {
    char name[128];
    struct flat_binder_object binder;
} RegisteredService;

static RegisteredService g_services[MAX_SERVICES];
static int g_service_count = 0;

#define APEX_HANDLE       1
#define STATS_HANDLE      2
#define DUMMY_MAX_HANDLE  16

static void init_synthetic_services() {
    // Dynamic in-memory registry: services are registered dynamically via addService
}

// Reply ring buffers
static uint8_t g_reply_payloads[16][128];
static binder_size_t g_reply_offsets[16][8];
static int g_reply_ring_idx = 0;

static void parse_string16(const uint8_t *buf, size_t max_len, char *out, size_t out_max) {
    if (max_len < 4) { out[0] = '\0'; return; }
    int32_t len = *(int32_t *)buf;
    if (len <= 0 || (size_t)(len * 2 + 4) > max_len) { out[0] = '\0'; return; }
    const uint16_t *chars = (const uint16_t *)(buf + 4);
    size_t i = 0;
    while (i < (size_t)len && i < out_max - 1) {
        out[i] = (char)chars[i];
        i++;
    }
    out[i] = '\0';
}

static void extract_service_name(const struct binder_transaction_data *tr, char *out, size_t out_max) {
    out[0] = '\0';
    if (!tr || tr->data_size < 20 || !tr->data.ptr.buffer) return;
    const uint8_t *p = (const uint8_t *)tr->data.ptr.buffer;
    for (size_t off = 0; off + 8 <= tr->data_size; off += 4) {
        int32_t len = *(int32_t *)(p + off);
        if (len > 0 && len < 64 && off + 4 + len * 2 <= tr->data_size) {
            char temp[128] = {0};
            parse_string16(p + off, tr->data_size - off, temp, sizeof(temp));
            if (strlen(temp) > 0 && strcmp(temp, "android.os.IServiceManager") != 0) {
                strncpy(out, temp, out_max - 1);
                out[out_max - 1] = '\0';
                return;
            }
        }
    }
}

EXPORT int ioctl(int fd, int request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (!real_ioctl) {
        real_ioctl = (int (*)(int, int, ...))dlsym(RTLD_NEXT, "ioctl");
    }

    // Intercept BINDER_WRITE_READ (0xc0306201)
    if ((unsigned int)request == (unsigned int)BINDER_WRITE_READ && arg != NULL) {
        init_synthetic_services();
        struct binder_write_read *bwr = (struct binder_write_read *)arg;
        if (bwr->write_size > 0 && bwr->write_buffer != 0) {
            uint8_t *wptr = (uint8_t *)bwr->write_buffer;
            uint32_t cmd = *(uint32_t *)wptr;
            if (cmd == BC_TRANSACTION) {
                struct binder_transaction_data *tr = (struct binder_transaction_data *)(wptr + 4);
                if (tr->target.handle == 0) {
                    char sname[128] = {0};
                    extract_service_name(tr, sname, sizeof(sname));
                    LOGI("ioctl: ServiceManager transaction code=%u, service='%s', data_size=%zu, offsets_size=%zu",
                         tr->code, sname, (size_t)tr->data_size, (size_t)tr->offsets_size);

                    // Check if binder object is provided in addService (code 5)
                    if (tr->code == 5 && tr->offsets_size >= sizeof(binder_size_t) && tr->data.ptr.offsets != 0) {
                        binder_size_t off = *(binder_size_t *)tr->data.ptr.offsets;
                        if (off + sizeof(struct flat_binder_object) <= tr->data_size) {
                            struct flat_binder_object *fbo = (struct flat_binder_object *)(tr->data.ptr.buffer + off);
                            if (strlen(sname) > 0) {
                                int existing = -1;
                                for (int i = 0; i < g_service_count; i++) {
                                    if (strcmp(g_services[i].name, sname) == 0) { existing = i; break; }
                                }
                                int target_idx = (existing >= 0) ? existing : g_service_count;
                                if (target_idx < MAX_SERVICES) {
                                    strncpy(g_services[target_idx].name, sname, sizeof(g_services[0].name) - 1);
                                    g_services[target_idx].binder = *fbo;
                                    if (existing < 0) g_service_count++;
                                    LOGI("ServiceManager: REGISTERED service [%s] -> type=0x%x, binder/handle=%lu (total: %d)",
                                         sname, fbo->hdr.type, (unsigned long)fbo->binder, g_service_count);
                                }
                            }
                        }
                    }

                    // Prepare successful reply in read_buffer
                    if (bwr->read_size >= sizeof(uint32_t) * 2 + sizeof(struct binder_transaction_data) + 4) {
                        uint8_t *rptr = (uint8_t *)bwr->read_buffer;
                        *(uint32_t *)rptr = BR_TRANSACTION_COMPLETE;
                        rptr += 4;
                        *(uint32_t *)rptr = BR_REPLY;
                        rptr += 4;

                        int ring_idx = (g_reply_ring_idx++) & 15;
                        uint8_t *payload = g_reply_payloads[ring_idx];
                        binder_size_t *offsets = g_reply_offsets[ring_idx];
                        memset(payload, 0, 128);
                        memset(offsets, 0, sizeof(binder_size_t) * 8);

                        struct binder_transaction_data *reply_tr = (struct binder_transaction_data *)rptr;
                        memset(reply_tr, 0, sizeof(*reply_tr));
                        reply_tr->target.ptr = 0;
                        reply_tr->cookie = tr->cookie;
                        reply_tr->code = 0;
                        reply_tr->flags = 0;

                        int found_idx = -1;
                        if (strlen(sname) > 0) {
                            for (int i = 0; i < g_service_count; i++) {
                                if (strcmp(g_services[i].name, sname) == 0) { found_idx = i; break; }
                            }
                        }

                        if (tr->code == 4 || tr->code == 2) {
                            // checkService2 (4) / getService2 (2) -> returns Service union
                            int32_t *p32 = (int32_t *)payload;
                            if (found_idx >= 0) {
                                p32[0] = 0;  // EX_NONE
                                p32[1] = 1;  // Service is_present = 1
                                p32[2] = 1;  // Tag 1 (raw IBinder)
                                p32[3] = 0;  // 4-byte padding so flat_binder_object is 8-byte aligned at offset 16!
                                struct flat_binder_object *fbo = (struct flat_binder_object *)(payload + 16);
                                *fbo = g_services[found_idx].binder;
                                offsets[0] = 16;
                                reply_tr->data_size = 16 + sizeof(struct flat_binder_object);
                                reply_tr->offsets_size = sizeof(binder_size_t);
                                reply_tr->data.ptr.buffer = (binder_uintptr_t)payload;
                                reply_tr->data.ptr.offsets = (binder_uintptr_t)offsets;
                                LOGI("ServiceManager: RETURNING registered service [%s] for code=%u", sname, tr->code);
                            } else {
                                p32[0] = 0;  // EX_NONE
                                p32[1] = 0;  // Service is_present = 0 (NULL)
                                reply_tr->data_size = 8;
                                reply_tr->offsets_size = 0;
                                reply_tr->data.ptr.buffer = (binder_uintptr_t)payload;
                                reply_tr->data.ptr.offsets = 0;
                                LOGI("ServiceManager: Service [%s] NOT FOUND -> returning null Service for code=%u", sname, tr->code);
                            }
                        } else if (tr->code == 1 || tr->code == 3) {
                            // getService (1) / checkService (3) -> returns @nullable IBinder
                            int32_t *p32 = (int32_t *)payload;
                            p32[0] = 0; // EX_NONE
                            p32[1] = 0; // 4-byte padding for 8-byte alignment of flat_binder_object at offset 8!
                            struct flat_binder_object *fbo = (struct flat_binder_object *)(payload + 8);
                            if (found_idx >= 0) {
                                *fbo = g_services[found_idx].binder;
                                LOGI("ServiceManager: RETURNING registered binder for [%s] code=%u", sname, tr->code);
                            } else {
                                memset(fbo, 0, sizeof(*fbo));
                                fbo->hdr.type = BINDER_TYPE_BINDER;
                                fbo->flags = 0x13;
                                fbo->binder = 0;
                                fbo->cookie = 0;
                                LOGI("ServiceManager: Binder for [%s] NOT FOUND -> returning null binder for code=%u", sname, tr->code);
                            }
                            offsets[0] = 8;
                            reply_tr->data_size = 8 + sizeof(struct flat_binder_object);
                            reply_tr->offsets_size = sizeof(binder_size_t);
                            reply_tr->data.ptr.buffer = (binder_uintptr_t)payload;
                            reply_tr->data.ptr.offsets = (binder_uintptr_t)offsets;
                        } else if (tr->code == 5) {
                            // addService -> void
                            int32_t *p32 = (int32_t *)payload;
                            p32[0] = 0; // EX_NONE
                            reply_tr->data_size = 4;
                            reply_tr->offsets_size = 0;
                            reply_tr->data.ptr.buffer = (binder_uintptr_t)payload;
                            reply_tr->data.ptr.offsets = 0;
                            LOGI("ServiceManager: addService [%s] -> replied EX_NONE (0)", sname);
                        } else if (tr->code == 9) {
                            // isDeclared -> boolean
                            int32_t *p32 = (int32_t *)payload;
                            p32[0] = 0; // EX_NONE
                            p32[1] = (found_idx >= 0) ? 1 : 0;
                            reply_tr->data_size = 8;
                            reply_tr->offsets_size = 0;
                            reply_tr->data.ptr.buffer = (binder_uintptr_t)payload;
                            reply_tr->data.ptr.offsets = 0;
                        } else if (tr->code == 16) {
                            // getServiceDebugInfo -> ServiceDebugInfo[] (length 0)
                            int32_t *p32 = (int32_t *)payload;
                            p32[0] = 0; // EX_NONE
                            p32[1] = 0; // count 0
                            reply_tr->data_size = 8;
                            reply_tr->offsets_size = 0;
                            reply_tr->data.ptr.buffer = (binder_uintptr_t)payload;
                            reply_tr->data.ptr.offsets = 0;
                            LOGI("ServiceManager: replied empty array for getServiceDebugInfo (code=16)");
                        } else if (tr->code == 6 || tr->code == 10 || tr->code == 12) {
                            // listServices (6), getDeclaredInstances (10), getUpdatableNames (12) -> String[] (length 0)
                            int32_t *p32 = (int32_t *)payload;
                            p32[0] = 0; // EX_NONE
                            p32[1] = 0; // length 0
                            reply_tr->data_size = 8;
                            reply_tr->offsets_size = 0;
                            reply_tr->data.ptr.buffer = (binder_uintptr_t)payload;
                            reply_tr->data.ptr.offsets = 0;
                        } else {
                            // generic success
                            int32_t *p32 = (int32_t *)payload;
                            p32[0] = 0; // EX_NONE
                            reply_tr->data_size = 4;
                            reply_tr->offsets_size = 0;
                            reply_tr->data.ptr.buffer = (binder_uintptr_t)payload;
                            reply_tr->data.ptr.offsets = 0;
                            LOGI("ServiceManager: replied generic EX_NONE (0) for code=%u", tr->code);
                        }

                        rptr += sizeof(*reply_tr);
                        bwr->write_consumed = bwr->write_size;
                        bwr->read_consumed = (rptr - (uint8_t *)bwr->read_buffer);
                        return 0;
                    }
                } else if (tr->target.handle >= 1 && tr->target.handle <= DUMMY_MAX_HANDLE) {
                    LOGI("ioctl: Synthetic service handle=%u, code=%u, data_size=%zu -> replying generic EX_NONE",
                         tr->target.handle, tr->code, (size_t)tr->data_size);
                    if (bwr->read_size >= sizeof(uint32_t) * 2 + sizeof(struct binder_transaction_data) + 4) {
                        uint8_t *rptr = (uint8_t *)bwr->read_buffer;
                        *(uint32_t *)rptr = BR_TRANSACTION_COMPLETE;
                        rptr += 4;
                        *(uint32_t *)rptr = BR_REPLY;
                        rptr += 4;

                        int ring_idx = (g_reply_ring_idx++) & 15;
                        uint8_t *payload = g_reply_payloads[ring_idx];
                        memset(payload, 0, 128);

                        struct binder_transaction_data *reply_tr = (struct binder_transaction_data *)rptr;
                        memset(reply_tr, 0, sizeof(*reply_tr));
                        reply_tr->cookie = tr->cookie;

                        int32_t *p32 = (int32_t *)payload;
                        p32[0] = 0; // EX_NONE
                        p32[1] = 0; // length 0 / null / false
                        reply_tr->data_size = 8;
                        reply_tr->offsets_size = 0;
                        reply_tr->data.ptr.buffer = (binder_uintptr_t)payload;
                        reply_tr->data.ptr.offsets = 0;

                        rptr += sizeof(*reply_tr);
                        bwr->write_consumed = bwr->write_size;
                        bwr->read_consumed = (rptr - (uint8_t *)bwr->read_buffer);
                        return 0;
                    }
                }
            }
        }
    }

    return real_ioctl(fd, request, arg);
}



EXPORT int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    if (!real_getsockopt) {
        real_getsockopt = (int (*)(int, int, int, void *, socklen_t *))dlsym(RTLD_NEXT, "getsockopt");
    }
    int res = real_getsockopt(sockfd, level, optname, optval, optlen);
    if (res == 0 && level == SOL_SOCKET && optname == SO_PEERCRED && optval && optlen && *optlen >= sizeof(struct ucred)) {
        struct ucred *cr = (struct ucred *)optval;
        LOGI("getsockopt(fd=%d, SO_PEERCRED): real uid=%d -> spoofing uid=1000", sockfd, cr->uid);
        cr->uid = 1000;
        cr->gid = 1000;
    }
    return res;
}

EXPORT int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!real_connect) {
        real_connect = (int (*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "connect");
    }
    if (addr && addr->sa_family == AF_UNIX) {
        struct sockaddr_un *sun = (struct sockaddr_un *)addr;
        if (sun->sun_path[0] != '\0' && strstr(sun->sun_path, "zygote") != NULL) {
            LOGI("connect(fd=%d, path=%s): redirecting to abstract @container_zygote", sockfd, sun->sun_path);
            struct sockaddr_un new_sun;
            memset(&new_sun, 0, sizeof(new_sun));
            new_sun.sun_family = AF_UNIX;
            new_sun.sun_path[0] = '\0';
            const char *target = "container_zygote";
            memcpy(new_sun.sun_path + 1, target, strlen(target));
            socklen_t new_len = sizeof(sa_family_t) + 1 + strlen(target);
            return real_connect(sockfd, (const struct sockaddr *)&new_sun, new_len);
        }
    }
    return real_connect(sockfd, addr, addrlen);
}

EXPORT int fs_prepare_dir(const char* path, mode_t mode, uid_t uid, gid_t gid) {
    LOGI("Intercepted fs_prepare_dir(path=%s) -> 0", path ? path : "null");
    return 0;
}

EXPORT int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    if (!real_getsockname) {
        real_getsockname = (int (*)(int, struct sockaddr *, socklen_t *))dlsym(RTLD_NEXT, "getsockname");
    }
    int res = real_getsockname(sockfd, addr, addrlen);

    if (res == 0 && addr && addr->sa_family == AF_UNIX) {
        struct sockaddr_un *sun = (struct sockaddr_un *)addr;
        if (addrlen && *addrlen <= sizeof(sa_family_t)) {
            const char *fake = "/dev/socket/zygote";
            size_t l = strlen(fake) + 1;
            memcpy(sun->sun_path, fake, l);
            *addrlen = sizeof(sa_family_t) + l;
        } else if (sun->sun_path[0] == '\0') {
            const char *fake = "/dev/socket/zygote";
            if (strstr(sun->sun_path + 1, "usap") != NULL) {
                fake = "/dev/socket/usap_pool_primary";
            }
            size_t l = strlen(fake) + 1;
            memcpy(sun->sun_path, fake, l);
            if (addrlen) *addrlen = sizeof(sa_family_t) + l;
        }
    }
    return res;
}

EXPORT int prctl(int option, ...) {
    va_list ap;
    va_start(ap, option);
    unsigned long arg2 = va_arg(ap, unsigned long);
    unsigned long arg3 = va_arg(ap, unsigned long);
    unsigned long arg4 = va_arg(ap, unsigned long);
    unsigned long arg5 = va_arg(ap, unsigned long);
    va_end(ap);

    if (!real_prctl) {
        real_prctl = (int (*)(int, unsigned long, unsigned long, unsigned long, unsigned long))dlsym(RTLD_NEXT, "prctl");
    }
    int res = real_prctl(option, arg2, arg3, arg4, arg5);
    if (res == -1 && (errno == EPERM || errno == EACCES)) {
        return 0;
    }
    return res;
}

EXPORT int android_get_control_socket(const char *name) {
    if (!name) return -1;
    char key[64];
    snprintf(key, sizeof(key), "ANDROID_SOCKET_%s", name);
    const char *val = getenv(key);
    if (val) return atoi(val);
    return -1;
}

EXPORT int unshare(int flags) { return 0; }
EXPORT int mount(const char *s, const char *t, const char *f, unsigned long m, const void *d) { return 0; }
EXPORT int umount(const char *t) { return 0; }
EXPORT int umount2(const char *t, int f) { return 0; }
EXPORT int setresuid(uid_t r, uid_t e, uid_t s) { return 0; }
EXPORT int setresgid(gid_t r, gid_t e, gid_t s) { return 0; }
EXPORT int setreuid(uid_t r, uid_t e) { return 0; }
EXPORT int setregid(gid_t r, gid_t e) { return 0; }
EXPORT int setuid(uid_t u) { return 0; }
EXPORT int setgid(gid_t g) { return 0; }
EXPORT int setgroups(size_t s, const gid_t *l) { return 0; }
EXPORT int capset(void *h, const void *d) { return 0; }

EXPORT int fstat(int fd, struct stat *buf) {
    static int (*real_fstat)(int, struct stat *) = NULL;
    if (!real_fstat) real_fstat = (int (*)(int, struct stat *))dlsym(RTLD_NEXT, "fstat");
    int res = real_fstat(fd, buf);
    if (res == 0 && buf && S_ISFIFO(buf->st_mode)) {
        buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFCHR;
    }
    return res;
}

EXPORT int fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
    static int (*real_fstatat)(int, const char *, struct stat *, int) = NULL;
    if (!real_fstatat) real_fstatat = (int (*)(int, const char *, struct stat *, int))dlsym(RTLD_NEXT, "fstatat");
    int res = real_fstatat(dirfd, pathname, buf, flags);
    if (res == 0 && buf && S_ISFIFO(buf->st_mode)) {
        buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFCHR;
    }
    return res;
}

EXPORT ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    static ssize_t (*real_readlink)(const char *, char *, size_t) = NULL;
    if (!real_readlink) real_readlink = (ssize_t (*)(const char *, char *, size_t))dlsym(RTLD_NEXT, "readlink");
    ssize_t res = real_readlink(path, buf, bufsiz);
    if (res > 0 && path && strstr(path, "/proc/self/fd/")) {
        if (buf && (strncmp(buf, "pipe:", 5) == 0 || strncmp(buf, "socket:", 7) == 0)) {
            const char *fake = "/dev/null";
            size_t l = strlen(fake);
            if (bufsiz > l) {
                memcpy(buf, fake, l + 1);
                return l;
            }
        }
    }
    return res;
}

EXPORT ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
    static ssize_t (*real_readlinkat)(int, const char *, char *, size_t) = NULL;
    if (!real_readlinkat) real_readlinkat = (ssize_t (*)(int, const char *, char *, size_t))dlsym(RTLD_NEXT, "readlinkat");
    ssize_t res = real_readlinkat(dirfd, pathname, buf, bufsiz);
    if (res > 0 && pathname && strstr(pathname, "/proc/self/fd/")) {
        if (buf && (strncmp(buf, "pipe:", 5) == 0 || strncmp(buf, "socket:", 7) == 0)) {
            const char *fake = "/dev/null";
            size_t l = strlen(fake);
            if (bufsiz > l) {
                memcpy(buf, fake, l + 1);
                return l;
            }
        }
    }
    return res;
}
