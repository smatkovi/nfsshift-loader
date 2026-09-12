#!/bin/sh
# Run two headless instances of the loader on the build host and let them find
# each other over the loopback: one hosts, one joins.
#
#   tools/x86-lan.sh SECONDS "HOST_SCRIPT" "CLIENT_SCRIPT" OUT_PREFIX
#
# The client uses a second port pair and is pointed at the host directly, so
# the test does not depend on broadcast delivery to two sockets on one machine.
set -e
HOST=$(BUILD_HOST="$BUILD_HOST" sh "$(cd "$(dirname "$0")/.." && pwd)/tools/buildhost.sh")
SECS=$1; HSCRIPT=$2; CSCRIPT=$3; OUT=$4
SHOT_MS=${SHOT_MS:-1000}

cat > /tmp/nfs-lan-remote.sh <<EOF
#!/bin/sh
cd /tmp/nfsx86
rm -rf shots_h shots_c
mkdir -p shots_h shots_c save_h save_c
# Seed both instances from the known-good save, otherwise each of them opens
# the first-run language picker instead of the main menu.
[ -f save/gamesett ] && cp -f save/gamedata save/gamesett save_h/ && cp -f save/gamedata save/gamesett save_c/
COMMON="NFS_NO_SENSORS=1 SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LANG=en_US.UTF-8 NFS_VSYNC=0"
env \$COMMON NFS_SHOT_DIR=/tmp/nfsx86/shots_h NFS_SHOT_MS=$SHOT_MS \\
    NFS_MP_NAME=HOSTPC NFS_INPUT_SCRIPT='$HSCRIPT' \\
    ./build/harbour-nfsshift --data /tmp/nfsx86/data --user /tmp/nfsx86/save_h --no-rotate > host.log 2>&1 &
sleep 2
env \$COMMON NFS_SHOT_DIR=/tmp/nfsx86/shots_c NFS_SHOT_MS=$SHOT_MS \\
    NFS_MP_NAME=CLIENTPC NFS_MP_PORT=45480 NFS_MP_HOST=127.0.0.1:45471 NFS_INPUT_SCRIPT='$CSCRIPT' \\
    ./build/harbour-nfsshift --data /tmp/nfsx86/data --user /tmp/nfsx86/save_c --no-rotate > client.log 2>&1 &
sleep $SECS
pkill -9 -f 'build/harbour-nfsshift' || true
sleep 1
exit 0
EOF
scp -q /tmp/nfs-lan-remote.sh "$HOST:/tmp/nfs-lan-remote.sh"
ssh "$HOST" "sh /tmp/nfs-lan-remote.sh"

for who in h c; do
    mkdir -p "$OUT-$who.d" && rm -f "$OUT-$who.d"/*
    ssh "$HOST" "cd /tmp/nfsx86/shots_$who && tar -cf - ." | tar -xf - -C "$OUT-$who.d"
done
scp -q "$HOST:/tmp/nfsx86/host.log" "$OUT-host.log"
scp -q "$HOST:/tmp/nfsx86/client.log" "$OUT-client.log"

python3 - "$OUT" <<'PY'
import glob, sys
from PIL import Image
out = sys.argv[1]
for who in ('h', 'c'):
    fs = sorted(glob.glob('%s-%s.d/shot*.ppm' % (out, who)))
    W, H = 480, 360
    for page in range(0, len(fs), 12):
        ims = [Image.open(f).resize((W, H)) for f in fs[page:page+12]]
        sheet = Image.new('RGB', (W * 3, H * ((len(ims) + 2) // 3)))
        for i, im in enumerate(ims):
            sheet.paste(im, ((i % 3) * W, (i // 3) * H))
        sheet.save('%s-%s_%02d.png' % (out, who, page // 12))
    print(who, len(fs), 'frames')
PY
echo "--- host ---";   grep -a '\[mp\]' "$OUT-host.log"   | head -40 || true
echo "--- client ---"; grep -a '\[mp\]' "$OUT-client.log" | head -40 || true
