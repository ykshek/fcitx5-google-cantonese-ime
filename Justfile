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
    test -f "$flatpak_files/share/fcitx5/imicon/google-ime.svg"
    test -f "$flatpak_files/share/metainfo/org.fcitx.Fcitx5.Addon.GoogleIME.metainfo.xml"

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
