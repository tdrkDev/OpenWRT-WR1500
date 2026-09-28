#!/bin/sh
# Send recovery.bin (next to this script) to the WR1500 in recovery mode.
# Usage: ./tftp.sh [router-ip]      (default 192.168.1.6)
DIR=$(cd "$(dirname "$0")" && pwd)

if [ ! -f "$DIR/recovery.bin" ]; then
    echo "recovery.bin not found in $DIR: unpack the whole ZIP and run this script from there." >&2
    exit 1
fi

for PY in python3 python; do
    if command -v "$PY" >/dev/null 2>&1 &&
       "$PY" -c 'import sys; sys.exit(sys.version_info < (3, 7))' 2>/dev/null; then
        exec "$PY" "$DIR/recovery_install.py" "$DIR/recovery.bin" "$@"
    fi
done

echo "Python 3.7+ is required: https://www.python.org/downloads/ (on macOS also: xcode-select --install)" >&2
exit 1
