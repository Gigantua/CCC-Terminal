import ctypes
import msvcrt
import sys
import time

kernel = ctypes.windll.kernel32
handle = kernel.GetStdHandle(-10)
mode = ctypes.c_uint()
kernel.GetConsoleMode(handle, ctypes.byref(mode))
kernel.SetConsoleMode(handle, (mode.value | 0x200) & ~0x7)
report = {"mouse": "1000", "drag": "1002", "any": "1003", "pixels": "1003h\x1b[?1016"}.get(sys.argv[1])
focus = "\x1b[?1004h" if sys.argv[1] == "any" else ""
sys.stdout.write("\x1b[?1049h" + focus + ("\x1b[?" + report + "h\x1b[?1006h" if report else "") + "probe-ready\r\n")
sys.stdout.flush()
got = ""
end = time.time() + 5
while time.time() < end:
    if not msvcrt.kbhit():
        time.sleep(0.01)
        continue
    c = msvcrt.getwch()
    if c == "q":
        break
    got += c
kernel.SetConsoleMode(handle, mode.value)
sys.stdout.write("\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1004l\x1b[?1006l\x1b[?1016l\x1b[?1049l")
print("probe-" + sys.argv[1] + "=[" + got.replace("\x1b", "ESC") + "]")
