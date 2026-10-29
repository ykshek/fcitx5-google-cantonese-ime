# Justfile with convenient tasks to build and install the Fcitx5 Flatpak add-on.

# Quick test: query Google Input Tools directly and print the raw JSON response.
# Useful for checking network access and the itc code without building.
query-google q:
    @curl -sS "https://inputtools.google.com/request?text={{q}}&itc=yue-hant-t-i0-und&num=8"

install-flatpak-builder:
    flatpak install --user --noninteractive --assumeyes flathub org.flatpak.Builder

build-flatpak: install-flatpak-builder
    #!/usr/bin/env bash
    set -euo pipefail
    rm -rf build
    rm -rf builddir
    rm -rf .flatpak-builder
    source_archive=flatpak/source.tar.gz
    trap 'rm -f "$source_archive"' EXIT
    mkdir -p build
    tar -czf "$source_archive" --transform='s,^,source/,' \
        CMakeLists.txt LICENSE data src tests \
        flatpak/org.fcitx.Fcitx5.Addon.GoogleIME.metainfo.xml
    flatpak run --command=flathub-build org.flatpak.Builder \
        --user --force-clean --disable-cache --disable-rofiles-fuse \
        --install-deps-from=flathub \
        --repo=build/flatpak-repo \
        flatpak/org.fcitx.Fcitx5.Addon.GoogleIME.yml
    flatpak_files=builddir/files
    if [[ ! -f "$flatpak_files/lib/fcitx5/libfcitx5-google-ime.so" ]]; then
        echo "Error: Flatpak add-on library was not installed under lib/fcitx5"
        exit 1
    fi
    test ! -e "$flatpak_files/lib64/fcitx5/libfcitx5-google-ime.so"
    test -f "$flatpak_files/share/fcitx5/addon/google-ime.conf"
    test -f "$flatpak_files/share/fcitx5/inputmethod/google-ime.conf"
    grep -Fxq "Icon=fcitx-googlepinyin-panel" \
        "$flatpak_files/share/fcitx5/inputmethod/google-ime.conf"
    test -f "$flatpak_files/share/metainfo/org.fcitx.Fcitx5.Addon.GoogleIME.metainfo.xml"

# Make the Papirus "fcitx-googlepinyin-panel" icon reachable by the desktop's
# status notifier.
#
# fcitx5 running inside the Flatpak rewrites every icon name that starts with
# "fcitx" to "org.fcitx.Fcitx5.fcitx..." before exporting it over
# StatusNotifierItem (IconTheme::iconName in fcitx5). The official fcitx5
# Flatpak handles this by building every official add-on's icons into the main
# app (the fcitx5-all-icons module + install_icons in flatpak-fcitx5), which
# Flatpak exports to the host icon theme under the prefixed name. A third-party
# add-on cannot add files to the main app, and icons inside the add-on extension
# are only visible inside the sandbox, so the host desktop never sees them.
#
# Papirus only provides the unprefixed name, so link it into the user's hicolor
# theme under the exact name fcitx5 asks the desktop for. No icon artwork is
# copied or vendored; the link points at the system theme file.

# Link the Papirus tray icon under the name fcitx5 asks the desktop for.
install-tray-icon:
    #!/usr/bin/env bash
    set -euo pipefail

    data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
    icon_name="fcitx-googlepinyin-panel"
    # What fcitx5 asks for in Flatpak: "org.fcitx.Fcitx5." + icon name.
    alias_name="org.fcitx.Fcitx5.$icon_name"

    source_icon=""
    for theme_dir in \
        "$data_home/icons/Papirus-Dark" \
        "$data_home/icons/Papirus" \
        "$data_home/icons/Papirus-Light" \
        /usr/share/icons/Papirus-Dark \
        /usr/share/icons/Papirus \
        /usr/share/icons/Papirus-Light; do
        for size in 24x24 22x22 16x16; do
            for extension in svg png; do
                candidate="$theme_dir/$size/panel/$icon_name.$extension"
                if [[ -f "$candidate" ]]; then
                    source_icon="$candidate"
                    break 3
                fi
            done
        done
    done

    if [[ -z "$source_icon" ]]; then
        echo "Error: $icon_name was not found in any Papirus icon theme." >&2
        echo "Install papirus-icon-theme (or Papirus-Dark) first." >&2
        exit 1
    fi

    extension="${source_icon##*.}"
    if [[ "$extension" == svg ]]; then
        icon_dir="$data_home/icons/hicolor/scalable/apps"
    else
        icon_dir="$data_home/icons/hicolor/$size/apps"
    fi

    mkdir -p "$icon_dir"
    link="$icon_dir/$alias_name.$extension"
    ln -sfn "$source_icon" "$link"
    echo "Linked $link -> $source_icon"

    # Refresh the hicolor cache when the directory is a proper icon theme.
    if [[ -f "$data_home/icons/hicolor/index.theme" ]] &&
       command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f -t "$data_home/icons/hicolor" >/dev/null 2>&1 || true
    fi

