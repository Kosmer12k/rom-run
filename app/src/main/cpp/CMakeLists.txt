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
    micro_init.cpp
    input_forwarder.cpp
)

target_link_libraries(engine
    android
    nativewindow
    binder_ndk
    log
    dl
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



