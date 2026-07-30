"""Read the ESP32 console for a while (Windows side, no idf.py needed).

    python scripts\\monitor.py [COM5] [seconds]

Uwaga: na płytkach z natywnym USB (USB-Serial/JTAG) linie DTR/RTS steruują resetem
i bootloaderem. pyserial domyślnie je aktywuje przy otwarciu portu, co restartuje
układ - a przy włączonej opcji APP_POWER_CYCLE_FORCE_ON dwa takie restarty pod rząd
zapalają żarówkę. Dlatego port otwieramy z DTR/RTS ustawionymi na False.
Do celowego resetu jest scripts/reset_monitor.py.
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.5
# Musi być ustawione PRZED open(), inaczej sterownik na chwilę ściągnie reset.
ser.dtr = False
ser.rts = False

try:
    ser.open()
except Exception as exc:  # noqa: BLE001
    print(f"ERROR: cannot open {port}: {exc}", flush=True)
    sys.exit(1)

print(f"--- reading {port} for {duration:.0f}s (bez resetu) ---", flush=True)
end = time.time() + duration
try:
    while time.time() < end:
        data = ser.read(4096)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
finally:
    ser.close()
