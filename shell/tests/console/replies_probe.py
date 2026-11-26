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
    sys.stdout.write(seq)
    sys.stdout.flush()
    return read(0.3, 2).replace("\x1b", "ESC").replace("\x07", "BEL")


steps = [
    ("pixels", "\x1b[14t"),
    ("cell", "\x1b[16t"),
    ("chars", "\x1b[18t"),
    ("resize", "\x1b[?2048h"),
    ("mode2048", "\x1b[?2048$p"),
    ("mode2027", "\x1b[?2027$p"),
    ("scheme", "\x1b[?996n"),
    ("fg", "\x1b]10;?\x1b\\"),
    ("setbg", "\x1b[?2031h\x1b]11;#f0f0f0\x1b\\"),
    ("bg", "\x1b]11;?\x1b\\"),
    ("pal", "\x1b]4;1;rgb:12/34/56\x1b\\\x1b]4;1;?\x1b\\"),
    ("kitty", "\x1b]21;foreground=#010203;2=?;foreground=?\x1b\\"),
    ("push", "\x1b[#P\x1b]4;1;#ffffff\x1b\\\x1b[#Q\x1b]4;1;?\x1b\\"),
    ("reset", "\x1b]104\x1b\\\x1b]4;1;?\x1b\\"),
]
for name, seq in steps:
    print(name + "=[" + ask(seq) + "]")
sys.stdout.write("\x1b[?2048l\x1b[?2031l\x1b]111\x1b\\\x1b]110\x1b\\")
kernel.SetConsoleMode(handle, mode.value)
print("probe-done")
