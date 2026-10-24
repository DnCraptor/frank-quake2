#!/usr/bin/env python3
#
# frank-quake2 — debug UART helper (UART0, 115200).
#
#   tools/uart.py cap SECONDS [OUT]        capture the log (default: stdout)
#   tools/uart.py cmd "LINE" [SECONDS]     send a Quake console line, capture
#   tools/uart.py bootsel                  reboot into BOOTSEL (~BOOT~)
#   tools/uart.py reset [SECONDS]          reboot (~RST~), capture the boot
#   tools/uart.py script OUT STEP...       run steps, capturing all along:
#                                          "cmd LINE" sends a console line,
#                                          "wait SECONDS" captures meanwhile,
#                                          "until REGEX SECONDS" captures until
#                                          REGEX shows up (or the timeout)
#
# Port: $Q2_UART (default /dev/cu.usbmodem11102).
#
# SPDX-License-Identifier: GPL-3.0-or-later
import os, re, sys, termios, time

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


# text received since the last command sent (what "until" searches)
since_cmd = ""
t0 = time.time()


class Stamped:
    """Writes each line prefixed with the seconds since the start."""

    def __init__(self, f):
        self.f, self.bol = f, True

    def write(self, text):
        for part in text.splitlines(True):
            if self.bol:
                self.f.write("%7.2f " % (time.time() - t0))
            self.f.write(part)
            self.bol = part.endswith("\n")

    def flush(self):
        self.f.flush()


def capture(fd, seconds, out, until=None):
    global since_cmd
    end = time.time() + seconds
    while True:
        if until and until.search(since_cmd):
            return True
        if time.time() >= end:
            return False
        data = os.read(fd, 65536)
        if data:
            text = data.decode("latin-1")
            out.write(text)
            out.flush()
            since_cmd = (since_cmd + text)[-65536:]


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
    elif op == "script":
        out = Stamped(open(sys.argv[2], "w"))
        os.write(fd, b"\n")
        for step in sys.argv[3:]:
            kind, _, arg = step.partition(" ")
            if kind == "cmd":
                global since_cmd
                since_cmd = ""
                out.write("\n>>> %s\n" % arg)
                os.write(fd, (arg + "\n").encode())
                capture(fd, 0.3, out)
            elif kind == "wait":
                capture(fd, float(arg), out)
            elif kind == "until":
                rx, _, secs = arg.rpartition(" ")
                if not capture(fd, float(secs), out, re.compile(rx)):
                    print("timeout waiting for %s (%d bytes since the command, ends %r)"
                          % (rx, len(since_cmd), since_cmd[-80:]), file=sys.stderr)
            else:
                sys.exit("unknown step " + step)
    else:
        sys.exit("unknown command " + op)


if __name__ == "__main__":
    main()
