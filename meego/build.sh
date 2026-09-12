#!/bin/sh
# Baut libnfsmp.so (LD_PRELOAD-Patcher fuer den originalen N9-Loader) mit der
# MADDE-Toolchain auf dem Build-Rechner und holt das Ergebnis nach
# build/meego/.
#
# Toolchain und Flags: meego-runtime-patch.md §6.  Der MADDE-gcc-Wrapper
# (gcc 4.4.1, arm-none-linux-gnueabi) haengt Sysroot und specs selbst an,
# es sind keine Umgebungsvariablen noetig.  Ergebnis ist hardfp; das ist
# richtig so, siehe §6.3 und den ABI-Hinweis in preload.c.
#
# Umgebung:
#   BUILD_HOST   Standard sebastian@192.168.1.21
#   REMOTE_DIR   Standard /tmp/nfsmp-meego   (Root dort ist fast voll!)
#   MADDE_TARGET Standard harmattan_10.2011.34-1_rt1.2
set -e

HOST=$(BUILD_HOST="$BUILD_HOST" sh "$(cd "$(dirname "$0")/.." && pwd)/tools/buildhost.sh")
REMOTE=${REMOTE_DIR:-/tmp/nfsmp-meego}
TARGET=${MADDE_TARGET:-harmattan_10.2011.34-1_rt1.2}
HERE=$(cd "$(dirname "$0")/.." && pwd)

echo "== Quellen nach $HOST:$REMOTE uebertragen =="
tar -C "$HERE" -cf - meego src/mp |
    ssh "$HOST" "rm -rf $REMOTE && mkdir -p $REMOTE && tar -xf - -C $REMOTE"

ssh "$HOST" "REMOTE='$REMOTE' TARGET='$TARGET' sh -s" <<'REMOTE_SCRIPT'
set -e
cd "$REMOTE"
CC="$HOME/QtSDK/Madde/targets/$TARGET/bin/gcc"
READELF="$HOME/QtSDK/Madde/targets/$TARGET/bin/readelf"
test -x "$CC" || { echo "MADDE-gcc nicht gefunden: $CC" >&2; exit 1; }

# Kernquellen einsammeln; fehlende Teile durch die Attrappen ersetzen.
CORE=""
for f in src/mp/*.c; do [ -e "$f" ] && CORE="$CORE $f"; done

if [ -z "$CORE" ] || ! grep -l 'mp_stubs\[\]' $CORE >/dev/null 2>&1; then
    echo "WARNUNG: keine Datei in src/mp definiert mp_stubs[] —"
    echo "         benutze die Attrappe meego/stub_table_dummy.c."
    CORE="$CORE meego/stub_table_dummy.c"
fi
if [ -z "$CORE" ] || ! grep -l 'mp_pump' $CORE >/dev/null 2>&1; then
    echo "WARNUNG: kein Protokollkern in src/mp gefunden —"
    echo "         benutze die Test-Attrappe meego/test/core_dummy.c."
    CORE="$CORE meego/test/core_dummy.c"
    EXTRA_INC="-Imeego/test"
fi

echo "== Uebersetze =="
echo "   $CORE meego/preload.c"
set -x
$CC -shared -fPIC -O2 -Wall -Wextra -std=gnu99 -marm \
    -Isrc/mp -Imeego $EXTRA_INC \
    -o libnfsmp.so meego/preload.c $CORE \
    -ldl -lpthread -lrt
set +x

"$READELF" -hd libnfsmp.so | grep -E 'Class|Machine|Flags|NEEDED'
"$READELF" -A libnfsmp.so | grep -E 'Tag_CPU_name|Tag_CPU_arch:|Tag_FP_arch|Tag_ABI_VFP_args' || true
ls -l libnfsmp.so
REMOTE_SCRIPT

echo "== Ergebnis abholen =="
mkdir -p "$HERE/build/meego"
ssh "$HOST" "cat $REMOTE/libnfsmp.so" > "$HERE/build/meego/libnfsmp.so.new"
mv "$HERE/build/meego/libnfsmp.so.new" "$HERE/build/meego/libnfsmp.so"
ls -la "$HERE/build/meego/libnfsmp.so"
