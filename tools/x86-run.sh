#!/bin/sh
# Run the headless x86_64 build on the build host with an input script and
# fetch framebuffer dumps as a contact sheet.
#   tools/x86-run.sh SECONDS "INPUT_SCRIPT" OUT_PREFIX [extra env...]
set -e
HOST=$(BUILD_HOST="$BUILD_HOST" sh "$(cd "$(dirname "$0")/.." && pwd)/tools/buildhost.sh")
SECS=$1; SCRIPT=$2; OUT=$3; shift 3
HERE=$(cd "$(dirname "$0")/.." && pwd)
ssh "$HOST" "cd /tmp/nfsx86 && rm -rf shots && mkdir -p shots save &&
  (env NFS_NO_SENSORS=1 SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LANG=en_US.UTF-8 NFS_SHOT_DIR=/tmp/nfsx86/shots NFS_SHOT_MS=\${SHOT_MS:-1000} NFS_VSYNC=0 NFS_INPUT_SCRIPT='$SCRIPT' $* ./build/harbour-nfsshift --data /tmp/nfsx86/data --user /tmp/nfsx86/save --no-rotate > run.log 2>&1 & PID=\$!; sleep $SECS; kill -9 \$PID 2>/dev/null; true)"
mkdir -p "$OUT.d" && rm -f "$OUT.d"/*
ssh "$HOST" "cd /tmp/nfsx86/shots && tar -cf - ." | tar -xf - -C "$OUT.d"
scp -q "$HOST:/tmp/nfsx86/run.log" "$OUT.log"
python3 - "$OUT.d" "$OUT" <<'PY'
import glob, sys
from PIL import Image
d, out = sys.argv[1], sys.argv[2]
fs = sorted(glob.glob(d + '/shot*.ppm'))
W, H = 480, 360
for page in range(0, len(fs), 12):
    ims = [Image.open(f).resize((W, H)) for f in fs[page:page+12]]
    sheet = Image.new('RGB', (W * 3, H * ((len(ims) + 2) // 3)))
    for i, im in enumerate(ims):
        sheet.paste(im, ((i % 3) * W, (i // 3) * H))
    sheet.save('%s_%02d.png' % (out, page // 12))
print(len(fs), 'frames')
PY
