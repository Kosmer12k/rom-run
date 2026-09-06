#include "micro_init.h"
#include <android/log.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>

#define LOGD_INIT(...) __android_log_print(ANDROID_LOG_DEBUG, INIT_LOG_TAG, __VA_ARGS__)
#define LOGI_INIT(...) __android_log_print(ANDROID_LOG_INFO,  INIT_LOG_TAG, __VA_ARGS__)
#define LOGW_INIT(...) __android_log_print(ANDROID_LOG_WARN,  INIT_LOG_TAG, __VA_ARGS__)
#define LOGE_INIT(...) __android_log_print(ANDROID_LOG_ERROR, INIT_LOG_TAG, __VA_ARGS__)

#define EXPORT_SYMBOL __attribute__((visibility("default")))

namespace {

std::atomic<pid_t> g_zygote_pid{-1};
std::atomic<bool> g_is_running{false};
std::once_flag g_sigchld_once;

void sigchld_signal_handler(int /* sig */, siginfo_t* /* info */, void* /* ucontext */) {
    int status;
    pid_t reaped_pid;
    while ((reaped_pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (reaped_pid == g_zygote_pid.load(std::memory_order_relaxed)) {
            g_is_running.store(false, std::memory_order_release);
            g_zygote_pid.store(-1, std::memory_order_release);

            if (WIFEXITED(status)) {
                LOGW_INIT("Guest Zygote (PID %d) terminated normally with exit code: %d",
                          reaped_pid, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                LOGE_INIT("Guest Zygote (PID %d) killed by signal: %d (%s)",
                          reaped_pid, WTERMSIG(status), strsignal(WTERMSIG(status)));
            }
        } else {
            LOGD_INIT("Reaped child process %d (status=%d)", reaped_pid, status);
        }
    }
}

// Background thread loop to read stdout/stderr from child pipe and stream to Logcat
void run_logcat_reader(int read_fd) {
    LOGI_INIT("Logcat pipe reader thread started on fd=%d", read_fd);
    char buffer[2048];
    std::string line_accumulator;

    while (true) {
        ssize_t bytes_read = read(read_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read <= 0) {
            if (bytes_read < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;
            }
            // EOF or error
            break;
        }

        buffer[bytes_read] = '\0';
        line_accumulator.append(buffer, bytes_read);

        size_t newline_pos;
        while ((newline_pos = line_accumulator.find('\n')) != std::string::npos) {
            std::string line = line_accumulator.substr(0, newline_pos);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            __android_log_print(ANDROID_LOG_DEBUG, "GUEST_AOSP", "%s", line.c_str());
            line_accumulator.erase(0, newline_pos + 1);
        }
    }

    if (!line_accumulator.empty()) {
        __android_log_print(ANDROID_LOG_DEBUG, "GUEST_AOSP", "%s", line_accumulator.c_str());
    }

    close(read_fd);
    LOGI_INIT("Logcat pipe reader thread closed");
}

} // anonymous namespace

namespace engine::init {

void setup_guest_env(const std::string& rootfs_path, const std::string& hook_so_path) {
    LOGI_INIT("Setting up guest environment variables (rootfs=%s)", rootfs_path.c_str());

    // Ensure essential dalvik-cache directory exists
    std::string cache_dir = rootfs_path + "/data/dalvik-cache/arm64";
    mkdir((rootfs_path + "/data").c_str(), 0777);
    mkdir((rootfs_path + "/data/dalvik-cache").c_str(), 0777);
    mkdir(cache_dir.c_str(), 0777);

    // 1. AOSP partition roots & Container Rootfs
    setenv("CONTAINER_ROOTFS", rootfs_path.c_str(), 1);
    setenv("ANDROID_ROOT", (rootfs_path + "/system").c_str(), 1);
    setenv("ANDROID_DATA", (rootfs_path + "/data").c_str(), 1);
    setenv("ANDROID_ART_ROOT", (rootfs_path + "/apex/com.android.art").c_str(), 1);
    setenv("ANDROID_TZDATA_ROOT", (rootfs_path + "/apex/com.android.tzdata").c_str(), 1);
    setenv("ANDROID_I18N_ROOT", (rootfs_path + "/apex/com.android.i18n").c_str(), 1);

    // 2. Android 13 DEX2OATBOOTCLASSPATH and BOOTCLASSPATH jars
    std::string dex2oat_bcp =
        rootfs_path + "/apex/com.android.art/javalib/core-oj.jar:" +
        rootfs_path + "/apex/com.android.art/javalib/core-libart.jar:" +
        rootfs_path + "/apex/com.android.art/javalib/okhttp.jar:" +
        rootfs_path + "/apex/com.android.art/javalib/bouncycastle.jar:" +
        rootfs_path + "/apex/com.android.art/javalib/apache-xml.jar";
    setenv("DEX2OATBOOTCLASSPATH", dex2oat_bcp.c_str(), 1);

    std::string bcp = dex2oat_bcp + ":" +
        rootfs_path + "/apex/com.android.i18n/javalib/core-icu4j.jar:" +
        rootfs_path + "/system/framework/framework.jar:" +
        rootfs_path + "/system/framework/framework-graphics.jar:" +
        rootfs_path + "/system/framework/ext.jar:" +
        rootfs_path + "/system/framework/telephony-common.jar:" +
        rootfs_path + "/system/framework/voip-common.jar:" +
        rootfs_path + "/system/framework/ims-common.jar";
    setenv("BOOTCLASSPATH", bcp.c_str(), 1);

    // 3. Dynamic linker and runtime library search paths
    // Check if running on an x86 host using binfmt_misc / houdini64
    bool is_houdini_host = (access("/system/bin/houdini64", F_OK) == 0);
    if (!is_houdini_host) {
        std::string ld_lib_path =
            rootfs_path + "/apex/com.android.art/lib64:" +
            rootfs_path + "/apex/com.android.runtime/lib64/bionic:" +
            rootfs_path + "/system/lib64:" +
            rootfs_path + "/vendor/lib64";
        setenv("LD_LIBRARY_PATH", ld_lib_path.c_str(), 1);
    } else {
        // On Houdini hosts, unset host LD_LIBRARY_PATH to avoid x86 linker loading ARM64 libs
        unsetenv("LD_LIBRARY_PATH");
    }

    // 4. Executable execution PATH
    std::string path_env =
        rootfs_path + "/system/bin:" +
        rootfs_path + "/apex/com.android.runtime/bin:" +
        rootfs_path + "/apex/com.android.art/bin";
    setenv("PATH", path_env.c_str(), 1);

    // 5. Injected VFS & Fake Permission Hook Library (if provided)
    if (!hook_so_path.empty()) {
        if (is_houdini_host) {
            unsetenv("LD_PRELOAD");
            setenv("ARM_LD_PRELOAD", hook_so_path.c_str(), 1);
            LOGI_INIT("Configured ARM_LD_PRELOAD=%s (Houdini translation detected)", hook_so_path.c_str());
        } else {
            setenv("LD_PRELOAD", hook_so_path.c_str(), 1);
            LOGI_INIT("Configured LD_PRELOAD=%s (Native ARM64 host)", hook_so_path.c_str());
        }
    }
}

void install_sigchld_handler() {
    std::call_once(g_sigchld_once, []() {
        struct sigaction sa{};
        sa.sa_sigaction = sigchld_signal_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_NOCLDSTOP;
        sigemptyset(&sa.sa_mask);

        if (sigaction(SIGCHLD, &sa, nullptr) != 0) {
            LOGE_INIT("Failed to install SIGCHLD handler: %s", strerror(errno));
        } else {
            LOGI_INIT("SIGCHLD handler registered successfully");
        }
    });
}

pid_t spawn_zygote(const std::string& rootfs_path, const std::string& hook_so_path) {
    if (g_is_running.load(std::memory_order_acquire)) {
        LOGW_INIT("spawn_zygote: Zygote is already running with PID %d", g_zygote_pid.load());
        return g_zygote_pid.load();
    }

    install_sigchld_handler();

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        LOGE_INIT("Failed to create stdout/stderr pipe: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOGE_INIT("fork() failed to create child process: %s", strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    if (pid == 0) {
        // ====================================================================
        // Child Process (Guest Zygote Context)
        // ====================================================================
        close(pipe_fds[0]); // Close unused read end

        // Duplicate write end of pipe to stdout and stderr
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);

        // Setup environment variables in child memory
        setup_guest_env(rootfs_path, hook_so_path);

        // Path to target AOSP app_process64 binary
        std::string binary_path = rootfs_path + "/system/bin/app_process64";

        // Standard AOSP Zygote initialization arguments
        char* const argv[] = {
            const_cast<char*>("zygote64"),
            const_cast<char*>("/system/bin"),
            const_cast<char*>("--zygote"),
            const_cast<char*>("--start-system-server"),
            nullptr
        };

        // Execute guest Zygote binary
        execv(binary_path.c_str(), argv);

        // If execv returns, an error occurred
        fprintf(stderr, "[FATAL] execv(%s) failed: %s (errno=%d)\n",
                binary_path.c_str(), strerror(errno), errno);
        fflush(stderr);
        _exit(errno);
    }

    // ========================================================================
    // Parent Process (Container Host Supervisor)
    // ========================================================================
    close(pipe_fds[1]); // Close unused write end in parent

    g_zygote_pid.store(pid, std::memory_order_release);
    g_is_running.store(true, std::memory_order_release);

    LOGI_INIT("Spawned guest Zygote process successfully (PID: %d)", pid);

    // Launch detached background reader thread for Logcat pipe streaming
    int read_fd = pipe_fds[0];
    std::thread log_thread(run_logcat_reader, read_fd);
    log_thread.detach();

    return pid;
}

void terminate_zygote() {
    pid_t pid = g_zygote_pid.load(std::memory_order_acquire);
    if (pid <= 0) {
        LOGD_INIT("terminate_zygote: No active Zygote process to terminate");
        return;
    }

    LOGI_INIT("Terminating guest Zygote (PID: %d)...", pid);
    kill(pid, SIGTERM);

    // Wait up to 1 second for graceful termination
    for (int i = 0; i < 10; ++i) {
        if (!g_is_running.load(std::memory_order_acquire)) {
            LOGI_INIT("Zygote terminated gracefully");
            return;
        }
        usleep(100000); // 100ms
    }

    // Force kill if still unresponsive
    if (g_is_running.load(std::memory_order_acquire)) {
        LOGW_INIT("Zygote unresponsive to SIGTERM. Sending SIGKILL to PID %d", pid);
        kill(pid, SIGKILL);
    }
}

bool is_zygote_running() {
    return g_is_running.load(std::memory_order_acquire);
}

pid_t get_zygote_pid() {
    return g_zygote_pid.load(std::memory_order_acquire);
}

} // namespace engine::init

// ============================================================================
// C-Linkage Exported Wrappers
// ============================================================================

extern "C" {

EXPORT_SYMBOL pid_t init_spawn_zygote(const char* rootfs_path, const char* hook_so_path) {
    if (!rootfs_path) return -1;
    return engine::init::spawn_zygote(
        rootfs_path,
        hook_so_path ? hook_so_path : ""
    );
}

EXPORT_SYMBOL void init_terminate_zygote() {
    engine::init::terminate_zygote();
}

EXPORT_SYMBOL int init_is_zygote_running() {
    return engine::init::is_zygote_running() ? 1 : 0;
}

EXPORT_SYMBOL pid_t init_get_zygote_pid() {
    return engine::init::get_zygote_pid();
}

} // extern "C"
