cmake_minimum_required(VERSION 3.22.1)

project("engine" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(
    -Wall
    -Wextra
    -Werror=return-type
    -fvisibility=default
    -O3
    -fPIC
)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Core engine shared library (Host Runtime Bridge)
add_library(engine SHARED
    native_bridge.cpp
    vfs_hook.cpp
    property_service.cpp
    binder_shim.cpp
    display_renderer.cpp
    frame_streamer.cpp
    micro_init.cpp
    input_forwarder.cpp
)

target_link_libraries(engine
    android
    nativewindow
    binder_ndk
    log
    dl
    # Note: pthread is built into libc on Android NDK, no -lpthread needed
)

# Dedicated LD_PRELOAD Hook Library for Guest Processes (Zygote, ServiceManager)
add_library(vfs_hook SHARED
    vfs_hook.cpp
    property_service.cpp
)

target_link_libraries(vfs_hook
    log
    dl
)

target_compile_definitions(vfs_hook PRIVATE VFS_HOOK_SHARED=1)
target_compile_options(vfs_hook PRIVATE -fno-plt -fno-exceptions -fno-rtti)
set_target_properties(vfs_hook PROPERTIES LINK_FLAGS "-nostdlib++ -Wl,-z,now -Wl,-z,relro")

# Standalone Zygote & SystemServer Shim Library
add_library(zygote_shim SHARED
    zygote_shim.c
)

target_link_libraries(zygote_shim
    log
    dl
)

target_compile_options(zygote_shim PRIVATE -fno-plt -fno-exceptions -fno-rtti -O3)
set_target_properties(zygote_shim PROPERTIES LINK_FLAGS "-nostdlib++ -Wl,-z,now -Wl,-z,relro")
