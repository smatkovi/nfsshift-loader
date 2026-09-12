#!/bin/sh
# Startet den ORIGINALEN N9-Loader mit der vorgeladenen LAN-Bibliothek.
#
# Die in digsigsums gelisteten Originaldateien bleiben unangetastet
# (meego-runtime-patch.md §7): eigenes Verzeichnis, eigenes Startskript,
# dasselbe Binary.  Das Arbeitsverzeichnis muss /opt/ea-mobile-nfsshift
# bleiben, dort liegen res.dz, gamedata und gamesett; und derselbe Binaerpfad
# haelt die syspart-Einordnung ("classify gaming") gueltig.
#
# Umgebung, die libnfsmp.so auswertet:
#   NFSMP_NAME=<Name>   Spielername in Lobby und HUD (sonst $USER, sonst "N9")
#   NFSMP_QUIET=1       keine [nfsmp]-Meldungen auf stderr
#   NFSMP_DISABLE=1     Patcher komplett aus (A/B-Vergleich)
#   NFSMP_FALLBACK=1    Pollthread statt mprotect-Hook (meego-runtime-patch §4.2)
cd /opt/ea-mobile-nfsshift || exit 1
LD_PRELOAD=/opt/nfsshift-mp/libnfsmp.so
export LD_PRELOAD
exec /opt/usr/bin/ea-mobile-nfsshift "$@"
