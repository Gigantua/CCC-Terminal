import os
import subprocess
import sys
import time

marker = os.path.join(os.environ["TEMP"], "ccc_tree_" + sys.argv[2])
if sys.argv[1] == "child":
    time.sleep(1.5)
    open(marker, "w").write("survived")
    sys.exit(0)
if os.path.exists(marker):
    os.remove(marker)
subprocess.Popen([sys.executable, __file__, "child", sys.argv[2]], creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
try:
    print("tree-waiting", flush=True)
    time.sleep(30)
    print("tree-not-interrupted")
except KeyboardInterrupt:
    print("tree-interrupted", flush=True)
time.sleep(2.5)
print("tree-child-" + ("alive" if os.path.exists(marker) else "killed"))
