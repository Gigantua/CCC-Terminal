import base64
import ctypes
import mmap
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


name = "ccc_shm_probe"
shared = mmap.mmap(-1, 4096, tagname=name)
shared.write(bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255]))
sys.stdout.write("\x1b_Ga=T,t=s,f=24,s=2,v=2,i=3,S=12,c=4,r=2;" + base64.b64encode(name.encode()).decode() + "\x1b\\")
sys.stdout.write("\x1b_Ga=t,t=s,f=24,s=2,v=2,i=4;" + base64.b64encode(b"missing_shm").decode() + "\x1b\\")
sys.stdout.flush()
print("\r\nshm=[" + read(0.3, 2).replace("\x1b", "ESC") + "]")
print("shm-ready")
while msvcrt.getwch() != "\r":
    pass
kernel.SetConsoleMode(handle, mode.value)
