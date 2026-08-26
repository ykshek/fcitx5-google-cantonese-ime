# Justfile with convenient tasks to run the daemon, build locally, and build in podman container

setup-venv:
	python3 -m venv .venv && . .venv/bin/activate && pip install -r daemon/requirements.txt

run-daemon:
	. .venv/bin/activate && python daemon/server.py

build-local:
	mkdir -p cpp/build && cd cpp/build && cmake .. && make -j$(nproc)

podman-build-container:
	podman build -t fcitx5-google-ime-build -f Containerfile .

podman-build:
	# Build the project inside the container and drop artifacts into the host workspace
	podman run --rm -v "$(pwd)":/work:Z -w /work fcitx5-google-ime-build /bin/bash -c "mkdir -p cpp/build && cd cpp/build && cmake .. && make -j$(nproc)"

# Quick test against the daemon (requires the daemon to be running)
curl-suggest q:
	@echo curl "http://127.0.0.1:8765/suggest?q={{q}}&itc=zh-t-i0-pinyin&num=8"
