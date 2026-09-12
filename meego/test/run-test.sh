#!/bin/sh
# Baut libnfsmp.so (Plattformschicht + echte Patchtabellen + Kern-Attrappe) und
# den fakeloader mit MADDE auf dem Build-Rechner und laesst beides unter
# qemu-arm gegen den Harmattan-Sysroot laufen — einmal ohne und einmal mit
# LD_PRELOAD.
#
# Getestet wird die Plattformschicht, nicht der Protokollkern: die 29 Stubs,
# die 3 bl-Hooks und mp_init/mp_pump kommen aus meego/test/core_dummy.c und
# protokollieren nur, was preload.c ihnen uebergibt.  Die Patchtabellen sind
# die echten aus src/mp/patches.c (Attrappe nur, falls die fehlt).
#
# Exit-Code 0 = alle Pruefungen des fakeloader bestanden.
set -e

HOST=$(BUILD_HOST="$BUILD_HOST" sh "$(cd "$(dirname "$0")/../.." && pwd)/tools/buildhost.sh")
REMOTE=${REMOTE_DIR:-/tmp/nfsmp-meego-test}
TARGET=${MADDE_TARGET:-harmattan_10.2011.34-1_rt1.2}
SYSROOT_NAME=${MADDE_SYSROOT:-harmattan_sysroot_10.2011.34-1_slim}
HERE=$(cd "$(dirname "$0")/../.." && pwd)

tar -C "$HERE" -cf - meego src/mp |
    ssh "$HOST" "rm -rf $REMOTE && mkdir -p $REMOTE && tar -xf - -C $REMOTE"

ssh "$HOST" "REMOTE='$REMOTE' TARGET='$TARGET' SYSROOT_NAME='$SYSROOT_NAME' sh -s" <<'REMOTE_SCRIPT'
set -e
cd "$REMOTE"
CC="$HOME/QtSDK/Madde/targets/$TARGET/bin/gcc"
READELF="$HOME/QtSDK/Madde/targets/$TARGET/bin/readelf"
SYSROOT="$HOME/QtSDK/Madde/sysroots/$SYSROOT_NAME"
CFLAGS="-O2 -Wall -Wextra -std=gnu99 -marm -Isrc/mp -Imeego -Imeego/test"

TABLES=src/mp/patches.c
if ! grep -l 'mp_stubs\[\]' $TABLES >/dev/null 2>&1; then
    echo "WARNUNG: src/mp/patches.c fehlt — benutze meego/stub_table_dummy.c"
    TABLES=meego/stub_table_dummy.c
fi
echo "Patchtabellen: $TABLES"

echo "== libnfsmp.so (preload + Tabellen + Kern-Attrappe) =="
$CC -shared -fPIC $CFLAGS -o libnfsmp.so \
    meego/preload.c $TABLES meego/test/core_dummy.c \
    -ldl -lpthread -lrt

echo "== fakeloader =="
$CC $CFLAGS -o fakeloader meego/test/fakeloader.c $TABLES -ldl \
    -Wl,--section-start=.imgglob=0x7c328 \
    -Wl,--section-start=.nexpglob=0x6be38

"$READELF" -h libnfsmp.so | grep -E 'Class|Machine|Flags'
"$READELF" -d libnfsmp.so | grep NEEDED
"$READELF" -A libnfsmp.so | grep -E 'Tag_CPU_name|Tag_FP_arch|Tag_ABI_VFP_args' || true

echo
echo "=== Lauf 1: ohne LD_PRELOAD ==="
qemu-arm -L "$SYSROOT" ./fakeloader

echo
echo "=== Lauf 2: mit LD_PRELOAD ==="
qemu-arm -L "$SYSROOT" -E LD_PRELOAD="$REMOTE/libnfsmp.so" ./fakeloader
rc=$?
echo "fakeloader-Exitcode: $rc"
exit $rc
REMOTE_SCRIPT
