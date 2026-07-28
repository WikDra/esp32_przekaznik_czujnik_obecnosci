"""Read the ESP32 console for a while (Windows side, no idf.py needed).

    python scripts\\monitor.py [COM5] [seconds]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

try:
    ser = serial.Serial(port, 115200, timeout=0.5)
except Exception as exc:  # noqa: BLE001
    print(f"ERROR: cannot open {port}: {exc}", flush=True)
    sys.exit(1)

print(f"--- reading {port} for {duration:.0f}s ---", flush=True)
end = time.time() + duration
try:
    while time.time() < end:
        data = ser.read(4096)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
finally:
    ser.close()
