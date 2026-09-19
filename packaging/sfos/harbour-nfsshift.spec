# RPM fuer den NFS-Shift-Loader auf Sailfish OS.
#
# Gebaut wird mit packaging/sfos/build-rpm.sh (rpmbuild im sb2-Target des
# Containers sfossdk52).  Direkt im Container:
#
#   sb2 -t SailfishOS-5.2.0.15-aarch64 rpmbuild -bb harbour-nfsshift.spec
#   sb2 -t ... rpmbuild -bb --with prebuilt  harbour-nfsshift.spec   # s. u.
#   sb2 -t ... rpmbuild -bb --with gamedata  harbour-nfsshift.spec   # nur privat
#
# Das Paket enthaelt ausschliesslich unseren eigenen Code.  Die rund 100 MB
# Spieldaten von Electronic Arts sind NICHT enthalten; sie werden mit
# harbour-nfsshift-import-data aus der Kopie des Nutzers importiert.

# --with gamedata: legt die Spieldaten aus einem gamedata/-Verzeichnis im
# Quelltarball mit ins Paket, statt sie beim ersten Start zu importieren.  Das
# Ergebnis enthaelt dann fremdes Material (Electronic Arts) und darf nicht
# weitergegeben werden -- es ist fuer die eigenen Geraete gedacht.
%bcond_with gamedata

%if %{with gamedata}
# Mit den Spieldaten ist das ein anderes Paket als das freie mit derselben
# Version.  Das Release bekommt deshalb "full" angehaengt: 0.1.2-1full ist
# neuer als 0.1.2-1, das volle Paket ersetzt ein installiertes freies also
# beim Aktualisieren, und beide sind nie zu verwechseln.
%global relsuffix full
%global datalicense and Proprietary
%global datasummary samt Spieldaten
%endif

# --with prebuilt: uebernimmt eine fertig gebaute ausfuehrbare Datei aus
# prebuilt/harbour-nfsshift im Quelltarball, statt sie neu zu uebersetzen.
# Spart bei jedem Paketbau das vollstaendige Uebersetzen von dynarmic.
%bcond_with prebuilt

# Kein -debuginfo-Paket: dynarmic liefert rund 50 MB Debug-Info, und bei
# --with gamedata liefe der debuginfo-Lauf zusaetzlich ueber ~100 MB Daten.
%global debug_package %{nil}

Name:       harbour-nfsshift
Version:    0.1.4
Release:    1%{?relsuffix}
Summary:    Need for Speed Shift (Marmalade-Build) auf Sailfish OS %{?datasummary}
License:    GPL-3.0-or-later %{?datalicense}
URL:        https://example.invalid/nfsshift-sfos
Source0:    %{name}-%{version}.tar.gz

%if %{without prebuilt}
BuildRequires:  cmake >= 3.16
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(zlib)
BuildRequires:  pkgconfig(liblzma)
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Sensors)
%endif

# libSDL2, libEGL, libGLESv2, libz, liblzma, libQt5Sensors, libQt5Core liest
# rpmbuild selbst aus dem ELF (soname-Requires).  EGL/GLESv2 kommen je nach
# Geraet von libhybris oder mesa - deshalb hier bewusst KEIN Paketname.
# Nur was nicht im ELF steht, steht hier:
Requires:       qt5-qtsensors-plugin-sensorfw
# python3 fuer harbour-nfsshift-import-data
Requires:       /usr/bin/python3

%description
Laedt den 32-Bit-ARM-Code des Marmalade-Builds von Need for Speed Shift und
setzt die Marmalade-Laufzeit auf SDL2 und OpenGL ES 2 neu um.  Auf aarch64
laeuft der Spielcode in einem JIT (dynarmic), auf armv7hl direkt auf der CPU.
Enthaelt den LAN-Mehrspielermodus (UDP 45470 Suche, UDP 45471 Spiel).

%if %{with gamedata}
Dieses Paket bringt die Datendateien des Originalspiels mit (Urheberrecht
Electronic Arts).  Es ist fuer die eigenen Geraete gedacht und darf nicht
weitergegeben werden.
%else
Die Spieldaten von Electronic Arts gehoeren nicht zu diesem Paket.  Sie werden
einmalig aus einer vorhandenen Kopie des Originals importiert:

    harbour-nfsshift-import-data

