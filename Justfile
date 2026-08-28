# Justfile with convenient tasks to build and install the fcitx5 plugin.
# The plugin calls Google Input Tools directly over HTTPS; there is no
# separate daemon to run.
image_name := "localhost/fcitx5-google-ime:dev"

build-local:
    mkdir -p cpp/build && cd cpp/build && cmake .. && make -j$(nproc)

build-container:
    #!/usr/bin/env bash
    echo "Building development container image..."
    podman build -t {{image_name}} -f Containerfile .

build:
    #!/usr/bin/env bash
    set -euo pipefail

    # Build using the development container with host system access
    rm -rf ./cpp/build
    sleep 1
    podman run --rm \
        --userns=keep-id \
        --volume "$(pwd):/work:Z" \
        --workdir /work \
        {{image_name}} \
        bash -c '
            mkdir -p cpp/build
            cd cpp/build
            cmake ..
            make -j$(nproc)
        '

# Quick test: query Google Input Tools directly and print the raw JSON response.
# Useful for checking network access and the itc code without building.
query-google q:
    @curl -sS "https://inputtools.google.com/request?text={{q}}&itc=yue-hant-t-i0-und&num=8"

install:
    #!/usr/bin/env bash
    sudo cp cpp/build/libfcitx5-google-ime.so /usr/local/lib/fcitx5/
    sudo cp cpp/data/addon/google-ime.conf /usr/local/share/fcitx5/addon/
    sudo cp cpp/data/inputmethod/google-ime.conf /usr/local/share/fcitx5/inputmethod/
    sudo chmod 644 /usr/local/share/fcitx5/addon/google-ime.conf /usr/local/share/fcitx5/inputmethod/google-ime.conf
    sudo ldconfig
