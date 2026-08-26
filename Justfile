# Justfile with convenient tasks to run the daemon, build locally, and build in podman container
image_name := "localhost/fcitx5-google-ime:dev"

setup-venv:
    python3 -m venv .venv && . .venv/bin/activate && pip install -r daemon/requirements.txt

run-daemon:
    . .venv/bin/activate && python daemon/server.py

build-local:
    mkdir -p cpp/build && cd cpp/build && cmake .. && make -j$(nproc)
build-container:
    #!/usr/bin/env bash
    echo "Building development container image..."
    podman build -t {{image_name}} -f Containerfile .

build:
    #!/usr/bin/env bash
    set -euo pipefail

    # Install using the development container with host system access
    echo "removing:"
    tree ./cpp/build
    sleep 1
    rm -rf -v ./cpp/build
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

# Quick test against the daemon (requires the daemon to be running)
curl-suggest q:
    @echo curl "http://127.0.0.1:8765/suggest?q={{q}}&itc=zh-t-i0-pinyin&num=8"

install:
    #!/usr/bin/env bash
    sudo cp cpp/build/libfcitx5-google-ime.so /usr/local/lib/fcitx5/modules/
    sudo cp cpp/build/libfcitx5-google-ime.so /usr/local/lib/fcitx5/
    sudo cp cpp/data/addon/google-ime.conf /usr/local/share/fcitx5/addon/
    sudo cp cpp/data/inputmethod/google-ime.conf /usr/local/share/fcitx5/inputmethod/
    sudo chmod 644 /usr/local/share/fcitx5/addon/google-ime.conf /usr/local/share/fcitx5/inputmethod/google-ime.conf
    sudo ldconfig
