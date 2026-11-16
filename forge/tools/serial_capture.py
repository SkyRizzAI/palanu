"""Reset the board over DTR/RTS and print serial output for N seconds.

    python serial_capture.py /dev/cu.usbserial-10 8 [--no-reset] [--send "text"] [--send-delay 3]

Run inside the ESP-IDF python environment (it provides pyserial).
"""
import sys
import time

import serial


def main() -> int:
    args = sys.argv[1:]
    port, seconds = args[0], float(args[1])
    reset = "--no-reset" not in args
    send = args[args.index("--send") + 1] if "--send" in args else None
    delay = float(args[args.index("--send-delay") + 1]) if "--send-delay" in args else 3.0

    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, 115200, 0.1
    p.dtr = False
    p.rts = False
    p.open()
    if reset:
        p.rts = True
        time.sleep(0.2)
        p.rts = False
    end = time.time() + seconds
    sent = send is None
    t0 = time.time()
    while time.time() < end:
        if not sent and time.time() - t0 >= delay:
            p.write(send.encode("utf-8").decode("unicode_escape").encode("latin-1"))
            sent = True
        data = p.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
    p.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
