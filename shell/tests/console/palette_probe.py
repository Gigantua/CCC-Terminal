import ctypes
import msvcrt
import sys
import time
from ctypes import wintypes


class Info(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.ULONG), ("size", wintypes._COORD), ("cursor", wintypes._COORD), ("attributes", wintypes.WORD),
                ("window", wintypes.SMALL_RECT), ("maxWindow", wintypes._COORD), ("popup", wintypes.WORD), ("fullscreen", wintypes.BOOL),
                ("colors", wintypes.DWORD * 16)]


kernel = ctypes.windll.kernel32
out = kernel.GetStdHandle(-11)
inp = kernel.GetStdHandle(-10)
mode = ctypes.c_uint()
kernel.GetConsoleMode(inp, ctypes.byref(mode))
kernel.SetConsoleMode(inp, (mode.value | 0x200) & ~0x7)
info = Info()
info.cbSize = ctypes.sizeof(Info)
kernel.GetConsoleScreenBufferInfoEx(out, ctypes.byref(info))
before = info.colors[1]
info.colors[1] = 0x112233
kernel.SetConsoleScreenBufferInfoEx(out, ctypes.byref(info))
again = Info()
again.cbSize = ctypes.sizeof(Info)
kernel.GetConsoleScreenBufferInfoEx(out, ctypes.byref(again))
sys.stdout.write("\x1b]4;4;?\x07")
sys.stdout.flush()
got, end = "", time.time() + 2
while time.time() < end and not got.endswith("\x07"):
    if msvcrt.kbhit():
        got += msvcrt.getwch()
    else:
        time.sleep(0.01)
print("table=%06x,%06x osc=%s" % (before, again.colors[1], got.replace("\x1b", "ESC").replace("\x07", "")))
