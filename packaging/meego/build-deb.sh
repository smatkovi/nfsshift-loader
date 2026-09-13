#!/bin/sh
# Baut das Harmattan-Zusatzpaket nfsshift-mp fuer die Nokia N9.
#
# Das Paket laesst das installierte Originalspiel unberuehrt und legt nur
#   /opt/nfsshift-mp/libnfsmp.so        die LD_PRELOAD-Bibliothek
#   /opt/nfsshift-mp/startup-mp.sh      Starter mit LD_PRELOAD
#   /usr/share/applications/nfsshift-mp.desktop
#   /usr/share/themes/base/meegotouch/icons/nfsshift-mp-80.png
# daneben.  Es enthaelt weder Spieldaten noch Spielcode.
#
#   packaging/meego/build-deb.sh                 # nimmt build/meego/libnfsmp.so
#   packaging/meego/build-deb.sh --so PFAD       # andere Bibliothek
#   packaging/meego/build-deb.sh --stub          # Platzhalter erzwingen
#   packaging/meego/build-deb.sh --version 0.2.0
#
# Fehlt build/meego/libnfsmp.so, baut das Skript einen Platzhalter: bevorzugt
# eine echte, aber leere ARM-.so mit der MADDE-Toolchain auf dem Build-Rechner
# (BUILD_HOST), sonst eine inerte Datei.  In beiden Faellen bekommt die Version
# den Zusatz "~stub", damit so ein Paket nie mit einem echten verwechselt wird.
#
# Ergebnis: build/meego/nfsshift-mp_<version>_armel.deb
# Das Skript ist idempotent und arbeitet ausschliesslich unter /tmp bzw. im
# build-Verzeichnis des Projekts.
set -e

HERE=$(cd "$(dirname "$0")/../.." && pwd)          # Projektwurzel
PKGDIR="$HERE/packaging/meego"
OUTDIR="$HERE/build/meego"
HOST=$(BUILD_HOST="$BUILD_HOST" sh "$(cd "$(dirname "$0")/../.." && pwd)/tools/buildhost.sh")
MADDE=${MADDE_GCC:-\$HOME/QtSDK/Madde/targets/harmattan_10.2011.34-1_rt1.2/bin/gcc}

VERSION=${VERSION:-0.1.1}
SO=
FORCE_STUB=

while [ $# -gt 0 ]; do
    case "$1" in
        --so)      SO=$2; shift ;;
        --version) VERSION=$2; shift ;;
        --stub)    FORCE_STUB=1 ;;
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
        *) echo "unbekannte Option: $1" >&2; exit 2 ;;
    esac
    shift
done

[ -n "$SO" ] || SO="$OUTDIR/libnfsmp.so"
mkdir -p "$OUTDIR"

STAGE=${TMPDIR:-/tmp}/nfsshift-mp-stage.$$
STUBERR=${TMPDIR:-/tmp}/nfsshift-mp-stub.$$.err
trap 'rm -rf "$STAGE" "$STUBERR"' EXIT INT TERM
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" \
         "$STAGE/opt/nfsshift-mp" \
         "$STAGE/usr/share/applications" \
         "$STAGE/usr/share/themes/base/meegotouch/icons" \
         "$STAGE/usr/share/doc/nfsshift-mp"

