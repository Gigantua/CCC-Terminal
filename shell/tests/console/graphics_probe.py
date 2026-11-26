import base64
import ctypes
import msvcrt
import struct
import sys
import time
import zlib

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


def wait_enter():
    while msvcrt.getwch() != "\r":
        pass


def send(control, payload=b""):
    sys.stdout.write("\x1b_G" + control + (";" + base64.b64encode(payload).decode() if payload else "") + "\x1b\\")


def ask(name, *commands):
    for c in commands:
        send(*c)
    sys.stdout.flush()
    print(name + "=[" + read(0.3, 2).replace("\x1b", "ESC") + "]")


def png(w, h, rgba):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


rgb = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255])
rgba = bytes(range(64))
b64 = base64.b64encode(rgb).decode()
ask("query", ("i=31,s=1,v=1,a=q,t=d,f=24", b"\x00\x00\x00"))
sys.stdout.write("\x1b_Gi=5,f=24,s=2,v=2,m=1;" + b64[:8] + "\x1b\\\x1b_Gm=0;" + b64[8:] + "\x1b\\")
sys.stdout.flush()
print("chunked=[" + read(0.3, 2).replace("\x1b", "ESC") + "]")
ask("png", ("i=6,f=100", png(4, 4, rgba)))
ask("zlib", ("i=7,f=32,s=4,v=4,o=z", zlib.compress(rgba)))
ask("bad", ("i=8,f=24,s=10,v=10", rgb))
sys.stdout.write("\x1b[16;5H")
ask("place", ("a=p,i=5,c=4,r=2",))
sys.stdout.write("\x1b[20;1H")
ask("placepng", ("a=p,i=6,p=2,z=-1,C=1",))
ask("missing", ("a=p,i=99",))
sys.stdout.write("\x1b[26;1H")
print("placed-ready")
wait_enter()
ask("delete", ("a=d,d=I,i=5",), ("a=p,i=5",))
print("deleted-ready")
wait_enter()
kernel.SetConsoleMode(handle, mode.value)
