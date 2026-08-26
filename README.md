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