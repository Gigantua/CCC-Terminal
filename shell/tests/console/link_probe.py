import sys

ESC = "\x1b"


def link(uri, text):
    return ESC + "]8;;" + uri + ESC + "\\" + text + ESC + "]8;;" + ESC + "\\"


lines = [
    "plain " + link("https://example.com/one", "alpha") + " after",
    "reset " + ESC + "]8;;https://example.com/two" + ESC + "\\be" + ESC + "[0;31mta" + ESC + "[0m" + ESC + "]8;;" + ESC + "\\ done",
    "bell " + ESC + "]8;;file:///C:/tmp/x.txt\x07gamma" + ESC + "]8;;\x07 end",
    "unsafe " + link("calc.exe", "delta") + " end",
]
if sys.argv[1] == "file":
    open(sys.argv[2], "w", newline="").write("\r\n".join(lines) + "\r\n")
else:
    sys.stdout.write("\r\n".join(lines) + "\r\nlinks-ready\r\n")
    sys.stdout.flush()
    sys.stdin.readline()
