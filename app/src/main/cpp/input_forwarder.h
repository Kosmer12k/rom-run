#ifndef ENGINE_INPUT_FORWARDER_H
#define ENGINE_INPUT_FORWARDER_H

#include <jni.h>
#include <sys/types.h>
#include <cstdint>
#include <string>

#define INPUT_LOG_TAG "Engine-Input"

#pragma pack(push, 1)
// Binary protocol packet for zero-latency touch event forwarding
struct InputEventPacket {
    int32_t action;     // ACTION_DOWN=0, ACTION_UP=1, ACTION_MOVE=2, ACTION_CANCEL=3, ACTION_POINTER_DOWN=5, ACTION_POINTER_UP=6
    int32_t pointer_id; // Pointer identifier
    float x;            // X coordinate in surface pixels
    float y;            // Y coordinate in surface pixels
    float pressure;     // Touch contact pressure (0.0f - 1.0f)
    int64_t timestamp;  // Hardware event timestamp in ms
};
#pragma pack(pop)

namespace engine::input {

/**
 * Initializes the Unix domain input socket server (using Linux abstract namespace
 * "\0guest_input" to bypass untrusted_app filesystem permission boundaries).
 */
bool init_input_server(const std::string& socket_name = "guest_input");

/**
 * Shuts down the input server and terminates client connections.
 */
void shutdown_input_server();

/**
 * Dispatches an input event packet to all connected guest input consumers.
 * Non-blocking dispatch to ensure zero impact on the host UI thread.
 */
bool dispatch_touch_event(
    int32_t action,
    int32_t pointer_id,
    float x,
    float y,
    float pressure,
    int64_t timestamp
);

/**
 * Returns true if at least one guest input listener is currently connected.
 */
bool has_active_client();

} // namespace engine::input

// ============================================================================
// JNI Exported Functions
// ============================================================================
#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jboolean JNICALL
Java_com_engine_container_MainActivity_sendTouchEvent(
    JNIEnv* env,
    jobject thiz,
    jint action,
    jint pointerId,
    jfloat x,
    jfloat y,
    jfloat pressure,
    jlong timestamp
);

JNIEXPORT jboolean JNICALL
Java_com_engine_container_MainActivity_nativeInitInputServer(
    JNIEnv* env,
    jobject thiz,
    jstring jSocketName
);

#ifdef __cplusplus
}
#endif

#endif // ENGINE_INPUT_FORWARDER_H
