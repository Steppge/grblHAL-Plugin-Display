"""Screen image of the P4 display over its console (USB-C "UART", CH340) as PNG.

    python shot.py COM9 main.png

Sends "shot" to the display, which answers with the screen as raw RGB565 (about 14 s at 921600 baud).
Needs pyserial and Pillow. The display does not reset: DTR and RTS stay off.
"""

import sys
import time

import serial
from PIL import Image


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    port, out = sys.argv[1], sys.argv[2]

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 921600
    ser.timeout = 30
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.write(b"\nshot\n")    # the newline ends a half line left by another program (e.g. the PC-Deck app)

    deadline = time.time() + 10
    while True:
        line = ser.readline()
        if line.startswith(b"SHOT "):
            break
        if line.startswith(b"shot:"):
            sys.exit(line.decode(errors="replace").strip())
        if time.time() > deadline:
            sys.exit("no answer from the display")

    w, h, size = (int(x) for x in line.split()[1:4])
    data = bytearray()
    while len(data) < size:
        chunk = ser.read(size - len(data))
        if not chunk:
            sys.exit(f"timeout after {len(data)} of {size} bytes")
        data += chunk
        print(f"\r{len(data) * 100 // size} %", end="", flush=True)
    print()
    ser.close()

    stride = size // h
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        row = y * stride
        for x in range(w):
            v = data[row + 2 * x] | (data[row + 2 * x + 1] << 8)
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            px[x, y] = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
    img.save(out)
    print(f"{out}: {w}x{h}")


if __name__ == "__main__":
    main()
