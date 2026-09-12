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

# --with gamedata: baut zusaetzlich das (nicht weitergebbare) Datenpaket aus
# einem gamedata/-Verzeichnis im Quelltarball.
%bcond_with gamedata

# --with prebuilt: uebernimmt eine fertig gebaute ausfuehrbare Datei aus
# prebuilt/harbour-nfsshift im Quelltarball, statt sie neu zu uebersetzen.
# Spart bei jedem Paketbau das vollstaendige Uebersetzen von dynarmic.
%bcond_with prebuilt

# Kein -debuginfo-Paket: dynarmic liefert rund 50 MB Debug-Info, und bei
# --with gamedata liefe der debuginfo-Lauf zusaetzlich ueber ~100 MB Daten.
%global debug_package %{nil}

Name:       harbour-nfsshift
Version:    0.1.0
Release:    1
Summary:    Need for Speed Shift (Marmalade-Build) auf Sailfish OS
License:    GPL-3.0-or-later
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
%if %{with gamedata}
Requires:       %{name}-data = %{version}-%{release}
%endif

%description
Laedt den 32-Bit-ARM-Code des Marmalade-Builds von Need for Speed Shift in
einen JIT (dynarmic) und setzt die Marmalade-Laufzeit auf SDL2 und OpenGL ES 2
neu um.  Enthaelt den LAN-Mehrspielermodus (UDP 45470 Suche, UDP 45471 Spiel).

Die Spieldaten von Electronic Arts gehoeren nicht zu diesem Paket.  Sie werden
einmalig aus einer vorhandenen Kopie des Originals importiert:

    harbour-nfsshift-import-data

Das Werkzeug sucht ~/Downloads/nfsshift*.deb bzw. ein bereits entpacktes
Verzeichnis und legt die Daten unter ~/.local/share/harbour-nfsshift/data ab.

%if %{with gamedata}
%package data
Summary:    Spieldaten von Need for Speed Shift
License:    Proprietary
BuildArch:  noarch
Requires:   %{name} = %{version}-%{release}

%description data
Die unveraenderten Datendateien des Originalspiels (NFSShift.s3e, res.dz,
Musik, Splashscreens).  Urheberrecht Electronic Arts - nicht weitergebbar,
nur fuer den privaten Gebrauch auf dem eigenen Geraet gebaut.
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
%files data
%defattr(-,root,root,-)
%dir %{_datadir}/%{name}/data
%{_datadir}/%{name}/data/*
%endif

%changelog
* Sat Sep 12 2026 smatkovi <sebastian.matkovich@gmail.com> - 0.1.0-1
- Erste Fassung: Loader plus LAN-Mehrspielermodus, Datenimport statt Spieldaten
