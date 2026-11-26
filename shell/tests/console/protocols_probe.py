import base64
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


def show(text):
    return text.replace("\x1b", "ESC").replace("\x07", "BEL")


def ask(name, seq, idle=0.3, limit=2):
    sys.stdout.write(seq)
    sys.stdout.flush()
    print(name + "=[" + show(read(idle, limit)) + "]")


def b64(s):
    return base64.b64encode(s.encode()).decode()


ask("width", "\r\x1b[6n\x1b]66;w=2; \x07\x1b[6n\x1b]66;s=2; \x07\x1b[6n\r\n")
sys.stdout.write("a\x1b]66;w=3;X\x07b\r\n")
ask("cursors", "\x1b[> q")
ask("cursorset", "\x1b[>1;2:3:4 q\x1b[>40;2:255:0:0 q\x1b[>100 q\x1b[>101 q")
ask("cursorclear", "\x1b[>0;4 q\x1b[>100 q")
ask("list", "\x1b]5522;type=read;" + b64(".") + "\x1b\\")
ask("write", "\x1b]5522;type=write\x1b\\\x1b]5522;type=wdata:mime=" + b64("text/plain") + ";" + b64("hello5522") + "\x1b\\\x1b]5522;type=wdata\x1b\\")
print("read-ready")
ask("read", "\x1b]5522;type=read:id=a1;" + b64("text/plain") + "\x1b\\", 0.5, 15)
ask("mode", "\x1b[?5522h\x1b[?5522$p")
print("paste-ready")
event = read(0.5, 15)
print("event=[" + show(event) + "]")
pw = event.split(":pw=")[1].split("\x1b")[0] if ":pw=" in event else ""
ask("pasted", "\x1b]5522;type=read:pw=" + pw + ":name=" + b64("Paste event") + ";" + b64("text/plain") + "\x1b\\")
sys.stdout.write("\x1b[?5522l\x1b_Ga=T,U=1,i=9,c=2,r=1,f=24,s=2,v=2,q=2;" + base64.b64encode(bytes(12)).decode() + "\x1b\\")
sys.stdout.write("\x1b[38;5;9m\U0010EEEE̅̅\U0010EEEE̅̍\x1b[39m\r\n")
print("placeholder-ready")
while msvcrt.getwch() != "\r":
    pass
kernel.SetConsoleMode(handle, mode.value)
