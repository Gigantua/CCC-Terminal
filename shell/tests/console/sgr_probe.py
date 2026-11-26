import sys

E = "\x1b["
print(E + "4:3mcurly" + E + "0m " + E + "1;4mbold" + E + "4:0mnoul" + E + "0m")
print(E + "38:2::10:20:30mrgbspace" + E + "0m " + E + "38:2:10:20:30mrgbshort" + E + "0m " + E + "38;2;1;2;3mlegacy" + E + "0m")
print(E + "58:5:196;4mulcolor" + E + "59;24m " + E + "21mdouble" + E + "24m " + E + "4:5;58;2;9;8;7mdashed" + E + "0m")
print(E + "4:4mdotted" + E + "0m " + E + "3;4:2mitalicdouble" + E + "0m")
if len(sys.argv) > 1 and sys.argv[1] == "live":
    print("sgr-ready")
    sys.stdin.readline()
