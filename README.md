## Warning

This is a mostly vibe-coded slop project. Code quality is not guaranteed and has the possibility of going down the drain.

This IME thingamajig uses HTTP GET requests to Google Input Tools to send your inputs through the Internet to Google Input Tools, and will come with all the caveats of that.

## fcitx5 Google Input Tools IME

This project provides a prototype that connects fcitx5 to Google Input Tools. (and make it so that I don't have to open a browser everytime I want to type Cantonese)

Structure:
- daemon/: Python daemon that queries Google Input Tools and exposes a local HTTP API (/suggest)
- cpp/: A fcitx5 input-method module that forwards composition/candidate requests to the daemon (needs fcitx5 dev headers to build)

Quick start (daemon prototype):

1. Create a virtualenv and install dependencies:

   python3 -m venv .venv
   source .venv/bin/activate
   pip install -r daemon/requirements.txt

2. Run the daemon:

   python daemon/server.py

3. Test:

   curl "http://127.0.0.1:8765/suggest?q=leihou&itc=yue-hant-t-i0-und&num=8"


## Building

`just` scripts are included to conveniently build and test stuff in a containerized way using Podman:

```bash
just setup-venv     # setup python venv and install deps for daemon
just run-daemon     # run the daemon
just build-local    # build the C++ skeleton locally
just build-container # build the container
just build   # build the C++ project inside the Fedora container
```

## Installation

Very rudimentary for now, use `just install`, which copies the relavant files to the correct places. However, note that you may also have to either:

- `sudo cp /usr/local/lib/fcitx5/libfcitx5-google-ime.so /usr/lib64/fcitx5/libfcitx5-google-ime.so`, or
- `echo "/usr/local/lib" | sudo tee -a /etc/ld.so.conf.d/google-ime.conf`

as otherwise`fcitx5` may not recognize the `.so` in `/usr/local`.



## Notes

`engine.cc` is a feature-complete fcitx5 InputMethodEngine that builds against `Fcitx5::Core` and implements the full keypress → async Google query → candidate panel → commit loop.

Most other things are unfinished and will have a lot of bugs.
