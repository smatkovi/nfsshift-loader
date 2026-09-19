#!/bin/sh
# Baut harbour-nfsshift als RPM fuer Sailfish OS.
#
# Der Bau laeuft auf dem Build-Rechner im Docker-Container mit dem Sailfish-SDK
# (rpmbuild im sb2-Target).  Das fertige Paket landet lokal unter
# <projekt>/build/rpm/.
#
#   packaging/sfos/build-rpm.sh                 # vollstaendiger Bau aus dem Quelltext
#   packaging/sfos/build-rpm.sh --prebuilt      # fertiges Binary aus dem Container uebernehmen
#   packaging/sfos/build-rpm.sh --check         # nur .spec und .desktop pruefen
#   packaging/sfos/build-rpm.sh --gamedata DIR  # Spieldaten von HIER mit einpacken
#   packaging/sfos/build-rpm.sh --gamedata-host DIR   # ... aus DIR auf dem Build-Rechner
#
# Mit --gamedata/--gamedata-host entsteht das private Vollpaket
# harbour-nfsshift-<ver>-1full: Loader und die rund 100 MB Spieldaten von
# Electronic Arts in einer Datei, nichts muss mehr importiert werden.  Es ist
# fuer die eigenen Geraete und darf nicht weitergegeben werden.
# --gamedata-host spart die 100 MB Uebertragung, wenn die Daten auf dem
# Build-Rechner ohnehin schon liegen (z. B. /tmp/nfsx86/data).
#
# Umgebungsvariablen:
#   BUILD_HOST     Vorgabe sebastian@192.168.1.21
#   SDK_CONTAINER  Vorgabe sfossdk52 (armv7hl: sfossdk52-0ad-arm, dessen Target liegt auf p7)
#   SDK_TARGET     Vorgabe SailfishOS-5.2.0.15-aarch64 (oder ...-armv7hl: nativ, ohne dynarmic)
#   JOBS           Vorgabe 16
#
# Alle Zwischenstaende liegen im Container unter /home/mersdk/rpmbuild-nfsshift
# und werden nach jedem Lauf wieder geloescht (die Root-Partition des
# Build-Rechners ist zu 97 % voll).  Das Skript ist idempotent: es raeumt am
# Anfang auf und ueberschreibt das Ergebnis.
set -e

HOST=$(BUILD_HOST="$BUILD_HOST" sh "$(cd "$(dirname "$0")/../.." && pwd)/tools/buildhost.sh")
CONTAINER=${SDK_CONTAINER:-sfossdk52}
TARGET=${SDK_TARGET:-SailfishOS-5.2.0.15-aarch64}
JOBS=${JOBS:-16}

HERE=$(cd "$(dirname "$0")/../.." && pwd)      # Projektwurzel
PROJ=$(basename "$HERE")
SPEC="$HERE/packaging/sfos/harbour-nfsshift.spec"
OUT="$HERE/build/rpm"

MODE=full
GAMEDATA=
GAMEDATA_HOST=

while [ $# -gt 0 ]; do
    case "$1" in
        --prebuilt) MODE=prebuilt ;;
        --check)    MODE=check ;;
        --full)     MODE=full ;;
        --gamedata) GAMEDATA=$2; shift ;;
        --gamedata-host) GAMEDATA_HOST=$2; shift ;;
        -h|--help)  sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "unbekannte Option: $1" >&2; exit 2 ;;
    esac
    shift
done

NAME=$(sed -n 's/^Name: *//p' "$SPEC" | head -1)
VERSION=$(sed -n 's/^Version: *//p' "$SPEC" | head -1)
RELEASE=$(sed -n 's/^Release: *//p' "$SPEC" | head -1)
[ -n "$NAME" ] && [ -n "$VERSION" ] || { echo "Name/Version nicht aus $SPEC lesbar" >&2; exit 1; }
# %{?relsuffix} steht so in der .spec; hier ausschreiben, damit die Zeile den
# Paketnamen zeigt, der wirklich herauskommt.
if [ -n "$GAMEDATA" ] || [ -n "$GAMEDATA_HOST" ]; then
    RELEASE=$(echo "$RELEASE" | sed 's/%{?relsuffix}/full/')
else
    RELEASE=$(echo "$RELEASE" | sed 's/%{?relsuffix}//')
fi
echo "== $NAME-$VERSION-$RELEASE  ($MODE, $TARGET)"

# --- 1. Quelltext in den Container spiegeln ---------------------------------
# Zweistufig: rsync auf den Build-Rechner, von dort mit tar in den Container
# (der sieht das Host-Dateisystem nicht).  Warum nicht wie frueher eine einzige
# tar-Pipe vom Telefon in den Container: ueber den cloudflared-Tunnel laeuft
# das mit rund 1 MB/s, und ein haengender Tunnel liess die Pipe minutenlang
# stehen.  rsync uebertraegt nur die Aenderungen und ein Abbruch kostet nicht
# den ganzen Baum; der zweite Schritt ist auf dem Build-Rechner lokal.
STAGE_HOST=${SRC_STAGE:-/tmp/nfssrc}
echo "== Quelltext -> $HOST:$STAGE_HOST/$PROJ (rsync)"
rsync -a --delete --exclude=build --exclude=.git \
      --rsync-path="mkdir -p '$STAGE_HOST/$PROJ' && rsync" \
      "$HERE/" "$HOST:$STAGE_HOST/$PROJ/"
