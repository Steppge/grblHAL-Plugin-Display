"""Firmware of the gamepad receiver (D1 Mini ESP32 on J7) over the P4 display's console, without the SD card.

    python gp_update.py COM30 gamepad.bin

gamepad.bin is the app image of the receiver build (firmware.bin of the esp32_gamepad build folder, sketch 1.4
or newer with two OTA partitions). The display receives it into PSRAM ("gpupdate", in blocks of 4 KB, each
acknowledged), checks the CRC32 and sends it to the receiver as an update from the card does.
No other program may have the port open (the PC-Deck app: quit it first). The display does not reset:
DTR and RTS stay off. Needs pyserial.
"""

import sys
import time
import zlib

import serial


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    port, path = sys.argv[1], sys.argv[2]
    data = open(path, 'rb').read()
    crc = zlib.crc32(data) & 0xFFFFFFFF

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 921600
    ser.timeout = 5
    ser.dtr = False
    ser.rts = False
    try:
        ser.open()
    except serial.SerialException as e:
        sys.exit(f'{port} could not be opened ({e}). Is the PC-Deck app running?')
    time.sleep(0.2)
    ser.reset_input_buffer()
    # the newline ends a half line left by another program
    ser.write(f'\ngpupdate {len(data)} {crc:08x}\n'.encode())

    block = 0
    deadline = time.time() + 5
    while not block:
        line = ser.readline().decode(errors='replace').strip()
        if line.startswith('gpupdate ready'):
            block = int(line.split()[2])
        elif line.startswith('gpupdate failed'):
            sys.exit(line)
        elif time.time() > deadline:
            sys.exit('no answer from the display (firmware without gpupdate?)')

    print(f'Sending {path} ({len(data)} bytes) to the display ...')
    for i in range(0, len(data), block):
        ser.write(data[i:i + block])
        want = min(i + block, len(data))
        while True:
            line = ser.readline().decode(errors='replace').strip()
            if line == f'K {want}':
                break
            if line.startswith('gpupdate failed'):
                sys.exit(line)
            if not line:
                sys.exit(f'no acknowledge at {want} bytes')
        print(f'\r  {want * 100 // len(data):3d} %', end='', flush=True)
    print()

    # the display updates the receiver: its lines until done / failed
    ser.timeout = 1
    deadline = time.time() + 300
    while time.time() < deadline:
        line = ser.readline().decode(errors='replace').strip()
        if not line:
            continue
        if line.startswith(('gpupdate', 'gamepad update', 'Gamepad receiver')):
            print('  ' + line)
        if line.startswith('gpupdate done'):
            ser.close()
            return
        if line.startswith('gpupdate failed'):
            ser.close()
            sys.exit(1)
    sys.exit('no end of the update within 300 s')


if __name__ == '__main__':
    main()
