import sys
import time

ESC = "\x1b"
out = sys.stdout
out.write(ESC + "]9;4;1;40" + ESC + "\\")
out.flush()
time.sleep(0.2)
out.write(ESC + "]9;4;3;0\x07")
out.flush()
time.sleep(0.2)
out.write("ring\x07done\r\n")
out.flush()
time.sleep(0.2)
if sys.argv[1] == "leave":
    out.write(ESC + "]9;4;2;75" + ESC + "\\")
else:
    out.write(ESC + "]9;4;0;0" + ESC + "\\")
out.write("notify-end\r\n")
out.flush()
time.sleep(0.2)