echo "== $STAGE_HOST/$PROJ -> $CONTAINER:/home/mersdk/nfsloader/$PROJ"
ssh "$HOST" "tar -C '$STAGE_HOST' -cf - '$PROJ' |
    docker exec -i $CONTAINER sh -c 'mkdir -p /home/mersdk/nfsloader && tar -xf - -C /home/mersdk/nfsloader'"

[ -n "$GAMEDATA" ] && [ -n "$GAMEDATA_HOST" ] &&
    { echo "--gamedata und --gamedata-host schliessen sich aus" >&2; exit 2; }

if [ -n "$GAMEDATA" ]; then
    [ -f "$GAMEDATA/NFSShift.s3e" ] || { echo "$GAMEDATA enthaelt keine NFSShift.s3e" >&2; exit 1; }
    echo "== Spieldaten -> Container (rund 100 MB, dauert)"
    tar -C "$GAMEDATA" -cf - . |
        ssh "$HOST" "docker exec -i $CONTAINER sh -c '
            rm -rf /home/mersdk/nfsloader/gamedata &&
            mkdir -p /home/mersdk/nfsloader/gamedata &&
            tar -xf - -C /home/mersdk/nfsloader/gamedata'"
fi

if [ -n "$GAMEDATA_HOST" ]; then
    echo "== Spieldaten $GAMEDATA_HOST (auf $HOST) -> Container"
    ssh "$HOST" "
        [ -f '$GAMEDATA_HOST/NFSShift.s3e' ] ||
            { echo '$GAMEDATA_HOST enthaelt keine NFSShift.s3e' >&2; exit 1; }
        tar -C '$GAMEDATA_HOST' -cf - . | docker exec -i $CONTAINER sh -c '
            rm -rf /home/mersdk/nfsloader/gamedata &&
            mkdir -p /home/mersdk/nfsloader/gamedata &&
            tar -xf - -C /home/mersdk/nfsloader/gamedata'"
    GAMEDATA=$GAMEDATA_HOST
fi

# --- 2. Bauen ---------------------------------------------------------------
mkdir -p "$OUT"

ssh "$HOST" "docker exec -i \
        -e TARGET='$TARGET' -e PROJ='$PROJ' -e NAME='$NAME' -e VERSION='$VERSION' \
        -e MODE='$MODE' -e JOBS='$JOBS' -e GAMEDATA='$GAMEDATA' \
        $CONTAINER bash -s" <<'REMOTE'
set -e
SRC=/home/mersdk/nfsloader/$PROJ
DYN=/home/mersdk/nfsloader/dynarmic
TOP=/home/mersdk/rpmbuild-nfsshift
STAGE=$TOP/stage/$NAME-$VERSION
SPEC=$SRC/packaging/sfos/$NAME.spec

# armv7hl fuehrt den Spielcode nativ aus und braucht dynarmic nicht.
case "$TARGET" in *armv7hl*) DYN= ;; esac
[ -z "$DYN" ] || [ -d "$DYN" ] || { echo "dynarmic fehlt unter $DYN"; exit 1; }

# --- Syntaxpruefung der .spec ---------------------------------------------
echo "-- rpm -q --specfile"
rpm -q --specfile "$SPEC" || { echo "spec fehlerhaft"; exit 1; }
if command -v desktop-file-validate >/dev/null 2>&1; then
    echo "-- desktop-file-validate"
    desktop-file-validate "$SRC/packaging/sfos/$NAME.desktop"
    echo "   .desktop ok"
else
    echo "-- desktop-file-validate nicht vorhanden, uebersprungen"
fi
if [ "$MODE" = check ]; then echo "-- nur Pruefung, fertig"; exit 0; fi

# --- Aufraeumen und Quelltarball bauen ------------------------------------
# RPMS mitraeumen, sonst bliebe z. B. ein harbour-nfsshift-data aus einem
# frueheren --gamedata-Lauf liegen und wuerde wieder mit zurueckgeholt.
rm -rf $TOP/BUILD $TOP/BUILDROOT $TOP/stage $TOP/SOURCES $TOP/RPMS
mkdir -p $TOP/BUILD $TOP/BUILDROOT $TOP/RPMS $TOP/SOURCES $TOP/SPECS $TOP/SRPMS "$STAGE"

cp -a $SRC/CMakeLists.txt $SRC/src $SRC/third_party $SRC/packaging "$STAGE/"
if [ -n "$DYN" ]; then
    cp -a $DYN "$STAGE/dynarmic"
    rm -rf "$STAGE/dynarmic/build" "$STAGE/dynarmic/.git"
fi

