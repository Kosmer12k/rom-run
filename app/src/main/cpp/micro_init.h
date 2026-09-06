#ifndef ENGINE_MICRO_INIT_H
#define ENGINE_MICRO_INIT_H

#include <sys/types.h>
#include <string>
#include <cstdint>

#define INIT_LOG_TAG "Engine-MicroInit"

namespace engine::init {

/**
 * Prepares the execution environment variables required for AOSP Zygote:
 * ANDROID_ROOT, ANDROID_DATA, ANDROID_ART_ROOT, ANDROID_TZDATA_ROOT,
 * BOOTCLASSPATH, LD_LIBRARY_PATH, LD_PRELOAD, and PATH.
 */
void setup_guest_env(const std::string& rootfs_path, const std::string& hook_so_path = "");

/**
 * Sets up a POSIX signal handler for SIGCHLD with waitpid(..., WNOHANG)
 * to reap child processes spawned by Zygote and avoid zombie leaks.
 */
void install_sigchld_handler();

/**
 * Spawns the guest AOSP Zygote64 binary (app_process64):
 * 1. Creates a pipe() for redirecting child stdout and stderr.
 * 2. Forks the container process.
 * 3. In child: redirects fds via dup2() and calls execv() with Zygote arguments.
 * 4. In parent: launches a background reader thread streaming stdout/stderr to Logcat.
 *
 * Returns child PID on success, or -1 on failure.
 */
pid_t spawn_zygote(const std::string& rootfs_path, const std::string& hook_so_path = "");

/**
 * Sends SIGTERM then SIGKILL to terminate the guest Zygote and its process tree.
 */
void terminate_zygote();

/**
 * Checks if the guest Zygote process is currently running.
 */
bool is_zygote_running();

/**
 * Retrieves the PID of the running Zygote process.
 */
pid_t get_zygote_pid();

} // namespace engine::init

// ============================================================================
// C-Linkage Exported Wrappers
// ============================================================================
#ifdef __cplusplus
extern "C" {
#endif

pid_t init_spawn_zygote(const char* rootfs_path, const char* hook_so_path);
void init_terminate_zygote();
int init_is_zygote_running();
pid_t init_get_zygote_pid();

#ifdef __cplusplus
}
#endif

#endif // ENGINE_MICRO_INIT_H
