#!/bin/sh
# Richtet die von der Android-Aufgabe gebaute APK fuer die Weitergabe her:
# zipalign, signieren (v1+v2+v3), pruefen und unter einem Namen mit Version
# ablegen.
#
#   packaging/android/sign-apk.sh                       # /tmp/android/out/nfsshift.apk
#   packaging/android/sign-apk.sh --apk /tmp/android/out/nfsshift-test.apk
#   packaging/android/sign-apk.sh --keystore ~/keys/nfsshift.jks --alias nfsshift
#   packaging/android/sign-apk.sh --local               # direkt auf dem Build-Rechner
#
# Umgebungsvariablen:
#   BUILD_HOST        Vorgabe sebastian@192.168.1.21
#   ANDROID_ROOT      Vorgabe /tmp/android (NDK/SDK/SDL2 der Android-Aufgabe)
#   NFSSHIFT_KS_PASS  Passwort fuer Keystore und Schluessel (Vorgabe "android")
#
# Der Keystore liegt bewusst NICHT unter /tmp und nicht im Projekt, sondern
# unter ~/.config/nfsshift/ auf dem Build-Rechner: /tmp ist dort ein tmpfs und
# waere nach einem Neustart weg.  Mit einem verlorenen Schluessel laesst sich
# eine bereits installierte App nicht mehr aktualisieren, sie muesste erst
# deinstalliert werden.  Fehlt der Keystore, wird einer angelegt (RSA 2048,
# 10000 Tage).  Alle Zwischenstaende liegen unter /tmp.
#
# Ergebnis: build/android/<paket>-<versionName>-<versionCode>-arm64-v8a.apk
set -e

HERE=$(cd "$(dirname "$0")/../.." && pwd)
OUTDIR="$HERE/build/android"
HOST=$(BUILD_HOST="$BUILD_HOST" sh "$(cd "$(dirname "$0")/../.." && pwd)/tools/buildhost.sh")
AROOT=${ANDROID_ROOT:-/tmp/android}
KSPASS=${NFSSHIFT_KS_PASS:-android}

APK=
KEYSTORE=
ALIAS=nfsshiftkey
LOCAL=

while [ $# -gt 0 ]; do
    case "$1" in
        --apk)      APK=$2; shift ;;
        --keystore) KEYSTORE=$2; shift ;;
        --alias)    ALIAS=$2; shift ;;
        --local)    LOCAL=1 ;;
        -h|--help)  sed -n '2,23p' "$0"; exit 0 ;;
        *) echo "unbekannte Option: $1" >&2; exit 2 ;;
    esac
    shift
done

BODY=${TMPDIR:-/tmp}/nfsshift-sign.$$.sh
LOG=${TMPDIR:-/tmp}/nfsshift-sign.$$.log
trap 'rm -f "$BODY" "$LOG"' EXIT INT TERM

cat > "$BODY" <<'REMOTE'
set -e
AROOT=${AROOT:-/tmp/android}
WORK=/tmp/nfsshift-apk
rm -rf "$WORK"; mkdir -p "$WORK"

