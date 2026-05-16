#!/usr/bin/env bash
# Flash the elf, wait for CDC re-enumeration, capture serial for N seconds.
# Usage: ./flash_and_capture.sh <path/to.elf> [seconds]
set -e
ELF="$1"
SECS="${2:-15}"

# Try common CDC port names; on macOS the Pico shows up as cu.usbmodemNNNN.
find_port() {
    for p in /dev/cu.usbmodem*; do
        [[ -e $p ]] && { echo "$p"; return; }
    done
    return 1
}

picotool load -fx "$ELF" >/dev/null 2>&1
echo "[flash_and_capture] flashed, waiting for CDC..."

# Poll for CDC enumeration
for _ in $(seq 1 50); do
    if PORT=$(find_port); then break; fi
    sleep 0.2
done
if [[ -z "$PORT" ]]; then
    echo "[flash_and_capture] no CDC port found"
    exit 2
fi
echo "[flash_and_capture] port=$PORT, capturing ${SECS}s..."

# Set raw mode and read.  Python is the most reliable cross-tool here —
# `cat`/`dd` race with udev re-enumeration on macOS.
python3 - "$PORT" "$SECS" <<'PY'
import serial, sys, time
port, secs = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=0.5)
end = time.time() + secs
while time.time() < end:
    data = s.read(4096)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
PY
echo
echo "[flash_and_capture] done"
