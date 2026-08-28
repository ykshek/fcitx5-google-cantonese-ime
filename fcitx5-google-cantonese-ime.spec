Name:           fcitx5-google-cantonese-ime
Version:        0.2.0
Release:        1%{?dist}
Summary:        fcitx5 Cantonese input method backed by Google Input Tools

License:        AGPL-3.0-only
URL:            https://github.com/ykshek/fcitx5-google-cantonese-ime
Source0:        v%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  fcitx5-devel
BuildRequires:  libcurl-devel
BuildRequires:  json-devel
BuildRequires:  pkgconf-pkg-config
BuildRequires:  extra-cmake-modules
# git-core is only needed by the CMake FetchContent fallback when the system
# nlohmann_json package is unavailable; harmless otherwise.
BuildRequires:  git-core

Requires:       fcitx5
Requires:       libcurl

%description
A self-contained fcitx5 input method engine that queries Google Input Tools
over HTTPS and parses the JSON response itself. There is no separate daemon and
no Python runtime: a single fcitx5 shared library performs the romanized-input
to Cantonese-candidate lookup. Primarily intended for Traditional Chinese
(Cantonese, itc=yue-hant-t-i0-und) input.

%prep
%autosetup -n v%{version}

%build
%cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=%{_prefix}
%cmake_build

%install
%cmake_install

%check
# Smoke test: the three runtime files must be installed at their expected
# fcitx5 locations.
test -f %{buildroot}%{_libdir}/fcitx5/libfcitx5-google-ime.so
test -f %{buildroot}%{_datadir}/fcitx5/addon/google-ime.conf
test -f %{buildroot}%{_datadir}/fcitx5/inputmethod/google-ime.conf

%files
%license LICENSE
%doc README.md
%{_libdir}/fcitx5/libfcitx5-google-ime.so
%{_datadir}/fcitx5/addon/google-ime.conf
%{_datadir}/fcitx5/inputmethod/google-ime.conf

%changelog
* Fri Aug 28 2026 Alex Shek <hms.starryfish@gmail.com> - 0.2.0-1
- Initial RPM package for fcitx5-google-cantonese-ime.
