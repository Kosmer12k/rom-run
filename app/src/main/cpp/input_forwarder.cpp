#include "input_forwarder.h"
#include <android/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>

#define LOGD_INP(...) __android_log_print(ANDROID_LOG_DEBUG, INPUT_LOG_TAG, __VA_ARGS__)
#define LOGI_INP(...) __android_log_print(ANDROID_LOG_INFO,  INPUT_LOG_TAG, __VA_ARGS__)
#define LOGW_INP(...) __android_log_print(ANDROID_LOG_WARN,  INPUT_LOG_TAG, __VA_ARGS__)
#define LOGE_INP(...) __android_log_print(ANDROID_LOG_ERROR, INPUT_LOG_TAG, __VA_ARGS__)

#define EXPORT_SYMBOL __attribute__((visibility("default")))

namespace {

std::atomic<int> g_server_fd{-1};
std::atomic<bool> g_is_running{false};
std::mutex g_clients_mutex;
std::vector<int> g_connected_clients;
std::thread g_accept_thread;

// Accept worker thread for incoming guest input consumer connections
void accept_loop(int server_fd) {
    LOGI_INP("Input server accept loop active on fd=%d", server_fd);

    while (g_is_running.load(std::memory_order_acquire)) {
        struct sockaddr_un client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (!g_is_running.load(std::memory_order_acquire)) {
                break;
            }
            LOGE_INP("Input server accept() error: %s (errno=%d)", strerror(errno), errno);
            usleep(50000); // 50ms pause on error
            continue;
        }

        // Set non-blocking mode on client socket to prevent UI lockups
        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        }

        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            g_connected_clients.push_back(client_fd);
            LOGI_INP("New guest input consumer connected (client_fd=%d, total active=%zu)",
                     client_fd, g_connected_clients.size());
        }
    }

    LOGI_INP("Input server accept loop terminated");
}

} // anonymous namespace

namespace engine::input {

bool init_input_server(const std::string& socket_name) {
    if (g_is_running.load(std::memory_order_acquire)) {
        LOGW_INP("Input server already running");
        return true;
    }

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOGE_INP("Failed to create AF_UNIX socket: %s", strerror(errno));
        return false;
    }

    // Configure Linux Abstract Namespace Socket (leading \0 byte)
    // Abstract sockets bypass filesystem permissions in untrusted_app sandbox
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0'; // Abstract namespace marker

    size_t name_len = std::min(socket_name.length(), sizeof(addr.sun_path) - 2);
    memcpy(addr.sun_path + 1, socket_name.c_str(), name_len);

    socklen_t addr_len = sizeof(sa_family_t) + 1 + name_len;

    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), addr_len) != 0) {
        LOGE_INP("Failed to bind abstract input socket '@%s': %s", socket_name.c_str(), strerror(errno));
        close(server_fd);
        return false;
    }

    if (listen(server_fd, 8) != 0) {
        LOGE_INP("Failed to listen on input socket: %s", strerror(errno));
        close(server_fd);
        return false;
    }

    g_server_fd.store(server_fd, std::memory_order_release);
    g_is_running.store(true, std::memory_order_release);

    LOGI_INP("Input server listening on abstract socket '@%s' (fd=%d)", socket_name.c_str(), server_fd);

    g_accept_thread = std::thread(accept_loop, server_fd);
    return true;
}

void shutdown_input_server() {
    g_is_running.store(false, std::memory_order_release);

    int s_fd = g_server_fd.exchange(-1, std::memory_order_release);
    if (s_fd >= 0) {
        shutdown(s_fd, SHUT_RDWR);
        close(s_fd);
    }

    if (g_accept_thread.joinable()) {
        g_accept_thread.join();
    }

    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for (int client_fd : g_connected_clients) {
        if (client_fd >= 0) {
            close(client_fd);
        }
    }
    g_connected_clients.clear();
    LOGI_INP("Input server successfully shut down");
}

bool dispatch_touch_event(
    int32_t action,
    int32_t pointer_id,
    float x,
    float y,
    float pressure,
    int64_t timestamp
) {
    InputEventPacket packet{};
    packet.action = action;
    packet.pointer_id = pointer_id;
    packet.x = x;
    packet.y = y;
    packet.pressure = pressure;
    packet.timestamp = timestamp;

    std::lock_guard<std::mutex> lock(g_clients_mutex);
    if (g_connected_clients.empty()) {
        // No client currently attached to socket, return without error
        return false;
    }

    bool any_success = false;
    std::vector<int> dead_clients;

    for (int client_fd : g_connected_clients) {
        ssize_t sent = send(client_fd, &packet, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent == sizeof(packet)) {
            any_success = true;
        } else if (sent < 0 && (errno == EPIPE || errno == ECONNRESET || errno == EBADF)) {
            dead_clients.push_back(client_fd);
        }
    }

    // Prune dead client connections
    if (!dead_clients.empty()) {
        for (int dead_fd : dead_clients) {
            close(dead_fd);
            g_connected_clients.erase(
                std::remove(g_connected_clients.begin(), g_connected_clients.end(), dead_fd),
                g_connected_clients.end()
            );
            LOGI_INP("Pruned dead input consumer (fd=%d, remaining=%zu)", dead_fd, g_connected_clients.size());
        }
    }

    return any_success;
}

bool has_active_client() {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    return !g_connected_clients.empty();
}

} // namespace engine::input

// ============================================================================
// JNI Implementation
// ============================================================================

extern "C" {

EXPORT_SYMBOL jboolean JNICALL
Java_com_engine_container_MainActivity_sendTouchEvent(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jint action,
    jint pointerId,
    jfloat x,
    jfloat y,
    jfloat pressure,
    jlong timestamp
) {
    bool res = engine::input::dispatch_touch_event(
        static_cast<int32_t>(action),
        static_cast<int32_t>(pointerId),
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<float>(pressure),
        static_cast<int64_t>(timestamp)
    );
    return static_cast<jboolean>(res);
}

EXPORT_SYMBOL jboolean JNICALL
Java_com_engine_container_MainActivity_nativeInitInputServer(
    JNIEnv* env,
    jobject /* thiz */,
    jstring jSocketName
) {
    std::string socket_name = "guest_input";
    if (jSocketName != nullptr) {
        const char* c_str = env->GetStringUTFChars(jSocketName, nullptr);
        if (c_str != nullptr) {
            socket_name = c_str;
            env->ReleaseStringUTFChars(jSocketName, c_str);
        }
    }

    bool res = engine::input::init_input_server(socket_name);
    return static_cast<jboolean>(res);
}

} // extern "C"