EXTRA=""
if [ "$MODE" = prebuilt ]; then
    [ -f "$SRC/build/harbour-nfsshift" ] || {
        echo "Kein fertiges Binary unter $SRC/build - erst tools/build.sh laufen lassen."; exit 1; }
    mkdir -p "$STAGE/prebuilt"
    cp -a "$SRC/build/harbour-nfsshift" "$STAGE/prebuilt/"
    EXTRA="$EXTRA --with prebuilt"
fi
if [ -n "$GAMEDATA" ]; then
    cp -a /home/mersdk/nfsloader/gamedata "$STAGE/gamedata"
    EXTRA="$EXTRA --with gamedata"
fi

tar -C $TOP/stage -czf "$TOP/SOURCES/$NAME-$VERSION.tar.gz" "$NAME-$VERSION"
cp "$SPEC" "$TOP/SPECS/"
echo "-- Quelltarball $(du -h "$TOP/SOURCES/$NAME-$VERSION.tar.gz" | cut -f1)"

# Nur die Architektur: mit dem vollen Tripel (armv7hl-meego-linux-gnueabi)
# findet rpm seine Plattform-Makros nicht, %{_arch} bleibt dann unexpandiert
# und die Dateiliste geht ins Leere.
BUILD_TGT=${TARGET##*-}
echo "-- rpmbuild --target=$BUILD_TGT $EXTRA (nice -n ${NICENESS:-15}, -j$JOBS)"
set +e
# Der Build-Rechner wird nebenher benutzt: der ganze Bau laeuft freundlich,
# make/cmake unterhalb von rpmbuild erben die Nettigkeit.
nice -n ${NICENESS:-15} sb2 -t "$TARGET" -- rpmbuild \
    --define "_topdir $TOP" \
    --define "_smp_mflags -j$JOBS" \
    --define "_smp_build_ncpus $JOBS" \
    --define "_rpmfilename %%{name}-%%{version}-%%{release}.%%{arch}.rpm" \
    --target="$BUILD_TGT" \
    $EXTRA -bb "$TOP/SPECS/$NAME.spec" > $TOP/rpmbuild.log 2>&1
RC=$?
set -e
if [ $RC -ne 0 ]; then
    echo "-- rpmbuild fehlgeschlagen ($RC), letzte 60 Zeilen:"
    tail -60 $TOP/rpmbuild.log
    exit 1
fi
grep -E "^(Wrote|warning:|error:)" $TOP/rpmbuild.log || true

echo
echo "===== rpm -qpi ====="
for r in $TOP/RPMS/*.rpm; do rpm -qpi "$r"; echo; done
echo "===== rpm -qpl ====="
for r in $TOP/RPMS/*.rpm; do echo "--- $(basename "$r")"; rpm -qpl "$r"; done
echo "===== rpm -qp --requires ====="
for r in $TOP/RPMS/*.rpm; do echo "--- $(basename "$r")"; rpm -qp --requires "$r"; done
echo "===== rpm -qp --scripts ====="
for r in $TOP/RPMS/*.rpm; do rpm -qp --scripts "$r"; done
echo "===== Groessen ====="
ls -la $TOP/RPMS/

# Platz auf der (fast vollen) Root-Partition wieder freigeben.
rm -rf $TOP/BUILD $TOP/BUILDROOT $TOP/stage $TOP/SOURCES /home/mersdk/nfsloader/gamedata
df -h / | tail -1
REMOTE

if [ "$MODE" = check ]; then exit 0; fi

# --- 3. Ergebnis zurueckholen ----------------------------------------------
# Erst aus dem Container auf den Build-Rechner (lokal), dann von dort per rsync
# hierher: mit --partial ueberlebt ein Vollpaket von rund 100 MB auch einen
# Tunnel, der mittendrin abbricht -- der naechste Lauf setzt fort.  Die Kopie
# unter $STAGE_HOST/out bleibt liegen, von dort geht sie auch ins private Repo.
[ -t 2 ] && RSYNC_PROGRESS=--info=progress2 || RSYNC_PROGRESS=
echo "== hole RPMs nach $OUT"
LIST=$(ssh "$HOST" "docker exec $CONTAINER sh -c 'ls /home/mersdk/rpmbuild-nfsshift/RPMS/*.rpm'")
ssh "$HOST" "mkdir -p '$STAGE_HOST/out'"
for r in $LIST; do
    b=$(basename "$r")
    ssh "$HOST" "docker exec $CONTAINER cat '$r' > '$STAGE_HOST/out/$b'"
    rsync -a --partial $RSYNC_PROGRESS "$HOST:$STAGE_HOST/out/$b" "$OUT/$b"
    echo "   $OUT/$b  ($(wc -c < "$OUT/$b") B)"
done

# Lokale Gegenprobe (rpm ist auch auf dem Geraet vorhanden).
if command -v rpm >/dev/null 2>&1; then
    for b in $LIST; do
        echo "== rpm -qpi $OUT/$(basename "$b")"
        rpm -qpi "$OUT/$(basename "$b")" 2>&1 | head -20
    done
fi
echo "== fertig"
