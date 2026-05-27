#!/bin/bash
set -euo pipefail

# =========================================================
# Flash script for demo tag
# - Syncs the demo build folder from server
# - Uses ESP-IDF-generated flash args when available
# - Falls back to manual offsets if needed
# =========================================================

SERVER="${SERVER:-nelsonserver@intellidwell}"
REMOTE_BASE="${REMOTE_BASE:-/var/www/esp-idf-codespace/work/tag/tag}"
LOCAL_BASE="${LOCAL_BASE:-$HOME/esp32-firmware}"

PORT="${1:-/dev/ttyACM0}"
BAUD="${BAUD:-460800}"
MONITOR_BAUD="${MONITOR_BAUD:-115200}"
ESPTOOL="${ESPTOOL:-$HOME/.local/bin/esptool.py}"

REMOTE_BUILD="$REMOTE_BASE/build-demo"
LOCAL_BUILD="$LOCAL_BASE/demo"
CHIP="esp32c3"

die() {
    echo "ERROR: $*" >&2
    exit 1
}

have_cmd() {
    command -v "$1" >/dev/null 2>&1
}

echo "=== Checking prerequisites ==="
have_cmd rsync || die "rsync not found"
have_cmd ssh || die "ssh not found"
have_cmd screen || die "screen not found"
[ -x "$ESPTOOL" ] || die "esptool not found at $ESPTOOL"
[ -e "$PORT" ] || die "Serial port $PORT not found"

echo ""
echo "=== Using build: demo ==="
echo "Remote build: $REMOTE_BUILD"
echo "Local build:  $LOCAL_BUILD"

echo ""
echo "=== Resolving remote build directory ==="
ssh "$SERVER" "[ -d '$REMOTE_BUILD' ]" || die "Remote build directory not found: $REMOTE_BUILD"

echo ""
echo "=== Syncing firmware from server ==="
mkdir -p "$LOCAL_BUILD"

if ! rsync -avz --delete --progress --exclude "log/" --exclude "*.log" "$SERVER:$REMOTE_BUILD/" "$LOCAL_BUILD/"; then
    die "rsync failed. Check server path, SSH access, and build directory."
fi

cd "$LOCAL_BUILD"

BOOTLOADER="bootloader/bootloader.bin"
PARTITIONS="partition_table/partition-table.bin"

[ -f "$BOOTLOADER" ] || die "Missing $BOOTLOADER"
[ -f "$PARTITIONS" ] || die "Missing $PARTITIONS"

APP_BIN=""
if [ -f "intellikeep_tag.bin" ]; then
    APP_BIN="intellikeep_tag.bin"
else
    APP_BIN="$(find . -maxdepth 1 -type f -name "*.bin" \
        ! -name "bootloader.bin" \
        ! -name "partition-table.bin" \
        ! -name "ota_data_initial.bin" \
        | sed 's|^\./||' \
        | head -n 1)"
fi

[ -n "$APP_BIN" ] || die "Could not find application .bin file in $LOCAL_BUILD"
[ -f "$APP_BIN" ] || die "Application binary not found: $APP_BIN"

echo ""
echo "=== Build contents look valid ==="
echo "Application binary: $APP_BIN"

FLASHED=0

if [ -f "flash_args" ]; then
    echo ""
    echo "=== Using ESP-IDF flash_args ==="
    echo "=== Flashing $CHIP to $PORT ==="
    "$ESPTOOL" --chip "$CHIP" --port "$PORT" --baud "$BAUD" erase_flash
    "$ESPTOOL" --chip "$CHIP" --port "$PORT" --baud "$BAUD" write_flash @"flash_args"
    FLASHED=1
elif [ -f "flasher_args.json" ]; then
    echo ""
    echo "=== Found flasher_args.json ==="
    echo "=== Extracting flash parameters from JSON ==="

    if have_cmd python3; then
        python3 <<'PY'
import json

path = "flasher_args.json"
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

args = data.get("write_flash_args", [])
files = data.get("flash_files", {})

lines = []
for item in args:
    lines.append(str(item))

def addr_key(x):
    try:
        return int(x, 0)
    except Exception:
        return 0

for addr in sorted(files.keys(), key=addr_key):
    lines.append(addr)
    lines.append(files[addr])

with open("flash_args.generated", "w", encoding="utf-8") as out:
    out.write("\n".join(lines) + "\n")
PY

        [ -f "flash_args.generated" ] || die "Failed to generate flash_args.generated from flasher_args.json"

        echo ""
        echo "=== Flashing $CHIP to $PORT ==="
        "$ESPTOOL" --chip "$CHIP" --port "$PORT" --baud "$BAUD" erase_flash
        "$ESPTOOL" --chip "$CHIP" --port "$PORT" --baud "$BAUD" \
            --before default_reset --after hard_reset \
            write_flash @"flash_args.generated"
        FLASHED=1
    else
        die "python3 is required to convert flasher_args.json into esptool arguments"
    fi
else
    echo ""
    echo "=== No flash_args file found; using manual flash command ==="
    echo "=== Flashing $CHIP to $PORT ==="
    "$ESPTOOL" --chip "$CHIP" --port "$PORT" --baud "$BAUD" erase_flash
    "$ESPTOOL" --chip "$CHIP" --port "$PORT" --baud "$BAUD" \
        --before default_reset --after hard_reset write_flash \
        -z --flash_mode dio --flash_freq 40m --flash_size detect \
        0x0 "$BOOTLOADER" \
        0x8000 "$PARTITIONS" \
        0x10000 "$APP_BIN"
    FLASHED=1
fi

[ "$FLASHED" -eq 1 ] || die "Flash failed"

echo ""
echo "=== Flash successful! Starting monitor at $MONITOR_BAUD baud ==="
echo "Exit screen with: Ctrl+A then K"
sleep 1
exec screen "$PORT" "$MONITOR_BAUD"
