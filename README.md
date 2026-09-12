# NFS Shift Loader — Sailfish OS und Nokia N9

Führt den 32-Bit-ARM-Code des Marmalade-Builds von *Need for Speed Shift*
(MeeGo/Harmattan, `NFSShift.s3e`, 1.0.20) in einem JIT aus und setzt die
Marmalade-Laufzeit auf SDL2 und OpenGL ES 2 neu um. Dazu der nachgerüstete
**LAN-Mehrspielermodus** für bis zu 4 Spieler, plattformübergreifend zwischen
Sailfish OS, Android und der originalen N9.

> **Die Spieldaten von Electronic Arts gehören nicht hierher.** In diesem Repo
> steckt ausschließlich eigener Code. Zum Spielen braucht man die eigene Kopie
> des Originalpakets; der Import läuft beim ersten Start.
>
> Die Icons unter `packaging/*/icons/` sind aus dem Icon des Originalpakets
> abgeleitet und damit ebenfalls EA-Material — deshalb ist dieses Repo privat.
> Für eine Veröffentlichung müsste `packaging/make-icons.py` durch ein eigenes
> Motiv ersetzt werden.

## Zwei Wege auf zwei Geräte

| Ziel | Was läuft | Paket |
|---|---|---|
| **Sailfish OS** (aarch64) | unser eigener Loader | `harbour-nfsshift-*.aarch64.rpm` |
| **Nokia N9** (armel) | der *originale* Loader plus `LD_PRELOAD` | `nfsshift-mp_*_armel.deb` |

Auf der N9 läuft das Spiel ja bereits — dort fehlt nur der Mehrspielermodus.
Deshalb wird dort nichts ersetzt, sondern eine Bibliothek vorgeladen, die den
Spielcode zur Laufzeit patcht (`meego/preload.c`).

Die Android-Fassung liegt in einem eigenen Repo (`nfsshift-android`).

## Installieren

### Sailfish OS

```sh
devel-su pkcon install-local harbour-nfsshift-0.1.0-1.aarch64.rpm
harbour-nfsshift-import-data          # holt die Spieldaten aus ~/Downloads/nfsshift*.deb
```

Der Import entpackt `ar` + `tar.gz` selbst (reines python3) nach
`~/.local/share/harbour-nfsshift/data`. `--check` prüft, ob alles da ist.

### Nokia N9

Developer Mode einschalten, dann:

```sh
devel-su dpkg -i nfsshift-mp_0.1.0_armel.deb
```

Danach startet **NFS Shift LAN** das Originalspiel mit vorgeladener Bibliothek.
Das Originalspiel bleibt unangetastet und weiterhin startbar.
Details und Fehlersuche: `packaging/meego/INSTALL.md`.

## LAN-Mehrspieler

Bis zu 4 Spieler in einem WLAN. Zwei UDP-Ports: **45470** für die Hostsuche
(Rundruf), **45471** für die Sitzung.

Im Spiel: **Race → Multiplayer**, dort mit den Pfeilen zwischen **HOST GAME**
und **JOIN GAME** umschalten. Der Host wählt Strecke, Event und Auto und landet
in der Lobby; wer beitritt, sieht ihn in der Hostliste, klopft an, und der Host
bestätigt. Sind alle bereit, läuft ein gemeinsamer Countdown und das Rennen
startet auf allen Geräten zur selben Zeit.

Spielername: `NFS_MP_NAME`, sonst `LanPlayerName` in der `app.icf`, sonst der
Hostname.

`docs/DESIGN_LAN.md` beschreibt Protokoll, Uhrensynchronisation, die
Feldbelegung im `MultiplayerManager` und alle Eingriffe ins Spielimage.

## Wie das möglich war

Der ausgelieferte MeeGo-Build enthält die **komplette** Mehrspielerlogik der
iPhone-Fassung samt vier Spielerplätzen. Herausgenommen waren nur die
Transportschicht — 28 leere Methoden im `MultiplayerManager` — und der
Menüeintrag. Drei Sperren mussten fallen:

1. Beim Betreten des Multiplayer-Bildschirms schrieb das Spiel bedingungslos
   Fehler 17 („Multiplayer unavailable") und verließ den Modus sofort wieder.
2. Der Host/Join-Wähler auf diesem Bildschirm wurde nie aufgebaut — ohne ihn
   war „Join" überhaupt nicht erreichbar. Wir bauen ihn aus den vorhandenen
   Widgets selbst.
3. Zwei Setter waren wegkompiliert: `TrackObject::SetPeerIndex` (ohne ihn
   bekommen ab drei Spielern alle Netzautos den Index 0 — falsche Modelle,
   Namen und Kollisionen) und die Auto-Spalte der Lobby.

Alle Eingriffe stehen in `src/mp/patches.c` und prüfen vor jedem Schreibzugriff
das Originalwort; schlägt einer fehl, wird das Image byteweise zurückgerollt und
der Mehrspielermodus bleibt aus.

## Bauen

```sh
tools/build.sh                    # Sailfish aarch64, im SDK-Container
packaging/sfos/build-rpm.sh       # RPM
meego/build.sh                    # libnfsmp.so mit der MADDE-Toolchain
meego/test/run-test.sh            # qemu-Test der N9-Seite (45 Prüfungen)
packaging/meego/build-deb.sh      # .deb
tools/build-x86.sh                # x86_64 für automatisierte Tests
tools/x86-lan.sh                  # zwei Instanzen gegeneinander, headless
```

Alle Skripte arbeiten über `ssh` auf einem Baurechner; `tools/buildhost.sh`
sucht ihn (LAN-Adresse, sonst SSH-Alias), `BUILD_HOST` übersteuert.

## Stand

Der Mehrspielermodus ist im Zwei-Instanzen-Test vollständig durchgespielt:
Hostsuche, Beitritt, synchrone Lobbys, gemeinsamer Countdown mit identischem
Startzeitpunkt, Sample-Austausch mit sichtbaren Gegnerautos, drei Runden ohne
eine einzige Uhrenkorrektur, gemeinsamer Ergebnisbildschirm.

Auf echter Hardware ungetestet: Handy gegen Handy, und die N9 — ob Aegis das
`LD_PRELOAD` zulässt, weiß niemand. Die N9-Seite ist unter qemu nachgewiesen
(45 Prüfungen), nicht auf dem Gerät.
