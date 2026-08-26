fcitx5 Google Input Tools IME

This project provides a prototype that connects fcitx5 to Google Input Tools.

Structure:
- daemon/: Python daemon that queries Google Input Tools and exposes a local HTTP API (/suggest)
- cpp/: C++ skeleton for a fcitx5 input-method module that forwards composition/candidate requests to the daemon (needs fcitx5 dev headers to build)

Quick start (daemon prototype):

1. Create a virtualenv and install dependencies:

   python3 -m venv .venv
   source .venv/bin/activate
   pip install -r daemon/requirements.txt

2. Run the daemon:

   python daemon/server.py

3. Test:

   curl "http://127.0.0.1:8765/suggest?q=nei&itc=zh-t-i0-pinyin&num=8"

Notes and next steps:
- The Python daemon is a working prototype. To make a native fcitx5 IME, build the C++ plugin in cpp/ and register it as an fcitx5 addon. The cpp/ code here is a starting skeleton that needs the fcitx5 development headers and a small amount of glue to compile and register as an IM engine.
- After building and installing the fcitx5 plugin, the plugin should forward composition events to the daemon and display the returned candidates.

If you want, proceed now and the next step will be: create the C++ plugin integration (implementation + build instructions) and test it against the running daemon.

Podman / containerized build (Fedora)

A Containerfile is provided to build the C++ plugin in a reproducible Fedora container. Example usage:

  # Build the container image
  podman build -t fcitx5-google-ime-build -f Containerfile .

  # Build the project inside the container (artifacts written to host workspace)
  podman run --rm -v "$(pwd)":/work -w /work fcitx5-google-ime-build /bin/bash -c "mkdir -p cpp/build && cd cpp/build && cmake .. && make -j$(nproc)"

Justfile

A Justfile is included with convenient targets:
- just setup-venv     # setup python venv and install deps for daemon
- just run-daemon     # run the daemon
- just build-local    # build the C++ skeleton locally
- just podman-build   # build the C++ project inside the Fedora container

Notes

- The C++ code is a near-complete skeleton that includes the HTTP client and clear integration points for wiring into fcitx5's engine APIs. Building the final plugin requires the fcitx5 development headers on the build host or in the container.
- After building and installing the plugin into the appropriate fcitx5 modules directory, restart fcitx5 and enable the input method.
