# Android: APK signieren und Spieldaten aufspielen

## 1. APK bauen

Die APK selbst entsteht in der Android-Aufgabe auf dem Build-Rechner
(`sebastian@192.168.1.21`) unter `/tmp/android`. `/tmp` ist dort ein tmpfs und
uebersteht keinen Neustart; ist die Umgebung weg, stellt sie

```sh
ssh sebastian@192.168.1.21 'sh /tmp/android/build-test-apk.sh'
```

wieder her — und falls auch das Skript fehlt, liegt eine Kopie in der Analyse
unter `.../scratchpad/wf1/android-env/build-test-apk.sh`
(siehe `android-env.md` §6, Punkt 4).
Das Skript laedt NDK r27c, Platform android-35 und SDL2 2.30.12 nach und baut
alles neu (rund 730 MB Download, wenige Minuten auf 16 Kernen).

## 2. Signieren und ablegen

```sh
packaging/android/sign-apk.sh                    # sucht selbst, s. u.
packaging/android/sign-apk.sh --apk /tmp/android/out/nfsshift-test.apk
packaging/android/sign-apk.sh --local            # direkt auf dem Build-Rechner
```

Ohne `--apk` wird auf dem Build-Rechner der Reihe nach gesucht:
`out-loader/nfsshift.apk`, `out/nfsshift.apk`, `out-loader/unsigned.apk`,
`out/nfsshift-unsigned.apk`, `out/nfsshift-test.apk` (jeweils unter
`$ANDROID_ROOT`, Vorgabe `/tmp/android`) — also die echte Loader-APK vor der
Test-APK. Eine bereits signierte APK wird neu signiert; `apksigner` entfernt
die alten `META-INF`-Signaturen dabei selbst.

Das Skript laeuft von hier aus per ssh auf dem Build-Rechner, macht dort
`zipalign -f -p 4`, signiert mit `apksigner` (v1+v2+v3), prueft mit
`apksigner verify --min-sdk-version 21 -v` und legt das Ergebnis unter

```
build/android/<paket>-<versionName>-<versionCode>-arm64-v8a.apk
```

ab, zusammen mit einer `.sha256`-Datei auf dem Build-Rechner
(`/tmp/android/release/`). Zweimal hintereinander aufgerufen kommt dieselbe
Datei heraus.

**Keystore.** Vorgabe ist `~/.config/nfsshift/nfsshift-release.keystore` auf
dem Build-Rechner — bewusst *nicht* unter `/tmp` (tmpfs) und nicht im Projekt.
Fehlt er, legt das Skript ihn an (RSA 2048, 10000 Tage, Alias `nfsshiftkey`,
Passwort aus `NFSSHIFT_KS_PASS`, Vorgabe `android`).

> Wer diesen Schluessel verliert, kann eine bereits installierte App nicht mehr
> aktualisieren; Android verweigert ein Update mit anderer Signatur. Die Datei
> also sichern, bevor der Rechner neu aufgesetzt wird.

Eigener Schluessel:

```sh
NFSSHIFT_KS_PASS=geheim packaging/android/sign-apk.sh \
    --keystore ~/keys/nfsshift.jks --alias nfsshift
```

Das Passwort geht nur ueber stdin an die Gegenstelle, nie ueber die
Kommandozeile (damit es nicht in `ps` auftaucht).

## 3. Spieldaten per `adb push`

In der APK sind **keine** Spieldaten. Fuer die Entwicklung ist das
app-eigene externe Verzeichnis der schnellste Weg: `getExternalFilesDir(null)`
alias `SDL_AndroidGetExternalStoragePath()` zeigt auf

```
/sdcard/Android/data/<paketname>/files
```

Dieses Verzeichnis braucht seit API 19 **keine** Berechtigung und wird beim
Deinstallieren mit entfernt.

```sh
adb=/tmp/android/sdk/platform-tools/adb
PKG=org.nfsshift.loader          # Paketname aus "aapt2 dump badging"
DATA=$HOME/.local/share/harbour-nfsshift/data    # Quelle: siehe packaging/sfos

$adb shell mkdir -p /sdcard/Android/data/$PKG/files/data
$adb push "$DATA/." /sdcard/Android/data/$PKG/files/data/
$adb shell ls -l /sdcard/Android/data/$PKG/files/data | head
```

Der Push dauert bei rund 99 MB je nach Kabel/WLAN 30–120 s. Danach:

```sh
$adb install -r build/android/<paket>-<version>-<code>-arm64-v8a.apk
$adb logcat -s nfsshift SDL
```

Gegenprobe, dass alles angekommen ist (die beiden Dateien, ohne die der Loader
nicht startet):

```sh
$adb shell 'cd /sdcard/Android/data/'$PKG'/files/data && ls -l NFSShift.s3e res.dz'
# erwartet: NFSShift.s3e 637560 B, res.dz 70441701 B
```

Hat man die Daten nicht schon entpackt vorliegen, erledigt das der Importer aus
dem Sailfish-Teil — er ist reines Python und laeuft auf jedem Rechner:

```sh
python3 packaging/sfos/nfsshift-import-data \
    ~/Downloads/nfsshift_1.0.20.10m7_armel.deb --target /tmp/nfsdata
adb push /tmp/nfsdata/. /sdcard/Android/data/$PKG/files/data/
```

**Andere Wege** (aus `android-env.md` §5.3, hier nur zur Einordnung):
Assets in der APK (`aapt2 link -A assets/ -0 .dz`) waeren rechtlich der
schlechteste Weg, weil dann EA-Material weitergegeben wuerde; ein Import ueber
`Intent.ACTION_OPEN_DOCUMENT` aus der Original-`.deb` waere die saubere
Endnutzer-Variante, braucht aber eine eigene, von `SDLActivity` abgeleitete
Activity und ist noch nicht gebaut.

## 4. Was noch fehlt

* Geprueft wurde mit der Test-APK (`com.nfsshift.loadertest`, 627 303 B) aus
  `android-env.md` und mit der echten Loader-APK der Android-Aufgabe
  (`org.nfsshift.loader` 1.0 (1), 1 680 056 B aus
  `/tmp/android/out-loader/nfsshift.apk`) — beide signiert und mit
  `apksigner verify` gegen v1, v2 und v3 bestaetigt.
* Fuer LAN-Betrieb braucht die App `MulticastLock` und einen
  Foreground-Service (`android-env.md` §5.2, `packaging-lan-design.md` §1.3) —
  Rechte `INTERNET`, `ACCESS_WIFI_STATE`, `CHANGE_WIFI_MULTICAST_STATE`,
  `WAKE_LOCK` stehen bereits im Manifest der Test-App.
* An den Build-Rechner ist kein Android-Geraet angeschlossen; die
  `adb`-Schritte oben sind nicht auf einem Geraet erprobt. **UNSICHER.**
