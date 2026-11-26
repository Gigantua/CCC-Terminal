#!/bin/sh
cd "$(dirname "$0")/../../.."
token=ccc-tabs-test-$$
export CCC_TEST_TOKEN=$token
Xvfb :97 -screen 0 1920x1080x24 > /dev/null 2>&1 &
export DISPLAY=:97
fail=0

wait_until() { for i in $(seq 1 100); do eval "$1" && return 0; sleep 0.1; done; return 1; }
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
windows() { xdotool search --onlyvisible --class ccc 2>/dev/null | sort; }
count() { windows | wc -l; }
outline_visible() {
    for id in $(xwininfo -root -children | awk '/^ +0x/ && /has no name/ {print $1}'); do
        info=$(xwininfo -id "$id")
        echo "$info" | grep -q "Depth: 32" && echo "$info" | grep -q "IsViewable" && return 0
    done
    return 1
}
move() {
    for k in 1 2 3 4 5 6 7 8; do xdotool mousemove $(( $1 + ($3 - $1) * k / 8 )) $(( $2 + ($4 - $2) * k / 8 )); sleep 0.05; done
}
stop() {
    for p in /proc/[0-9]*; do grep -qs "CCC_TEST_TOKEN=$token" "$p/environ" && kill "${p#/proc/}" 2>/dev/null; done
}

wait_until "xwininfo -root > /dev/null 2>&1" || { echo "FAIL Xvfb"; stop; exit 1; }
./bin/ccc --shot "${TMPDIR:-/tmp}/ccc_tabs_a.bmp" --run "echo window-A" &
wait_until '[ $(count) -ge 1 ]'
a=$(windows | head -1)
xdotool windowmove "$a" 0 0
./bin/ccc --shot "${TMPDIR:-/tmp}/ccc_tabs_b.bmp" --run "echo window-B" &
wait_until '[ $(count) -ge 2 ]'
b=$(windows | grep -v "^$a$" | head -1)
xdotool windowmove "$b" 1000 0
xdotool windowfocus "$a" key ctrl+shift+n

xdotool mousemove 350 18 mousedown 1
move 350 18 600 500
wait_until outline_visible
check "outline follows a tab dragged off the strip" outline_visible
move 600 500 1150 18
xdotool mouseup 1
wait_until '! outline_visible'
check "outline hidden after the drop" '! outline_visible'
check "tab joined the other window (2 windows)" '[ $(count) -eq 2 ]'

xdotool mousemove 1350 18 mousedown 1
move 1350 18 1400 700
xdotool mouseup 1
wait_until '[ $(count) -ge 3 ]'
check "tab dropped on the desktop opens a new window (3 windows)" '[ $(count) -eq 3 ]'

stop
exit $fail