install-flatpak: install-flatpak-builder
    #!/usr/bin/env bash
    set -euo pipefail
    rm -rf build
    rm -rf builddir
    rm -rf .flatpak-builder
    source_archive=flatpak/source.tar.gz
    trap 'rm -f "$source_archive"' EXIT
    mkdir -p build
    tar -czf "$source_archive" --transform='s,^,source/,' \
        CMakeLists.txt LICENSE data src tests \
        flatpak/org.fcitx.Fcitx5.Addon.GoogleIME.metainfo.xml
    flatpak run --command=flathub-build org.flatpak.Builder \
        --user --install --force-clean --disable-rofiles-fuse \
        --install-deps-from=flathub \
        --repo=build/flatpak-repo \
        flatpak/org.fcitx.Fcitx5.Addon.GoogleIME.yml
    # The add-on itself cannot make its icon visible to the host desktop.
    if ! just install-tray-icon; then
        echo "Warning: the Papirus tray icon was not linked; fcitx5 will fall" >&2
        echo "back to a generic icon. Run 'just install-tray-icon' once Papirus" >&2
        echo "is installed." >&2
    fi
    echo
    echo "Restart fcitx5 so it loads the new add-on: flatpak kill org.fcitx.Fcitx5"

bump-version version:
    #!/usr/bin/env bash
    set -euo pipefail

    VERSION="{{version}}"

    # Validate version format (should be like 1.2.3)
    if ! echo "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
        echo "Error: Version must be in format X.Y.Z (e.g. 1.2.3)"
        exit 1
    fi

    echo "Bumping version to $VERSION..."

    # Check if working directory is clean
    if ! git diff --quiet || ! git diff --cached --quiet; then
        echo "Error: Working directory is not clean. Please commit or stash changes first."
        git status --porcelain
        exit 1
    fi

    METADATA=flatpak/org.fcitx.Fcitx5.Addon.GoogleIME.metainfo.xml
    RELEASE_DATE=$(date -u +%F)
    if ! grep -q '^<component ' "$METADATA" || ! grep -q '</component>' "$METADATA"; then
        echo "Error: Invalid AppStream metadata in $METADATA"
        exit 1
    fi
    if grep -q "<release version=\"$VERSION\"" "$METADATA"; then
        echo "Error: Version $VERSION already exists in $METADATA"
        exit 1
    fi

    # Update the version in CMakeLists.txt (project(... VERSION ...))
    echo "Updating version in CMakeLists.txt..."
    sed -i -E "s/(project\(fcitx5-google-cantonese-ime VERSION )[^ ]+/\1$VERSION/" CMakeLists.txt

    # Record the Flatpak release in its AppStream metadata.
    echo "Updating version in $METADATA..."
    if grep -q '<releases>' "$METADATA"; then
        sed -i "/<releases>/a\\    <release version=\"$VERSION\" date=\"$RELEASE_DATE\"/>" "$METADATA"
    else
        sed -i "/<\/component>/i\\  <releases>\\n    <release version=\"$VERSION\" date=\"$RELEASE_DATE\"/>\\n  </releases>" "$METADATA"
    fi

    # Verify the changes were made
    if ! grep -q "VERSION $VERSION" CMakeLists.txt; then
        echo "Error: Failed to update version in CMakeLists.txt"
        exit 1
    fi
    if ! grep -q "<release version=\"$VERSION\" date=\"$RELEASE_DATE\"/>" "$METADATA"; then
        echo "Error: Failed to update version in $METADATA"
        exit 1
    fi

    echo "Version updated successfully in CMakeLists.txt and AppStream metadata"
    git diff CMakeLists.txt "$METADATA"
    git add CMakeLists.txt "$METADATA"
    git commit -m "chore: bump version to v$VERSION"

commit version:
    #!/usr/bin/env bash
    set -euo pipefail

    VERSION="{{version}}"

    # Validate version format (should be like 1.2.3)
    if ! echo "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
        echo "Error: Version must be in format X.Y.Z (e.g. 1.2.3)"
        exit 1
    fi

    echo "Bumping version to $VERSION..."

    # Check if working directory is clean
    if ! git diff --quiet || ! git diff --cached --quiet; then
        echo "Error: Working directory is not clean. Please commit or stash changes first."
        git status --porcelain
        exit 1
    fi

    git push origin HEAD

    git tag "v$VERSION"
    git push origin "v$VERSION"
