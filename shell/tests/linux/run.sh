#!/bin/sh
cd "$(dirname "$0")"
ccc=../../../bin/ccc
out=${TMPDIR:-/tmp}
fail=0
for t in l*_*.bat; do
    n=${t%.bat}
    [ -f "$n.expected" ] || continue
    "$ccc" /d /c "$t" > "$out/ccc_$n.txt" 2>&1
    if cmp -s "$n.expected" "$out/ccc_$n.txt"; then echo "PASS $t"
    else echo "FAIL $t"; diff "$n.expected" "$out/ccc_$n.txt" | head -20; fail=1
    fi
done
exit $fail
