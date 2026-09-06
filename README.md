# rom-run

Android 13 ROM booting fully inside a container on top of an Android host (emulator/device), without root or a hypervisor.

## What this does

- Boots a **real Android 13 system_server** inside `com.engine.container`
- Uses a custom **zygote_shim** (`LD_PRELOAD`) to intercept binder IPC, syscalls, and service lookups
- Achieves `sys.boot_completed=1` with **246 Android services** running
- ARM64 native — not a simulator or dummy

## Architecture

```
Host Android (emulator-5554)
 └── com.engine.container  (our app)
      ├── zygote64  (from Android 13 ROM)
      ├── zygote    (from Android 13 ROM)
      └── system_server  ← 246 services, fully booted
```

## Key Components

| File | Description |
|------|-------------|
| `app/src/main/cpp/zygote_shim.c` | LD_PRELOAD shim: intercepts ioctl(BINDER_WRITE_READ), prctl, mount, uid/gid calls |
| `app/src/main/cpp/property_service.cpp` | Property service stub |
| `app/src/main/cpp/native_bridge.h` | Native bridge interface |
| `scripts/test_boot.sh` | Boot orchestration script |

## Status

- [x] zygote boot
- [x] system_server boot  
- [x] sys.boot_completed = 1
- [x] 246 services active
- [ ] Display output (in progress)

## How the shim works

The `zygote_shim.so` is injected via `LD_PRELOAD` and intercepts:
- `ioctl(BINDER_WRITE_READ)` → provides synthetic ServiceManager responses so `apexservice`, `stats`, etc. are found without a real binder driver
- `prctl` → silences permission errors  
- `mount/umount` → no-ops (container can't remount)
- `setuid/setgid/setgroups/capset` → no-ops
- `getsockopt(SO_PEERCRED)` → spoofs UID=1000 (system)
- `connect` → redirects zygote socket to abstract namespace

## Build

```bash
# NDK required (API 33+, arm64-v8a)
cd app
ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk

# Or via CMake
cmake -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 .
make zygote_shim
```

## Deploy

```bash
adb push libs/arm64-v8a/libzygote_shim.so /data/local/tmp/
adb shell "LD_PRELOAD=/data/local/tmp/libzygote_shim.so /system/bin/app_process64 ..."
```
