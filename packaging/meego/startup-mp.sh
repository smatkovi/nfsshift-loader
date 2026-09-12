#!/bin/sh
# Startet das installierte Original-NFS-Shift mit der LAN-Erweiterung.
#
# Das Original startet ueber /opt/ea-mobile-nfsshift/startup.sh mit
#   cd `dirname $0`; exec /opt/usr/bin/ea-mobile-nfsshift $*
# Das Arbeitsverzeichnis ist also /opt/ea-mobile-nfsshift, und genau dort
# sucht das Spiel res.dz und NFSShift.s3e.  Wir machen dasselbe und haengen
# nur libnfsmp.so per LD_PRELOAD davor.  Das Originalpaket bleibt unberuehrt.
#
# Die in digsigsums gelisteten Originaldateien bleiben unangetastet: eigenes
# Verzeichnis, eigenes Startskript, dasselbe Binary.  Weil wir denselben
# Binaerpfad exec'en, bleibt auch die syspart-Einordnung ("classify gaming")
# gueltig.
#
# Umgebung, die libnfsmp.so auswertet:
#   NFSMP_NAME=<Name>   Spielername in Lobby und HUD (sonst $USER, sonst "N9")
#   NFSMP_QUIET=1       keine [nfsmp]-Meldungen auf stderr
#   NFSMP_DISABLE=1     Patcher komplett aus (A/B-Vergleich)
#   NFSMP_FALLBACK=1    Pollthread statt mprotect-Hook
# Vom Skript selbst ausgewertet:
#   NFSMP_LOG=/home/user/nfsmp.log /opt/nfsshift-mp/startup-mp.sh
#     schreibt stdout+stderr des Spiels in eine Datei.  Wichtig beim Start
#     ueber das Menue-Symbol, dort geht stderr sonst verloren.

GAME=/opt/usr/bin/ea-mobile-nfsshift
LIB=/opt/nfsshift-mp/libnfsmp.so

if [ ! -x "$GAME" ]; then
    echo "nfsshift-mp: $GAME fehlt - ist das Originalspiel installiert?" >&2
    exit 1
fi
if [ ! -r "$LIB" ]; then
    echo "nfsshift-mp: $LIB fehlt" >&2
    exit 1
fi

cd /opt/ea-mobile-nfsshift || exit 1

LD_PRELOAD=$LIB
export LD_PRELOAD

if [ -n "$NFSMP_LOG" ]; then
    exec "$GAME" "$@" >"$NFSMP_LOG" 2>&1
fi
exec "$GAME" "$@"
