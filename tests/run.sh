#!/bin/sh
# Runs the Wine tests in a temporary prefix whose path contains a space.
# Usage: tests/run.sh BUILD_DIR. XCURL=/path/to/XCurl.dll tests with the game's XCurl.
set -eu
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree=d;mshtml=d}"
BUILD=$(CDPATH='' cd -- "$1" && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/dungeons2forlinux test.XXXXXX")
trap 'WINEPREFIX="$TMP/prefix" wineserver -w 2>/dev/null; rm -rf -- "$TMP" || :' EXIT
status=0

winpath() { printf 'Z:%s' "$1" | sed 's|/|\\|g'; }

# tokens DIR EXPIRY PF_TOKEN: writes a test tokens.txt
tokens() {
    mkdir -p "$1"
    printf 'exp=%s\nxuid=2533274800000001\ngamertag=Tester\nxbox=XBL3.0 x=1;xbox-token\nmc=XBL3.0 x=1;mc-token\npf=%s\nmsa=msa-token\n' "$2" "$3" > "$1/tokens.txt"
}

run() {
    state=$1
    shift
    WINEPREFIX="$TMP/prefix" WINEDEBUG=-all DUNGEONS2FORLINUX_DIR="$(winpath "$state")" \
        wine "$TMP/test_runtime.exe" "$@" 2>/dev/null || status=1
}

cp -- "$BUILD/xgameruntime.dll" "$BUILD/test_runtime.exe" "$TMP/"
now=$(date +%s)

echo "== runtime"
tokens "$TMP/valid" $((now + 86400)) 'XBL3.0 x=1;pf-token'
cp -- "${XCURL:-$BUILD/XCurl.dll}" "$TMP/XCurl.dll"
run "$TMP/valid" "$TMP/XCurl.dll"

echo "== sign-in helper succeeds"
tokens "$TMP/signin" $((now - 60)) ''
cat > "$TMP/stub signin.py" <<'PY'
import os, time
import sys
d = sys.argv[sys.argv.index("--state") + 1]
with open(os.path.join(d, "tokens.txt.tmp"), "w") as f:
    f.write("exp=%d\nxuid=2533274800000002\nxbox=XBL3.0 x=1;xbox-token\npf=XBL3.0 x=1;fresh-pf-token\n" % (time.time() + 86400))
os.replace(os.path.join(d, "tokens.txt.tmp"), os.path.join(d, "tokens.txt"))
PY
DUNGEONS2FORLINUX_SIGNIN="$TMP/stub signin.py" run "$TMP/signin" --signin-ok

echo "== embedded helper reports a failure (network blocked)"
tokens "$TMP/fail" $((now - 60)) ''
printf 'refresh=test-refresh-token\n' >> "$TMP/fail/tokens.txt"
https_proxy=http://127.0.0.1:9 HTTPS_PROXY=http://127.0.0.1:9 run "$TMP/fail" --signin-fail
if ! cmp -s "$TMP/fail/signin.py" "$(dirname "$0")/../src/signin.py"; then
    echo "FAIL: the embedded helper was not written out intact"
    status=1
fi
grep -q 'cannot reach' "$TMP/fail/login-error.txt" 2>/dev/null || { echo "FAIL: helper did not report the network error"; status=1; }

for log in "$TMP"/*/dungeons2forlinux.log; do
    if grep -a -q -e 'xbox-token' -e 'pf-token' -e 'mc-token' -e 'msa-token' -e 'test-refresh-token' "$log"; then
        echo "FAIL: a token value reached $log"
        status=1
    fi
    if grep -a -q 'QUERYSECRET' "$log"; then
        echo "FAIL: a URL query string reached $log"
        status=1
    fi
    if [ "$(tr -cd '\000' < "$log" | wc -c)" -ne 0 ]; then
        echo "FAIL: $log contains NUL bytes"
        status=1
    fi
done
exit $status
