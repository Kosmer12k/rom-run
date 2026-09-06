#!/system/bin/sh
# display_bridge.sh
# Runs INSIDE Android 13 guest: captures screen frames and pushes to UNIX socket.
# Host app (frame_streamer.cpp) reads frames via abstract socket @display_bridge_sock.
#
# Frame format (per frame):
#   [4 bytes] magic   = 0xF4AMED
#   [4 bytes] width   (uint32_t LE)
#   [4 bytes] height  (uint32_t LE)
#   [4 bytes] format  = 1 (RGBA_8888)
#   [width*height*4 bytes] raw pixel data

SOCKET_NAME="display_bridge_sock"
TARGET_FPS=30
SLEEP_MS=33  # ~30fps in milliseconds

# Get display dimensions
WIDTH=$(wm size 2>/dev/null | grep -oE '[0-9]+x[0-9]+' | cut -dx -f1)
HEIGHT=$(wm size 2>/dev/null | grep -oE '[0-9]+x[0-9]+' | cut -dx -f2)

# Defaults if wm size fails
[ -z "$WIDTH"  ] && WIDTH=1080
[ -z "$HEIGHT" ] && HEIGHT=1920

echo "[DisplayBridge] Resolution: ${WIDTH}x${HEIGHT}"
echo "[DisplayBridge] Socket: @${SOCKET_NAME}"
echo "[DisplayBridge] Target FPS: ${TARGET_FPS}"

# Wait for SurfaceFlinger to be ready
until service check SurfaceFlinger | grep -q "found"; do
    echo "[DisplayBridge] Waiting for SurfaceFlinger..."
    sleep 1
done
echo "[DisplayBridge] SurfaceFlinger ready"

# Build frame header (binary, 16 bytes)
# magic=0xF4AMED=0x00F4AMED=16,042,733 in LE: ED AM F4 00
# We use printf to emit binary
MAGIC_B0=$(printf '\xed')
MAGIC_B1=$(printf '\xam')  # 0xAM = 0xAD = 173
MAGIC_B1=$(printf '\xad')
MAGIC_B2=$(printf '\xf4')
MAGIC_B3=$(printf '\x00')

# Python-based bridge (more reliable binary framing)
if command -v python3 >/dev/null 2>&1; then
    echo "[DisplayBridge] Using python3 bridge"
    python3 - <<PYEOF
import socket, struct, subprocess, time, os, sys

SOCKET_NAME = b'display_bridge_sock'
WIDTH       = ${WIDTH}
HEIGHT      = ${HEIGHT}
FPS         = ${TARGET_FPS}
FRAME_MAGIC = 0xF4AMED  # 16042733

def make_server():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    addr = b'\x00' + SOCKET_NAME
    try:
        s.bind(addr)
    except OSError as e:
        print(f'[DisplayBridge] bind failed: {e}', flush=True)
        sys.exit(1)
    s.listen(1)
    print(f'[DisplayBridge] Listening on @{SOCKET_NAME.decode()}', flush=True)
    return s

def capture_frame():
    """Run screencap and return raw RGBA bytes."""
    try:
        result = subprocess.run(
            ['/system/bin/screencap'],
            capture_output=True, timeout=5
        )
        data = result.stdout
        # screencap output: 4-byte width, 4-byte height, 4-byte format, then pixels
        if len(data) < 12:
            return None, 0, 0
        import struct
        w, h, fmt = struct.unpack_from('<III', data, 0)
        pixels = data[12:]
        expected = w * h * 4
        if len(pixels) < expected:
            return None, 0, 0
        return pixels[:expected], w, h
    except Exception as e:
        print(f'[DisplayBridge] screencap error: {e}', flush=True)
        return None, 0, 0

srv = make_server()
interval = 1.0 / FPS

while True:
    print('[DisplayBridge] Waiting for host connection...', flush=True)
    try:
        conn, _ = srv.accept()
        print('[DisplayBridge] Host connected!', flush=True)
    except Exception as e:
        print(f'[DisplayBridge] accept error: {e}', flush=True)
        time.sleep(1)
        continue

    frame_idx = 0
    try:
        while True:
            t0 = time.monotonic()

            pixels, w, h = capture_frame()
            if pixels is None:
                time.sleep(0.5)
                continue

            # Send frame header: magic, width, height, format=1(RGBA)
            header = struct.pack('<IIII', FRAME_MAGIC, w, h, 1)
            conn.sendall(header)
            conn.sendall(pixels)

            frame_idx += 1
            if frame_idx % 30 == 0:
                print(f'[DisplayBridge] Sent frame {frame_idx} ({w}x{h})', flush=True)

            elapsed = time.monotonic() - t0
            sleep_t = interval - elapsed
            if sleep_t > 0:
                time.sleep(sleep_t)

    except BrokenPipeError:
        print('[DisplayBridge] Host disconnected', flush=True)
    except Exception as e:
        print(f'[DisplayBridge] error: {e}', flush=True)
    finally:
        try:
            conn.close()
        except:
            pass

PYEOF
else
    # Fallback: shell loop with nc (slower, for debugging only)
    echo "[DisplayBridge] python3 not found, using shell fallback"
    while true; do
        screencap -p /data/local/tmp/_frame.png 2>/dev/null
        sleep 0.1
    done
fi
