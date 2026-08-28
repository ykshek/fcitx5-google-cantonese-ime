## Warning

This is a mostly vibe-coded slop project. Code quality is not guaranteed and has the possibility of going down the drain.

This IME thingamajig makes HTTPS requests to Google Input Tools to send your inputs through the Internet to Google, and will come with all the caveats of that (latency, privacy, and dependence on a third-party endpoint you don't control).

## fcitx5 Google Input Tools IME

<img width="408" height="430" alt="20260828_094513" src="https://github.com/user-attachments/assets/94b6bf97-dcc7-40bf-b2af-6dee55f281e1" />

This project provides a prototype that connects fcitx5 to Google Input Tools. (and make it so that I don't have to open a browser everytime I want to type Cantonese)

## Architecture

The plugin is self-contained: a single fcitx5 shared library queries Google Input Tools directly over HTTPS and parses the JSON response itself. There is **no separate daemon** and **no Python runtime** to start or supervise.

```
keystroke
   │
   ▼
GoogleIMEEngine::keyEvent   (fcitx5 main thread)
   │  append char to composition buffer, show preedit
   │  bump sequence counter, spawn worker thread
   ▼
get_candidates()            (worker thread)
   │  libcurl HTTPS GET  ->  https://inputtools.google.com/request?text=..&itc=..&num=..
   │  nlohmann::json parse -> ["SUCCESS", [["<orig>", [cand, ...], ...]]]
   ▼
eventLoop().addDeferEvent   (back on fcitx5 main thread)
   │  sequence-guarded updateUI -> candidate panel
   ▼
select candidate -> commitString
```

Key points:

- `cpp/src/daemon_client.{h,cc}` — `get_candidates(text, itc, num)` performs the HTTPS request with libcurl and parses the response with nlohmann/json. `curl_global_init` is run exactly once via `std::call_once` (the function is called from detached worker threads inside fcitx5).
- `cpp/src/engine.{h,cc}` — the fcitx5 `InputMethodEngine`. It maintains a composition buffer, runs `get_candidates()` asynchronously, posts the result back to the main event loop via `addDeferEvent`, and renders candidates through a custom `CandidateList`. A sequence counter drops stale results when the user keeps typing.
- nlohmann/json decodes `\uXXXX` escapes (and UTF-16 surrogate pairs) into real UTF-8, so the engine no longer needs a hand-rolled string unescaper.
- The Google endpoint expects `text=` (not `q=`); the old Python daemon accepted `q=` and translated it to `text` internally — the plugin now passes `text` directly.

The Cantonese input code is `yue-hant-t-i0-und` (Traditional Chinese, Cantonese). You can pass a different `itc` for other Google Input Tools layouts, e.g. `zh-t-i0-pinyin` for Mandarin pinyin. The `itc` is currently hardcoded in `engine.cc` (`get_candidates(probe, "yue-hant-t-i0-und", 8)`).

## Quick start

There is no daemon to run anymore. Build the plugin, install it, then enable the input method in fcitx5.

```bash
# 1. Build (needs fcitx5 dev headers, libcurl, nlohmann/json)
just build-local

# 2. Install the .so + metadata
just install

# 3. Restart fcitx5 and enable "Google IME" (粵) in your input method list
fcitx5 -r -d
```

### Manual query test (no build needed)

You can sanity-check that Google Input Tools is reachable and that the `itc` code returns Cantonese, without building anything:

```bash
curl "https://inputtools.google.com/request?text=nei&itc=yue-hant-t-i0-und&num=8"
```

## Building

`just` scripts are included to conveniently build in a containerized way using Podman:

```bash
just build-container   # build the Fedora dev container image
just build             # build the C++ plugin inside the container
just build-local       # build locally (host must have the dev deps)
just install           # copy the .so + metadata into /usr/local/.../fcitx5
```

### Build dependencies

- `fcitx5-devel` (and `fcitx5-qt-devel`, `extra-cmake-modules`)
- `libcurl-devel` (with SSL/TLS support — default on most distros)
- `nlohmann/json` — provided by the system `json-devel` (Fedora) / `nlohmann-json3-dev` (Debian). If the system package is missing, CMake's `FetchContent` will fetch it from GitHub at configure time (needs `git` + network during `cmake`).

## Installation

Very rudimentary for now, use `just install`, which copies the relevant files to the correct places. However, note that you may also have to either:

- `sudo cp /usr/local/lib/fcitx5/libfcitx5-google-ime.so /usr/lib64/fcitx5/libfcitx5-google-ime.so`, or
- `echo "/usr/local/lib" | sudo tee -a /etc/ld.so.conf.d/google-ime.conf`

as otherwise `fcitx5` may not recognize the `.so` in `/usr/local`.

After installing, restart fcitx5 (`fcitx5 -r -d`) and add the "Google IME" input method in the fcitx5 configuration tool.

## Usage

- Type romanized input (latin letters). The preedit shows what you typed.
- Candidates appear automatically after a short debounce; press `1`-`9` (and `0`) to select one, or `Space` for the highlighted candidate.
- `Up`/`Down` move the highlight through the candidate list, wrapping across pages at the boundaries. `PageUp`/`PageDown` switch candidate pages directly.
- `Enter` commits the raw buffer (the typed latin letters) as an escape hatch.
- `Backspace` deletes the last character of the query and re-queries Google.
- `Escape` cancels the current composition.

## Behavior notes

- **Debounce / rate limiting.** Google Input Tools' public endpoint returns no rate-limit headers and does not send `429` for normal use (each request is ~100 ms). The official "try" page coalesces rapid keystrokes client-side rather than firing one request per key, so this plugin does the same: a 150 ms debounce timer is reset on every keystroke, so only the final query is sent. Stale in-flight responses are discarded via a per-input-context generation counter, so fast typing can never show an outdated candidate list.
- **Per-input-context state.** Composition state (buffer, candidates, page, cursor) is stored per input context via fcitx5's `InputContextProperty`, so switching focus between text fields no longer corrupts the composition.

## Notes

`engine.cc` is a feature-complete fcitx5 `InputMethodEngine` that builds against `Fcitx5::Core` and implements the full keypress → async Google query → candidate panel → commit loop.

Most other things are unfinished and will have a lot of bugs. Known gaps: no config UI despite the addon being marked `Configurable=True`, and no result caching across identical queries.
