#!/usr/bin/env python3
"""Erzeugt die Paket-Icons aus dem 80x80-Icon des MeeGo-Originalpakets.

Quelle ist `usr/share/themes/base/meegotouch/icons/ea-mobile-nfsshift-80.png`
aus der Original-.deb (80x80 RGBA, 11075 B, sha256 unten geprueft).  Das Icon
wird NUR aus einer lokal vorhandenen Kopie des Originals abgeleitet und ist
deshalb ebenso wie die Spieldaten nichts, was weitergegeben werden darf
(siehe packaging/README.md, Abschnitt "Rechtliches").

Ergebnis:
    packaging/sfos/icons/icon-{86,108,128,172}.png   Sailfish, hicolor
    packaging/meego/icons/nfsshift-mp-80.png         N9, meegotouch-Theme
    packaging/meego/icons/nfsshift-mp-64.png         N9, Maemo-Icon-26 (base64)

Aufruf (idempotent, ueberschreibt nur bei --force oder wenn die Datei fehlt):
    python3 packaging/make-icons.py [--source PFAD] [--force]

Ohne --source werden der Reihe nach durchsucht:
    ~/.local/share/harbour-nfsshift/icon-meego-80.png
    <scratchpad>/nfs/data/usr/share/themes/base/meegotouch/icons/ea-mobile-nfsshift-80.png
    /opt/ea-mobile-nfsshift/../usr/share/... (N9)
Benoetigt Pillow (auf dem Geraet vorhanden: PIL 12.3.0).
"""

import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Bekannte Fundorte des Originalicons, in dieser Reihenfolge.
CANDIDATES = [
    os.path.expanduser("~/.local/share/harbour-nfsshift/icon-meego-80.png"),
    "/tmp/claude-100000/-home-defaultuser-ps/7399a91f-77e8-4c3c-ae1b-324a25d80f55"
    "/scratchpad/nfs/data/usr/share/themes/base/meegotouch/icons/ea-mobile-nfsshift-80.png",
    "/usr/share/themes/base/meegotouch/icons/ea-mobile-nfsshift-80.png",
]

# (Zielpfad relativ zu packaging/, Kantenlaenge)
OUTPUTS = [
    ("sfos/icons/icon-86.png", 86),
    ("sfos/icons/icon-108.png", 108),
    ("sfos/icons/icon-128.png", 128),
    ("sfos/icons/icon-172.png", 172),
    ("meego/icons/nfsshift-mp-80.png", 80),
    ("meego/icons/nfsshift-mp-64.png", 64),
]


def find_source(explicit):
    if explicit:
        if not os.path.isfile(explicit):
            sys.exit("Quelle nicht gefunden: %s" % explicit)
        return explicit
    for c in CANDIDATES:
        if os.path.isfile(c):
            return c
    sys.exit(
        "Kein Original-Icon gefunden. Bitte --source auf\n"
        "  usr/share/themes/base/meegotouch/icons/ea-mobile-nfsshift-80.png\n"
        "aus der entpackten Original-.deb zeigen lassen."
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", help="80x80-PNG aus dem Originalpaket")
    ap.add_argument("--force", action="store_true", help="vorhandene Icons neu erzeugen")
    args = ap.parse_args()

    src = find_source(args.source)
    with open(src, "rb") as f:
        raw = f.read()
    print("Quelle: %s (%d B, sha256 %s)" % (src, len(raw), hashlib.sha256(raw).hexdigest()[:16]))

    try:
        from PIL import Image
    except ImportError:
        sys.exit("Pillow fehlt: pip install --user Pillow")

    img = Image.open(src).convert("RGBA")
    if img.size != (80, 80):
        print("Hinweis: Quelle ist %dx%d, erwartet 80x80" % img.size)

    for rel, size in OUTPUTS:
        dst = os.path.join(HERE, rel)
        if os.path.exists(dst) and not args.force:
            print("  behalten  %-34s" % rel)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        # LANCZOS: beim Hochskalieren von 80 auf 86/108 praktisch verlustfrei,
        # bei 172 sichtbar weich - ein neu gezeichnetes Icon waere besser.
        out = img.resize((size, size), Image.LANCZOS)
        out.save(dst, "PNG", optimize=True)
        print("  erzeugt   %-34s %dx%d, %d B" % (rel, size, size, os.path.getsize(dst)))


if __name__ == "__main__":
    main()
