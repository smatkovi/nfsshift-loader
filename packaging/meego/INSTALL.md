# nfsshift-mp auf der Nokia N9 installieren

`nfsshift-mp_0.1.0_armel.deb` (61 218 B) ist ein **Zusatzpaket**. Es enthaelt weder
Spieldaten noch Spielcode, sondern nur die Bibliothek `libnfsmp.so` (74 056 B, ARM
EABI5), ein Startskript, ein `.desktop` und ein Symbol. Das Originalspiel bleibt
unveraendert; nach der Installation stehen zwei Eintraege im Menue:

| Eintrag | startet |
|---|---|
| **NFS Shift** | das Original, genau wie vorher |
| **NFS Shift LAN** | dasselbe Binary, aber mit vorgeladener LAN-Bibliothek |

Die Bibliothek redet UDP auf Port **45470** (Hostsuche) und **45471** (Sitzung),
bis zu vier Spieler, auch gemischt mit den Sailfish-OS- und Android-Fassungen.
Alle Geraete muessen im **selben WLAN** haengen; Client-Isolation im Router
(„AP isolation“, Gastnetz) verhindert die Hostsuche.

## Voraussetzungen

1. **Das Originalspiel `nfsshift` muss installiert sein.** Das Paket deklariert
   `Depends: nfsshift`; `dpkg -i` bricht sonst ab. Erwartete Pfade des Originals:
   `/opt/usr/bin/ea-mobile-nfsshift` und `/opt/ea-mobile-nfsshift/` (mit `res.dz`,
   `gamedata`, `gamesett`).
2. **Developer Mode** an (Einstellungen → Sicherheit → Developer Mode), damit es
   `devel-su` und ein Terminal gibt.
3. Fuer die Installation ueber den App-Manager statt ueber das Terminal muss
   „Installationen aus fremden Quellen“ erlaubt sein.

## Aufspielen und installieren

Datei auf das Telefon bringen — per USB-Massenspeicher, oder ueber WLAN:

```sh
# vom PC aus, Developer Mode liefert die IP und das SSH-Passwort
scp nfsshift-mp_0.1.0_armel.deb user@<N9-IP>:/home/user/MyDocs/
```

Dann auf dem Telefon im Terminal:

```sh
devel-su                                   # Passwort setzt der Developer Mode
dpkg -i /home/user/MyDocs/nfsshift-mp_0.1.0_armel.deb
```

Alternativ im Dateimanager auf die `.deb` tippen — der App-Manager installiert sie.

Danach einmal **aus dem Terminal** starten, das ist der eigentliche Test:

```sh
/opt/nfsshift-mp/startup-mp.sh
```

Erwartete Zeilen auf stderr:

```
[nfsmp] geladen (Protokoll v1, Ports 45470/45471)
[nfsmp] Image: code=0x... data=0x... split=001c1078 memsz=... tramp=0x... pad=0x...
[nfsmp] Wort-Patches: 15 von 15 angewandt
[nfsmp] Stub-Umleitungen: 29 von 29
[nfsmp] bl-Hooks: 3 von 3 (Veneer-Schwanz: 152 von 3992 Byte belegt)
[nfsmp] bereit, Spielername 'user'
```

Kommen diese Zeilen, ist alles in Ordnung; ab dann geht auch der Start ueber das
Menuesymbol „NFS Shift LAN“.

### Nuetzliche Umgebungsvariablen

```sh
NFSMP_NAME="Sebastian" /opt/nfsshift-mp/startup-mp.sh   # Name in Lobby und HUD
NFSMP_LOG=/home/user/MyDocs/nfsmp.log /opt/nfsshift-mp/startup-mp.sh
NFSMP_QUIET=1   # keine [nfsmp]-Meldungen
NFSMP_DISABLE=1 # Bibliothek geladen, Patcher aus (A/B-Vergleich)
NFSMP_FALLBACK=1 # Pollthread statt mprotect-Hook
```

`NFSMP_LOG` ist wichtig, wenn ueber das Menuesymbol gestartet wird: dort geht
stderr sonst verloren.

