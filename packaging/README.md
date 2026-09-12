# Paketierung

Drei Plattformen, drei Pakete, ein gemeinsamer Grundsatz: **in den Paketen
steckt nur unser eigener Code.** Die rund 100 MB Spieldaten von Electronic Arts
bleiben beim Nutzer und werden beim ersten Start aus dessen eigener Kopie
importiert (siehe „Rechtliches" am Ende).

| Verzeichnis | Ziel | Ergebnis | Baut mit |
|---|---|---|---|
| `sfos/` | Sailfish OS, aarch64 | `harbour-nfsshift-<ver>-<rel>.aarch64.rpm` | `sfos/build-rpm.sh` |
| `meego/` | Nokia N9 / Harmattan, armel | `nfsshift-mp_<ver>_armel.deb` | `meego/build-deb.sh` |
| `android/` | Android, arm64-v8a | `<paket>-<ver>-<code>-arm64-v8a.apk` | `android/sign-apk.sh` |

Alle drei Skripte sind idempotent: sie raeumen vorher auf, schreiben ihr
Ergebnis immer an denselben Ort und liefern bei gleichen Eingaben dieselbe
Datei. Zwischenstaende liegen auf dem Build-Rechner ausschliesslich unter
`/tmp` bzw. in einem eigenen Verzeichnis im Container, das nach jedem Lauf
geloescht wird — die Root-Partition dort ist zu 97 % voll.

Ergebnisse landen lokal unter `build/rpm/`, `build/meego/` und
`build/android/`.

`make-icons.py` erzeugt einmalig alle Icons (86/108/128/172 px fuer Sailfish,
80 und 64 px fuer die N9) aus dem 80×80-Icon des Originalpakets; es laeuft nicht
im Paketbau mit, damit der Build kein ImageMagick braucht.

---

## 1. Sailfish OS — `harbour-nfsshift` (RPM)

```sh
packaging/sfos/build-rpm.sh              # vollstaendiger Bau aus dem Quelltext
packaging/sfos/build-rpm.sh --prebuilt   # fertiges Binary uebernehmen (schnell)
packaging/sfos/build-rpm.sh --check      # nur .spec und .desktop pruefen
```

Der Bau laeuft auf `sebastian@192.168.1.21` im Docker-Container `sfossdk52`
(Target `SailfishOS-5.2.0.15-aarch64`): Quelltext hineinspiegeln, Quelltarball
schnueren, `sb2 -t <target> rpmbuild --target=aarch64-meego-linux-gnu -bb`, dann
`rpm -qpi`/`-qpl`/`--requires`/`--scripts` zeigen und das RPM zurueckholen.

**Inhalt** (gebaut und geprueft, 809 478 B; `rpm -i --test` gegen die
Paketdatenbank dieses Geraets laeuft ohne Beanstandung durch):

```
/usr/bin/harbour-nfsshift                 2 695 488 B (gestript)
/usr/bin/harbour-nfsshift-import-data        17 062 B
/usr/share/applications/harbour-nfsshift.desktop
/usr/share/icons/hicolor/{86x86,108x108,128x128,172x172}/apps/harbour-nfsshift.png
/usr/share/harbour-nfsshift               (Verzeichnis, s. u.)
```

Die Icons entstehen offline aus dem 80×80-Icon des MeeGo-Originalpakets
(`packaging/make-icons.py`), damit der Paketbau kein ImageMagick braucht.

**Abhaengigkeiten.** `libSDL2`, `libEGL`, `libGLESv2`, `libz`, `liblzma`,
`libQt5Core`, `libQt5Sensors` liest rpmbuild selbst aus dem ELF als
soname-Requires — bewusst keine Paketnamen, weil EGL/GLESv2 je nach Geraet von
libhybris oder mesa kommen. Explizit steht nur da, was nicht im ELF steht:
`qt5-qtsensors-plugin-sensorfw` (sonst findet `QAccelerometer::connectToBackend()`
kein Backend, `src/sensors.cpp:187`) und `/usr/bin/python3` fuer das
Import-Werkzeug.

### 1.1 Sailjail

`harbour-nfsshift.desktop` enthaelt

```
[X-Sailjail]
OrganizationName=org.smatkovi
ApplicationName=harbour-nfsshift
Permissions=Internet;Audio
```

* **Internet** — `Base.permission` setzt `net none` und `protocol unix`;
  `Internet.permission` hebt das mit `ignore net none` /
  `protocol inet,inet6,netlink` auf. Ohne diese Permission bekommt die App
  ueberhaupt keinen Socket, UDP 45470 (Suche) und 45471 (Spiel) waeren tot.
  Eigene Messung auf dem Geraet: die Sailfish-Firewall laesst alle
  unprivilegierten Ports eingehend durch, auch Broadcast
  (`/etc/connman/firewall.d/11-allow-udp-non-privileged-ports-firewall.conf`),
  es ist also nichts weiter einzustellen.
* **Audio** — `Base.permission` setzt `nosound`; `Audio.permission` beginnt mit
  `ignore nosound` und gibt PulseAudio frei. Der Loader startet SDL mit
  `SDL_INIT_AUDIO` (`src/display.cpp:117`).

Genau diese Kombination benutzt `supertuxkart` auf demselben Geraet
(`/usr/share/applications/supertuxkart.desktop`), ebenso
`X-Nemo-Application-Type=generic`.

Nicht gesetzt ist **Sensors**. Das QtSensors-sensorfw-Backend braucht
`dbus-system.talk com.nokia.SensorService` aus `Sensors.permission`; ohne die
Permission meldet `src/sensors.cpp:188` „no accelerometer backend" und die
Neigungslenkung faellt aus — das Spiel laeuft weiter. Wer sie will, haengt
`;Sensors` an die `Permissions`-Zeile. Notausgang, falls der Sandkasten
GLES/sensorfw/Import stoert: `Sandboxing=Disabled` im selben Abschnitt (auf
diesem Geraet bei etlichen Apps in Gebrauch, u. a. `fingerterm`).

### 1.2 Datenimport beim ersten Start

Das Paket enthaelt keine Spieldaten. Einmalig:

```sh
harbour-nfsshift-import-data                 # sucht selbst
harbour-nfsshift-import-data ~/Downloads/nfsshift_1.0.20.10m7_armel.deb
harbour-nfsshift-import-data /pfad/zu/ea-mobile-nfsshift     # schon entpackt
harbour-nfsshift-import-data --check         # Zustand + sha256
harbour-nfsshift-import-data --list          # wo gesucht wird
harbour-nfsshift-import-data --force         # neu importieren
```

Gesucht wird nach `nfsshift*.deb` unter `~/Downloads`, `~/Downloads/*/`,
`~/android_storage/Download`, `/run/media/*/*` sowie nach bereits entpackten
Verzeichnissen (`/opt/ea-mobile-nfsshift`, `~/Downloads/ea-mobile-nfsshift`,
`…/opt/ea-mobile-nfsshift`, SD-Karte). Aus der `.deb` wird die `ar`-Huelle
selbst geparst und `data.tar.gz` gestreamt entpackt; nur die Dateien unter
`./opt/ea-mobile-nfsshift/` landen im Ziel

```
~/.local/share/harbour-nfsshift/data
```

Das Werkzeug ist idempotent (ist alles da und stimmen die sha256-Summen von
`NFSShift.s3e` und `res.dz`, schreibt es nichts), prueft vorher den freien
Platz und braucht nur die Python-Standardbibliothek — kein `ar`, kein `tar`,
kein `dpkg`.

**Wie der Loader die Daten findet.** Der Loader wird nicht geaendert; seine
Vorgabe ist `/usr/share/harbour-nfsshift/data` (`src/main.cpp:30`). Deshalb
legt das RPM in `%post` dort einen Symlink auf
`<home>/.local/share/harbour-nfsshift/data` an; das Home wird zur Installationszeit
aus `getent passwd 100000` bestimmt (Sailfish: `defaultuser`). Beim Entfernen
des Pakets raeumt `%postun` den Symlink wieder weg. Unter Sailjail sind beide
Seiten des Symlinks sichtbar: `/usr/share/harbour-nfsshift` wird aus dem
Dateinamen der `.desktop` gewhitelistet, `${HOME}/.local/share/harbour-nfsshift`
ebenfalls. Ist der Symlink verlorengegangen oder wurde ein anderes Ziel
gewaehlt:

```sh
devel-su harbour-nfsshift-import-data --link
```

Ohne Paket, direkt aus dem Quellbaum, geht es auch ohne Symlink:
`harbour-nfsshift --data ~/.local/share/harbour-nfsshift/data` (so macht es
`tools/run.sh`).

### 1.3 Privates Datenpaket

Nur fuer den eigenen Gebrauch, nicht weitergebbar:

```sh
packaging/sfos/build-rpm.sh --gamedata ~/.local/share/harbour-nfsshift/data
```

Das baut zusaetzlich `harbour-nfsshift-data` (noarch, rund 100 MB, Vorbild
`supertuxkart-data` auf demselben Geraet) und installiert die Daten nach
`/usr/share/harbour-nfsshift/data`. Dann entfaellt der Symlink. Achtung: das
kopiert 100 MB in den Container auf der fast vollen Root-Partition.

---

## 2. Nokia N9 / Harmattan — `nfsshift-mp` (DEB)

```sh
packaging/meego/build-deb.sh                 # nimmt build/meego/libnfsmp.so
packaging/meego/build-deb.sh --so PFAD
packaging/meego/build-deb.sh --stub          # Platzhalter erzwingen
```

Ein **Zusatzpaket**: es laesst das Originalpaket `nfsshift` voellig unberuehrt
(keine Konflikte mit dessen `digsigsums`, kein Aegis-Risiko) und stellt nur
einen zweiten Starter daneben.

```
/opt/nfsshift-mp/libnfsmp.so                   LD_PRELOAD-Bibliothek
/opt/nfsshift-mp/startup-mp.sh                 cd + LD_PRELOAD + exec Original
/usr/share/applications/nfsshift-mp.desktop    "NFS Shift LAN"
/usr/share/themes/base/meegotouch/icons/nfsshift-mp-80.png
```

`DEBIAN/control` wird aus `control.in` erzeugt: `Package: nfsshift-mp`,
`Section: user/games` (sonst taucht das Paket im App-Manager nicht auf),
`Architecture: armel`, `Depends: nfsshift`, `Maemo-Display-Name: NFS Shift LAN`
und `Maemo-Icon-26` als base64-PNG 64×64 mit genau einem fuehrenden Leerzeichen
je Folgezeile. `Installed-Size` rechnet das Bauskript aus.

Das `.deb` schreibt `mkdeb.py` selbst: Reihenfolge `debian-binary`,
`control.tar.gz`, `data.tar.gz`, gzip (das alte dpkg 1.15.x kann kein xz/zstd),
`ar`-Membernamen **ohne** angehaengten `/` — GNU `ar` haengt einen an, `dpkg-deb`
nicht, und mit dem Harmattan-dpkg ist das nie getestet worden. Ausserdem
reproduzierbar: feste mtime, `root:root`, sortierte Eintraege, gzip ohne
Zeitstempel — zwei Laeufe liefern dieselbe Datei Byte fuer Byte.

Pruefen (es gibt hier kein `dpkg-deb`):

```sh
ar t build/meego/nfsshift-mp_*.deb
python3 packaging/meego/mkdeb.py --info build/meego/nfsshift-mp_*.deb
```

`--info` ersetzt `dpkg-deb -I` und `-c`: es zeigt die ar-Mitglieder mit Offsets,
prueft die Reihenfolge, gibt `control` aus und listet `data.tar.gz` mit Rechten.

**Platzhalter.** Solange `build/meego/libnfsmp.so` noch nicht existiert, baut
das Skript einen Platzhalter — bevorzugt eine echte, leere ARM-`.so` mit der
MADDE-Toolchain auf dem Build-Rechner
(`~/QtSDK/Madde/targets/harmattan_10.2011.34-1_rt1.2/bin/gcc`, gcc 4.4.1), sonst
eine inerte Datei. Die Version bekommt dann den Zusatz `~stub`
(`nfsshift-mp_0.1.0~stub_armel.deb`), damit so ein Paket nie mit einem echten
verwechselt wird. Die erzeugte Stub-`.so` hat dieselben ELF-Merkmale wie das
Originalbinary des Spiels: `Flags 0x5000002`, `Tag_CPU_name "CORTEX-A8"`,
`Tag_CPU_arch v7`, `Tag_FP_arch VFPv3`, `Tag_ABI_VFP_args: VFP registers`.

Installieren auf der N9 (Developer Mode, „Installationen aus fremden Quellen"
erlaubt):

```sh
devel-su dpkg -i nfsshift-mp_0.1.0_armel.deb
```

Spieldaten braucht dieses Paket keine — sie liegen bereits in
`/opt/ea-mobile-nfsshift/`, und `startup-mp.sh` setzt das Arbeitsverzeichnis
genau dorthin, so wie das Original-`startup.sh`.

---

## 3. Android — signierte APK

Siehe `packaging/android/README.md`. Kurz:

```sh
packaging/android/sign-apk.sh            # zipalign + apksigner v1/v2/v3 + verify
```

Geprueft mit der Loader-APK der Android-Aufgabe
(`org.nfsshift.loader` 1.0 (1), 1 680 056 B) und mit der Test-APK aus
`android-env.md`; beide verifizieren gegen v1, v2 und v3.

Der Keystore liegt auf dem Build-Rechner unter
`~/.config/nfsshift/nfsshift-release.keystore` — nicht unter `/tmp` (tmpfs, weg
nach dem Neustart) und nicht im Projekt. Er muss gesichert werden: mit einer
anderen Signatur laesst sich eine installierte App nicht mehr aktualisieren.
Die Spieldaten kommen per `adb push` nach
`/sdcard/Android/data/<paket>/files/data/` — ein Verzeichnis, das seit API 19
keine Berechtigung braucht und beim Deinstallieren mitgeloescht wird.

---

## 4. Netz

Alle drei Pakete sprechen dasselbe Protokoll (`docs/DESIGN_LAN.md`):

| Zweck | Port |
|---|---|
| Suche (Broadcast-Sonde, Unicast-Antwort) | UDP **45470** |
| Sitzung (Lobby, Rennen, Samples) | UDP **45471** |

Beide liegen ueber 1024 und sind auf Sailfish OS ohne jede Aenderung eingehend
offen (nachgemessen, s. o.). Auf Android braucht der Empfang von Broadcasts
einen `MulticastLock` und — damit das Geraet im Hintergrund erreichbar bleibt —
einen Foreground-Service; das gehoert in die App, nicht ins Paket. Auf der N9
fordert das Originalpaket keine Aegis-Tokens an, und fuer einfache
AF_INET-Sockets gibt es dort auch keine.

---

## 5. Rechtliches

* **Unser Code** (Loader, Protokollkern, Glue, Patches, Paketierung) steht unter
  GPL-3.0-or-later und darf weitergegeben werden.
* **Die Spieldaten** (`res.dz` 70,4 MB, `NFSShift.s3e` 637 KB, 18 lizenzierte
  Musiktitel, Splashscreens; zusammen 99,0 MB) sind unveraendertes Material von
  Electronic Arts. Sie sind in **keinem** dieser Pakete enthalten und duerfen
  nicht weitergegeben werden — weder ueber Chum/OpenRepos noch als
  GitHub-Release noch in einer APK. Der Nutzer bringt seine eigene Kopie mit;
  `harbour-nfsshift-import-data` holt sie dort ab.
* **Das Icon** ist aus dem 80×80-Icon des Originalpakets hochskaliert und damit
  ebenfalls EA-Material. Fuer den privaten Gebrauch auf dem eigenen Geraet ist
  das unproblematisch; **vor einer Veroeffentlichung muss es durch ein eigenes
  Motiv ersetzt werden.** Betroffen sind `packaging/sfos/icons/*` und
  `packaging/meego/icons/*`; `packaging/make-icons.py` erzeugt sie neu.
* Das mit `--gamedata` baubare `harbour-nfsshift-data`-RPM ist ausdruecklich
  nur fuer das eigene Geraet gedacht; es traegt `License: Proprietary` und darf
  nicht verteilt werden.
* Aus demselben Grund ist der Jolla Store keine Option; Ziel sind Chum bzw.
  OpenRepos fuer das Loader-Paket allein.

---

## 6. Offene Punkte

1. **Sailjail ist nicht im Sandkasten erprobt.** Weder Wayland/EGL noch der
   Symlink `/usr/share/harbour-nfsshift/data → $HOME/...` sind unter firejail
   getestet worden — beide Seiten sind laut `sailjail --dry-run` eines
   installierten Beispiels gewhitelistet, aber das ist ein Indizienbeweis.
   **UNSICHER.** Erster Gegentest nach dem Installieren:
   `harbour-nfsshift-import-data --check` zeigt den Zustand des Symlinks.
   Die Scriptlet-Logik selbst (`%post`/`%postun`) ist ausserhalb von rpm
   gegengespielt worden: Symlink wird angelegt, zeigt auf die vorhandenen
   Daten, ein zweiter Lauf (Upgrade) laesst ihn stehen, `%postun` raeumt
   Symlink und Verzeichnis weg. Das Paket selbst wurde nicht installiert.
2. **`X-Nemo-Application-Type`:** `generic` (wie supertuxkart) gegen
   `no-invoker` — praktisch ausprobieren, welche Variante mit Lipstick
   schoener startet.
3. **Sensors-Permission** ist bewusst nicht gesetzt (§1.1); ob die
   Neigungslenkung ohne sie wirklich nur ausfaellt und nicht stoert, ist nicht
   gemessen.
4. **Zwei Instanzen auf einem Geraet** (Loopback-Test) koennten am Sandkasten
   scheitern — `harbour-fintube.desktop` berichtet genau davon. Fuer LAN-Tests
   deshalb zwei Geraete oder ein x86-Build (`tools/build-x86.sh`).
5. **Die N9 ist nicht getestet.** Weder `dpkg -i` des Zusatzpakets noch
   LD_PRELOAD gegen Aegis sind auf echter Hardware ausprobiert
   (`meego-runtime-patch.md` §9). Das gebaute `.deb` enthaelt bisher nur einen
   Platzhalter.
6. **Kein Android-Geraet** am Build-Rechner; die APK ist statisch korrekt und
   signiert, aber nie gestartet worden.
7. Das Import-Werkzeug prueft die sha256-Summen der Fassung 1.0.20.10m7 und
   warnt nur bei Abweichung — andere Spielversionen sind nicht untersucht.
