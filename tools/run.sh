#!/bin/sh
# Run the loader for at most $SECS seconds (default 20), log to $LOG.
HERE=$(cd "$(dirname "$0")/.." && pwd)
SECS=${SECS:-20}
LOG=${LOG:-/tmp/nfsshift-run.log}
DATA=${DATA:-$HOME/.local/share/harbour-nfsshift/data}
"$HERE/build/harbour-nfsshift" --data "$DATA" "$@" > "$LOG" 2>&1 &
PID=$!
i=0
while [ $i -lt "$SECS" ] && kill -0 $PID 2>/dev/null; do sleep 1; i=$((i+1)); done
if kill -0 $PID 2>/dev/null; then kill -9 $PID; echo "killed after ${SECS}s"; else wait $PID; echo "exit=$?"; fi
