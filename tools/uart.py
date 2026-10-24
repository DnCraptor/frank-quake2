#!/usr/bin/env python3
#
# frank-quake2 — debug UART helper (UART0, 115200).
#
#   tools/uart.py cap SECONDS [OUT]        capture the log (default: stdout)
#   tools/uart.py cmd "LINE" [SECONDS]     send a Quake console line, capture
#   tools/uart.py bootsel                  reboot into BOOTSEL (~BOOT~)
#   tools/uart.py reset [SECONDS]          reboot (~RST~), capture the boot
#
# Port: $Q2_UART (default /dev/cu.usbmodem11102).
#
# SPDX-License-Identifier: GPL-3.0-or-later
import os, sys, termios, time

DEV = os.environ.get("Q2_UART", "/dev/cu.usbmodem11102")


def open_port():
    fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
    a[4] = a[5] = termios.B115200
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 1  # reads return after 100 ms of silence
    termios.tcsetattr(fd, termios.TCSANOW, a)
    import fcntl
    fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) & ~os.O_NONBLOCK)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def capture(fd, seconds, out):
    end = time.time() + seconds
    while time.time() < end:
        data = os.read(fd, 65536)
        if data:
            out.write(data.decode("latin-1"))
            out.flush()


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__ or "usage: uart.py cap|cmd|bootsel|reset")
    fd = open_port()
    op = sys.argv[1]
    if op == "cap":
        out = open(sys.argv[3], "w") if len(sys.argv) > 3 else sys.stdout
        capture(fd, float(sys.argv[2]), out)
    elif op == "cmd":
        os.write(fd, b"\n")  # the first bytes after opening the port can be lost
        time.sleep(0.05)
        os.write(fd, (sys.argv[2] + "\n").encode())
        capture(fd, float(sys.argv[3]) if len(sys.argv) > 3 else 2, sys.stdout)
    elif op == "bootsel":
        os.write(fd, b"~BOOT~")
        capture(fd, 1, sys.stdout)
    elif op == "reset":
        os.write(fd, b"~RST~")
        capture(fd, float(sys.argv[2]) if len(sys.argv) > 2 else 10, sys.stdout)
    else:
        sys.exit("unknown command " + op)


if __name__ == "__main__":
    main()