Das Werkzeug sucht ~/Downloads/nfsshift*.deb bzw. ein bereits entpacktes
Verzeichnis und legt die Daten unter ~/.local/share/harbour-nfsshift/data ab.
%endif

%prep
%setup -q

%build
%if %{without prebuilt}
top=$(pwd)
mkdir -p build
cd build
# RelWithDebInfo wie tools/build.sh, damit das Paketbinary dem entspricht, was
# auf dem Geraet getestet wurde.  Die Debug-Info wird unten weggestrippt.
cmake .. \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DDYNARMIC_DIR="$top/dynarmic"
make %{?_smp_mflags} harbour-nfsshift
%endif

%install
rm -rf %{buildroot}

%if %{with prebuilt}
install -D -m 755 prebuilt/harbour-nfsshift %{buildroot}%{_bindir}/%{name}
%else
install -D -m 755 build/harbour-nfsshift %{buildroot}%{_bindir}/%{name}
%endif
# 51 MB unstripped -> rund 2,4 MB.  Nicht fatal, falls strip im Ziel fehlt.
strip %{buildroot}%{_bindir}/%{name} || :

install -D -m 755 packaging/sfos/nfsshift-import-data \
    %{buildroot}%{_bindir}/%{name}-import-data

install -D -m 644 packaging/sfos/%{name}.desktop \
    %{buildroot}%{_datadir}/applications/%{name}.desktop

for s in 86 108 128 172; do
    install -D -m 644 packaging/sfos/icons/icon-${s}.png \
        %{buildroot}%{_datadir}/icons/hicolor/${s}x${s}/apps/%{name}.png
done

# Der Loader sucht seine Daten unveraendert unter
# %{_datadir}/%{name}/data.  Dieses Verzeichnis wird beim Installieren zu
# einem Symlink in das Home des Geraetebesitzers (s. %%post) - so bleiben die
# EA-Daten beim Nutzer und das Paket bleibt frei von fremdem Material.
install -d %{buildroot}%{_datadir}/%{name}
%if %{with gamedata}
install -d %{buildroot}%{_datadir}/%{name}/data
cp -a gamedata/. %{buildroot}%{_datadir}/%{name}/data/
%endif

%if %{with gamedata}
%pre
# Ein frueher installiertes freies Paket hat unter %{_datadir}/%{name}/data
# einen Symlink ins Home angelegt (dessen %%post).  rpm kann darueber kein
# Verzeichnis auspacken ("Not a directory"), also weg damit -- die Daten im
# Home bleiben davon unberuehrt.
if [ -L %{_datadir}/%{name}/data ]; then
    rm -f %{_datadir}/%{name}/data
fi
exit 0
%endif

%if %{without gamedata}
%post
# Sailfish OS: der Geraetebesitzer hat uid 100000 ("defaultuser").
home=$(getent passwd 100000 2>/dev/null | cut -d: -f6)
[ -n "$home" ] || home=/home/defaultuser
if [ ! -e %{_datadir}/%{name}/data ]; then
    ln -sfn "$home/.local/share/%{name}/data" %{_datadir}/%{name}/data
fi
exit 0

%postun
if [ "$1" = "0" ]; then
    [ -L %{_datadir}/%{name}/data ] && rm -f %{_datadir}/%{name}/data
    rmdir %{_datadir}/%{name} 2>/dev/null
fi
exit 0
%endif

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%{_bindir}/%{name}-import-data
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
%dir %{_datadir}/%{name}
%if %{with gamedata}
%{_datadir}/%{name}/data
%endif


%changelog
* Sat Sep 12 2026 smatkovi <sebastian.matkovich@gmail.com> - 0.1.3-1
- Heap-Fenster wird zur Laufzeit gesucht, statt 0x10000000 zu erzwingen
  (noetig unter Android, wo ART mitten im Wunschbereich liegt; auf Sailfish
  unveraendert)

* Sat Sep 12 2026 smatkovi <sebastian.matkovich@gmail.com> - 0.1.0-1
- Erste Fassung: Loader plus LAN-Mehrspielermodus, Datenimport statt Spieldaten
