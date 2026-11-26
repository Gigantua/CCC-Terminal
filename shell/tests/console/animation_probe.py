import base64
import msvcrt
import sys


def send(control, payload=b""):
    sys.stdout.write("\x1b_G" + control + (";" + base64.b64encode(payload).decode() if payload else "") + "\x1b\\")


red, green, blue = bytes([255, 0, 0]), bytes([0, 255, 0]), bytes([0, 0, 255])
send("a=t,i=11,f=24,s=2,v=1,q=2", red + red)
send("a=f,i=11,f=24,s=2,v=1,q=2", green + green)
send("a=f,i=11,c=1,x=1,f=24,s=1,v=1,q=2", blue)
send("a=c,i=11,r=2,c=3,w=1,h=1,q=2")
send("a=p,i=11,c=4,r=1,q=2")
print("\r\nstopped-ready")
sys.stdout.flush()
while msvcrt.getwch() != "\r":
    pass
send("a=a,i=11,c=2,q=2")
print("current-ready")
sys.stdout.flush()
while msvcrt.getwch() != "\r":
    pass
send("a=a,i=11,r=1,z=150,q=2")
send("a=a,i=11,r=2,z=150,q=2")
send("a=a,i=11,r=3,z=150,q=2")
send("a=a,i=11,s=3,q=2")
print("running-ready")
sys.stdout.flush()
while msvcrt.getwch() != "\r":
    pass