# --- Werkzeuge --------------------------------------------------------------
BT=$(ls -d "$AROOT"/sdk/build-tools/*/ 2>/dev/null | sort -V | tail -1)
[ -n "$BT" ] || { echo "Keine build-tools unter $AROOT/sdk/build-tools."; \
                  echo "Android-Umgebung fehlt - $AROOT/build-test-apk.sh wiederherstellen."; exit 1; }
ZIPALIGN="$BT/zipalign"; APKSIGNER="$BT/apksigner"; AAPT2="$BT/aapt2"
for t in "$ZIPALIGN" "$APKSIGNER" "$AAPT2"; do
    [ -x "$t" ] || { echo "fehlt: $t"; exit 1; }
done
echo "== build-tools: $BT"

# --- Eingabe-APK ------------------------------------------------------------
if [ -z "$APK" ]; then
    # Reihenfolge: echte Loader-APK vor der Test-APK aus android-env.md.
    for c in "$AROOT/out-loader/nfsshift.apk" "$AROOT/out/nfsshift.apk" \
             "$AROOT/out-loader/unsigned.apk" "$AROOT/out/nfsshift-unsigned.apk" \
             "$AROOT/out/nfsshift-test.apk"; do
        [ -f "$c" ] && { APK=$c; break; }
    done
fi
[ -n "$APK" ] && [ -f "$APK" ] || { echo "Keine APK gefunden (--apk PFAD)."; exit 1; }
echo "== Eingabe: $APK ($(wc -c < "$APK") B)"

# --- Keystore ---------------------------------------------------------------
[ -n "$KEYSTORE" ] || KEYSTORE="$HOME/.config/nfsshift/nfsshift-release.keystore"
if [ ! -f "$KEYSTORE" ]; then
    echo "== Keystore $KEYSTORE fehlt - wird angelegt"
    mkdir -p "$(dirname "$KEYSTORE")"
    chmod 700 "$(dirname "$KEYSTORE")"
    keytool -genkeypair -keystore "$KEYSTORE" -alias "$ALIAS" \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass "$NFSSHIFT_KS_PASS" -keypass "$NFSSHIFT_KS_PASS" \
        -dname "CN=NFS Shift Loader,O=nfsshift-sfos,C=DE" >/dev/null
    chmod 600 "$KEYSTORE"
    echo "   BITTE SICHERN: ohne diesen Schluessel sind keine Updates moeglich."
fi
echo "== Keystore: $KEYSTORE (Alias $ALIAS)"

# --- zipalign + signieren ---------------------------------------------------
"$ZIPALIGN" -f -p 4 "$APK" "$WORK/aligned.apk"
"$APKSIGNER" sign \
    --ks "$KEYSTORE" --ks-key-alias "$ALIAS" \
    --ks-pass env:NFSSHIFT_KS_PASS --key-pass env:NFSSHIFT_KS_PASS \
    --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true \
    --out "$WORK/signed.apk" "$WORK/aligned.apk"

echo "== apksigner verify"
"$APKSIGNER" verify --min-sdk-version 21 -v "$WORK/signed.apk" | sed 's/^/   /'

# --- Version aus dem Manifest -----------------------------------------------
BADGING=$("$AAPT2" dump badging "$WORK/signed.apk" | head -1)
echo "== $BADGING"
# Immer den ERSTEN Treffer nehmen: in derselben Zeile steht weiter hinten
# noch "compileSdkVersionCodename='15'", auf das ein gieriges sed-".*name='"
# hereinfallen wuerde.
PKG=$(echo "$BADGING" | grep -o "name='[^']*'"        | head -1 | cut -d"'" -f2)
VC=$( echo "$BADGING" | grep -o "versionCode='[^']*'" | head -1 | cut -d"'" -f2)
VN=$( echo "$BADGING" | grep -o "versionName='[^']*'" | head -1 | cut -d"'" -f2)
[ -n "$PKG" ] || PKG=nfsshift
[ -n "$VC" ] || VC=0
[ -n "$VN" ] || VN=0.0
# printf statt echo: echo haengt ein \n an, das tr sonst in "_" verwandelt.
VN=$(printf '%s' "$VN" | tr -c 'A-Za-z0-9._-' '_')

REL="$AROOT/release"
mkdir -p "$REL"
NAME="$PKG-$VN-$VC-arm64-v8a.apk"
cp "$WORK/signed.apk" "$REL/$NAME"
sha256sum "$REL/$NAME" > "$REL/$NAME.sha256"
rm -rf "$WORK"
echo "== abgelegt: $REL/$NAME ($(wc -c < "$REL/$NAME") B)"
cat "$REL/$NAME.sha256"
echo "NFSSHIFT_RESULT=$REL/$NAME"
REMOTE

echo "== signiere APK ($([ -n "$LOCAL" ] && echo lokal || echo "auf $HOST"))"
{
    # Passwort und Parameter nur ueber stdin, nie in der Kommandozeile.
    printf 'NFSSHIFT_KS_PASS=%s; export NFSSHIFT_KS_PASS\n' "$KSPASS"
    printf 'APK=%s; KEYSTORE=%s; ALIAS=%s; AROOT=%s\n' "$APK" "$KEYSTORE" "$ALIAS" "$AROOT"
    cat "$BODY"
} | if [ -n "$LOCAL" ]; then sh -s; else ssh "$HOST" "bash -s"; fi | tee "$LOG"

RESULT=$(sed -n 's/^NFSSHIFT_RESULT=//p' "$LOG" | tail -1)
[ -n "$RESULT" ] || { echo "Signieren fehlgeschlagen." >&2; exit 1; }

mkdir -p "$OUTDIR"
BASE=$(basename "$RESULT")
if [ -n "$LOCAL" ]; then
    cp "$RESULT" "$OUTDIR/$BASE.new"
else
    ssh "$HOST" "cat '$RESULT'" > "$OUTDIR/$BASE.new"
fi
mv "$OUTDIR/$BASE.new" "$OUTDIR/$BASE"
echo "== $OUTDIR/$BASE ($(wc -c < "$OUTDIR/$BASE") B)"
echo "   Installieren:  adb install -r $OUTDIR/$BASE"
echo "   Spieldaten:    siehe packaging/android/README.md"