## Wieder loswerden

```sh
devel-su
dpkg -r nfsshift-mp
```

Das entfernt `/opt/nfsshift-mp/`, den zweiten Menueeintrag und das Symbol. Das
Originalspiel ist davon nicht betroffen — es wurde nie angefasst (keine Datei aus
`digsigsums` veraendert, kein Maintainer-Skript im Paket). Wer nur kurz ohne LAN
spielen will, startet einfach den alten Eintrag „NFS Shift“ oder benutzt
`NFSMP_DISABLE=1`.

## Wenn Aegis das LD_PRELOAD blockiert

**Symptom:** „NFS Shift LAN“ startet, das Spiel laeuft ganz normal — aber es gibt
keinen Mehrspielereintrag, und es kommt **keine einzige `[nfsmp]`-Zeile**.

**Logzeile** (auf stderr, also im Terminal oder in `NFSMP_LOG` sichtbar):

```
ERROR: ld.so: object '/opt/nfsshift-mp/libnfsmp.so' from LD_PRELOAD cannot be preloaded: ignored.
```

Zusaetzlich nachsehen: `dmesg | grep -i -E 'aegis|credential'` bzw.
`/var/log/syslog`.

**Der Reihe nach pruefen:**

1. **Ist es wirklich das Preload?** Ohne die obige `ld.so`-Zeile liegt es nicht an
   Aegis. Kommt `[nfsmp] geladen`, aber keine `Image:`-Zeile, feuert nur der
   `mprotect`-Hook nicht — dann `NFSMP_FALLBACK=1` probieren.
2. **Datei da und lesbar?** `ls -l /opt/nfsshift-mp/libnfsmp.so` muss
   `-rwxr-xr-x root root 74056` zeigen. Der Pfad im Startskript ist absolut, das
   ist Pflicht.
3. **Richtige Architektur?** Das Paket ist `armel`, EABI5, hardfp — passt zur N9.
   Ein auf dem PC gebautes x86-Objekt wuerde dieselbe Fehlermeldung ausloesen.
4. **`AT_SECURE`?** `ls -l /opt/usr/bin/ea-mobile-nfsshift`. Traegt das Binary ein
   `s`-Bit oder laeuft es mit erhoehten Credentials, ignoriert `ld.so` jedes
   `LD_PRELOAD` — das ist Standard-glibc-Verhalten, nicht Aegis. Gegenprobe: das
   Startskript einmal unter `devel-su` ausfuehren. Laeuft es als root, aber nicht
   als `user`, ist das die Ursache.

**Ausweichmoeglichkeiten**, falls es dabei bleibt (alle **UNSICHER**, mangels
Geraet nicht getestet):

* Globales Preload statt Umgebungsvariable: als root
  `echo /opt/nfsshift-mp/libnfsmp.so > /etc/ld.so.preload`, Telefon neu starten.
  Hilft, wenn die Variable unterwegs verloren geht, **nicht** bei `AT_SECURE`.
  Danach unbedingt wieder loeschen — die Datei gilt fuer *alle* Prozesse.
* Wenn `dmesg` tatsaechlich Aegis-Meldungen zeigt: die `.so` ist dem Validator
  unbekannt. Dann bleibt nur, sie ueber ein signiertes Paket bekannt zu machen,
  oder das Telefon in den „open mode“ zu bringen. Beides ist ein groesserer
  Eingriff als dieses Paket.
* Ohne LD_PRELOAD geht es auf der N9 gar nicht: die Sailfish-OS- und
  Android-Fassungen bringen ihren eigenen Loader mit und sind davon nicht
  betroffen — sie koennen mit einer funktionierenden N9 im selben Rennen fahren.

## Was noch nicht bewiesen ist

Der LAN-Code selbst ist im Zwei-Instanzen-Test vollstaendig verifiziert, und die
Bibliothek besteht alle 45 qemu-Pruefungen. **Ein Test auf echter Hardware steht
aber aus**: ob Aegis das Preload zulaesst und ob der `mprotect`-Hook im echten
Originalloader feuert, ist statisch begruendet, nicht gemessen.
