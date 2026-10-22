# Justfile with convenient tasks to build, install, and package the fcitx5 plugin.
# The plugin calls Google Input Tools directly over HTTPS; there is no
# separate daemon to run.
image_name := "localhost/fcitx5-google-cantonese-ime:dev"

build-local:
    mkdir -p build && cd build && cmake .. -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX:-/usr} && make -j$(nproc)

build-container:
    #!/usr/bin/env bash
    echo "Building development container image..."
    podman build -t {{image_name}} -f Containerfile .

build:
    #!/usr/bin/env bash
    set -euo pipefail

    # Build using the development container with host system access
    rm -rf ./build
    sleep 1
    podman run --rm \
        --userns=keep-id \
        --volume "$(pwd):/workspace:Z" \
        --workdir /workspace \
        {{image_name}} \
        bash -c '
            set -euo pipefail
            mkdir -p build
            cd build
            cmake .. \
                -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE:-Release} \
                -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX:-/usr}
            make -j$(nproc)
        '

# Quick test: query Google Input Tools directly and print the raw JSON response.
# Useful for checking network access and the itc code without building.
query-google q:
    @curl -sS "https://inputtools.google.com/request?text={{q}}&itc=yue-hant-t-i0-und&num=8"

install:
    #!/usr/bin/env bash
    set -euo pipefail

    # Install using the development container with host system access
    podman run --rm \
        --userns=keep-id \
        --volume "$(pwd):/workspace:Z" \
        --workdir /workspace/build \
        {{image_name}} \
        bash -c '
            # Install into a staging prefix, then mirror onto the host /usr.
            mkdir -p /workspace/build/prefix
            make install DESTDIR=/workspace/build/prefix
        '
    tree build/prefix
    sudo cp -v ./build/prefix/usr/lib64/fcitx5/libfcitx5-google-ime.so /usr/lib64/fcitx5/libfcitx5-google-ime.so
    sudo cp -v ./build/prefix/usr/share/fcitx5/addon/google-ime.conf /usr/share/fcitx5/addon/google-ime.conf
    sudo cp -v ./build/prefix/usr/share/fcitx5/inputmethod/google-ime.conf /usr/share/fcitx5/inputmethod/google-ime.conf
    sudo ldconfig

rpm: build-container
    #!/usr/bin/env bash
    set -euo pipefail
    # Get project version from the spec file
    VERSION=$(grep '^Version:' fcitx5-google-cantonese-ime.spec | awk '{print $2}')

    # Create RPM build structure
    mkdir -p rpmbuild/{SOURCES,SPECS,BUILD,RPMS,SRPMS}

    # Create source tarball
    echo "Creating source tarball..."
    git archive --format=tar.gz --prefix=v${VERSION}/ HEAD > rpmbuild/SOURCES/v${VERSION}.tar.gz

    # Copy spec file
    cp fcitx5-google-cantonese-ime.spec rpmbuild/SPECS/

    # Build RPM using development container
    podman run --rm \
        --userns=keep-id \
        --volume "$(pwd):/workspace:Z" \
        --workdir /workspace \
        {{image_name}} \
        bash -c "
            set -euo pipefail

            # Build the RPM
            rpmbuild --define '_topdir /workspace/rpmbuild' \
                     --define 'version ${VERSION}' \
                     -ba rpmbuild/SPECS/fcitx5-google-cantonese-ime.spec
        "

    echo "RPM build complete!"
    echo "Built packages:"
    find rpmbuild/RPMS -name "*.rpm" -type f
    find rpmbuild/SRPMS -name "*.rpm" -type f

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
    if [[ ! -f "$flatpak_files/lib/fcitx5/libfcitx5-google-ime.so" && \
          ! -f "$flatpak_files/lib64/fcitx5/libfcitx5-google-ime.so" ]]; then
        echo "Error: Flatpak add-on library was not installed under lib or lib64"
        exit 1
    fi
    test -f "$flatpak_files/share/fcitx5/addon/google-ime.conf"
    test -f "$flatpak_files/share/fcitx5/inputmethod/google-ime.conf"
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

    # Update the version in the spec file
    echo "Updating version in fcitx5-google-cantonese-ime.spec..."
    sed -i "s/^Version:.*/Version:        $VERSION/" fcitx5-google-cantonese-ime.spec

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
    if ! grep -q "^Version:[[:space:]]*$VERSION" fcitx5-google-cantonese-ime.spec; then
        echo "Error: Failed to update version in fcitx5-google-cantonese-ime.spec"
        exit 1
    fi
    if ! grep -q "VERSION $VERSION" CMakeLists.txt; then
        echo "Error: Failed to update version in CMakeLists.txt"
        exit 1
    fi
    if ! grep -q "<release version=\"$VERSION\" date=\"$RELEASE_DATE\"/>" "$METADATA"; then
        echo "Error: Failed to update version in $METADATA"
        exit 1
    fi

    echo "Version updated successfully in spec, CMakeLists.txt, and AppStream metadata"
    git diff fcitx5-google-cantonese-ime.spec CMakeLists.txt "$METADATA"
    git add fcitx5-google-cantonese-ime.spec CMakeLists.txt "$METADATA"
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
