import ctypes
import msvcrt
import sys
import time

kernel = ctypes.windll.kernel32
handle = kernel.GetStdHandle(-10)
mode = ctypes.c_uint()
kernel.GetConsoleMode(handle, ctypes.byref(mode))
kernel.SetConsoleMode(handle, (mode.value | 0x200) & ~0x7)


def read(idle, limit):
    got = ""
    last = time.time()
    end = last + limit
    while time.time() < end and time.time() - last < idle:
        if not msvcrt.kbhit():
            time.sleep(0.01)
            continue
        got += msvcrt.getwch()
        last = time.time()
    return got


def ask(seq):
    sys.stdout.write(seq + "\x1b[?u")
    sys.stdout.flush()
    return read(0.5, 2)


arg = sys.argv[1]
if arg == "stack":
    steps = ["\x1b[>1u", "\x1b[?1049h", "\x1b[>3u", "\x1b[=8;2u", "\x1b[=2;3u", "\x1b[?1049l", "\x1b[>5u", "\x1b[<u", "\x1b[<9u"]
    got = "".join(ask(s) for s in steps)
else:
    sys.stdout.write("\x1b[>" + arg + "uready-" + arg + "\r\n")
    sys.stdout.flush()
    got = read(1.5, 15)
    sys.stdout.write("\x1b[<u")
    sys.stdout.flush()
kernel.SetConsoleMode(handle, mode.value)
print("probe-" + arg + "=[" + got.replace("\x1b", "ESC").replace("\r", "CR") + "]")