# --- 1. Bibliothek besorgen -------------------------------------------------
make_remote_stub() {
    # Eine echte, hardfp gebaute ARM-.so ohne Funktion - nur damit das Paket
    # baubar und installierbar ist, bevor mp/nfsmp.c fertig ist.
    ssh "$HOST" "set -e
        d=/tmp/nfsshift-mp-stub
        mkdir -p \$d
        cat > \$d/stub.c <<'EOF'
/* Platzhalter fuer libnfsmp.so - tut nichts ausser einer Startmeldung. */
#include <stdio.h>
__attribute__((constructor)) static void nfsmp_stub_init(void)
{
    fprintf(stderr, \"[nfsmp] Platzhalterbibliothek, kein LAN-Code enthalten\\n\");
}
EOF
        $MADDE -shared -fPIC -O2 -Wall -o \$d/libnfsmp.so \$d/stub.c
        cat \$d/libnfsmp.so
        rm -rf \$d" > "$STAGE/opt/nfsshift-mp/libnfsmp.so"
}

if [ -z "$FORCE_STUB" ] && [ -f "$SO" ]; then
    cp "$SO" "$STAGE/opt/nfsshift-mp/libnfsmp.so"
    echo "== Bibliothek: $SO ($(wc -c < "$SO") B)"
else
    if [ -n "$FORCE_STUB" ]; then
        echo "== Platzhalter erzwungen (--stub)"
    else
        echo "== $SO fehlt - Platzhalter wird gebaut"
    fi
    VERSION="$VERSION~stub"
    if make_remote_stub 2>"$STUBERR" && \
       [ -s "$STAGE/opt/nfsshift-mp/libnfsmp.so" ]; then
        echo "   echte ARM-.so von $HOST (MADDE gcc 4.4.1)"
    else
        echo "   MADDE nicht erreichbar ($(tail -1 "$STUBERR" 2>/dev/null))"
        echo "   -> inerter Platzhalter"
        printf 'PLACEHOLDER libnfsmp.so - kein LAN-Code. Erst nach dem Bau von\nsrc/mp gegen die MADDE-Toolchain ersetzen.\n' \
            > "$STAGE/opt/nfsshift-mp/libnfsmp.so"
    fi
fi
chmod 755 "$STAGE/opt/nfsshift-mp/libnfsmp.so"

# --- 2. uebrige Dateien -----------------------------------------------------
cp "$PKGDIR/startup-mp.sh"       "$STAGE/opt/nfsshift-mp/startup-mp.sh"
chmod 755                        "$STAGE/opt/nfsshift-mp/startup-mp.sh"
cp "$PKGDIR/nfsshift-mp.desktop" "$STAGE/usr/share/applications/nfsshift-mp.desktop"
cp "$PKGDIR/icons/nfsshift-mp-80.png" \
   "$STAGE/usr/share/themes/base/meegotouch/icons/nfsshift-mp-80.png"
# Debian-Changelog: gzip -n, damit zwei Laeufe dieselbe Datei liefern (kein
# Name, kein Zeitstempel im Kopf).
gzip -9nc "$PKGDIR/changelog" > "$STAGE/usr/share/doc/nfsshift-mp/changelog.gz"
chmod 644 "$STAGE/usr/share/applications/nfsshift-mp.desktop" \
          "$STAGE/usr/share/themes/base/meegotouch/icons/nfsshift-mp-80.png" \
          "$STAGE/usr/share/doc/nfsshift-mp/changelog.gz"

# --- 3. control aus control.in ----------------------------------------------
# Maemo-Icon-26 ist ein 64x64-PNG als base64, Folgezeilen mit genau einem
# fuehrenden Leerzeichen.  Ohne dieses Feld zeigt der App-Manager kein Symbol.
VERSION="$VERSION" ICON="$PKGDIR/icons/nfsshift-mp-64.png" \
python3 - "$PKGDIR/control.in" "$STAGE/DEBIAN/control" <<'PY'
import base64, os, sys, textwrap
src, dst = sys.argv[1], sys.argv[2]
with open(os.environ["ICON"], "rb") as f:
    b64 = base64.b64encode(f.read()).decode("ascii")
icon = "\n".join(" " + line for line in textwrap.wrap(b64, 76))
with open(src, "r", encoding="utf-8") as f:
    ctl = f.read()
ctl = ctl.replace("@VERSION@", os.environ["VERSION"]).replace("@ICON@", icon)
with open(dst, "w", encoding="utf-8") as f:
    f.write(ctl)
print("== control: Version %s, Icon %d base64-Zeilen"
      % (os.environ["VERSION"], icon.count("\n") + 1))
PY

# --- 4. .deb schnueren ------------------------------------------------------
DEB="$OUTDIR/nfsshift-mp_${VERSION}_armel.deb"
python3 "$PKGDIR/mkdeb.py" "$STAGE" "$DEB"

# --- 5. Pruefen -------------------------------------------------------------
echo
if command -v dpkg-deb >/dev/null 2>&1; then
    echo "===== dpkg-deb -I ====="; dpkg-deb -I "$DEB"
    echo "===== dpkg-deb -c ====="; dpkg-deb -c "$DEB"
else
    echo "(dpkg-deb ist hier nicht vorhanden)"
fi
if command -v ar >/dev/null 2>&1; then
    echo "===== ar t ====="; ar t "$DEB"
fi
echo
python3 "$PKGDIR/mkdeb.py" --info "$DEB"
echo
echo "== fertig: $DEB"
echo "   Installieren auf der N9 (Developer Mode):  devel-su dpkg -i $(basename "$DEB")"
