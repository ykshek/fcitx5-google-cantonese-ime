## Warning

This is a mostly vibe-coded slop project. Code quality is not guaranteed and has the possibility of going down the drain.

This IME thingamajig makes HTTPS requests to Google Input Tools to send your inputs through the Internet to Google, and will come with all the caveats of that (latency, privacy, and dependence on a third-party endpoint you don't control).

## fcitx5 Google Input Tools IME

<img width="408" height="430" alt="20260828_094513" src="https://github.com/user-attachments/assets/94b6bf97-dcc7-40bf-b2af-6dee55f281e1" />

This project provides an IME that connects fcitx5 to Google Input Tools. (and make it so that I don't have to open a browser everytime I want to type Cantonese)

## Architecture

A fcitx5 shared library that queries Google Input Tools directly over HTTPS and parses the JSON response using the nlohmann/json library.

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

- `src/daemon_client.{h,cc}` — `get_candidates(text, itc, num)` performs the HTTPS request with libcurl and parses the response with nlohmann/json. `curl_global_init` is run exactly once via `std::call_once` (the function is called from detached worker threads inside fcitx5).
- `src/engine.{h,cc}` — the fcitx5 `InputMethodEngine`. It maintains a composition buffer, runs `get_candidates()` asynchronously, posts the result back to the main event loop via `addDeferEvent`, and renders candidates through the `CandidateList`. A sequence counter drops stale results when the user keeps typing.
- nlohmann/json decodes `\uXXXX` escapes (and UTF-16 surrogate pairs) into real UTF-8.
- Google's endpoint expects `text=` separated by `|` and `,` to check both preedit and the context of previous words to give a better prediction.
- The response from the HTTPS request also sends back the proper "spelling" of each word, which is also shown in the CandidateList with subtext.

The Cantonese input code is `yue-hant-t-i0-und` (Traditional Chinese, Cantonese, 廣東話輸入法). You can pass a different `itc` for other Google Input Tools layouts, e.g. `zh-t-i0-pinyin` for Mandarin pinyin. The `itc` is **user-configurable** — see [Configuration](#configuration) — and defaults to `yue-hant-t-i0-und` (compiled in as `GoogleIMEEngine::kInputCode`, used as the fallback when the user leaves the field blank).

## Quick start

### Fedora (RPM)

Prebuilt RPMs are published on the GitHub Releases page (and via COPR). Install
the package

```bash
sudo dnf5 copr enable ykshek/fcitx5-google-cantonese-ime
sudo dnf install fcitx5-google-cantonese-ime
```

Then restart fcitx5 and enable "Google IME" (粵) in your input method list

### Build from source

```bash
# 1. Build (needs fcitx5 dev headers, libcurl, nlohmann/json)
just build-local

# 2. Install the .so + metadata into /usr
just install

# 3. Restart fcitx5 and enable "Google IME" (粵) in your input method list
fcitx5 -r -d
```

### Manual query test

You can sanity-check that Google Input Tools is reachable and that the `itc` code returns a proper response(e.g. if geoblocked), without building anything:

```bash
curl "https://inputtools.google.com/request?text=nei&itc=yue-hant-t-i0-und&num=8"
```

## Building

`just` scripts are included to conveniently build in a containerized way using Podman:

```bash
just build-container   # build the Fedora dev container image
just build             # build the C++ plugin inside the container
just build-local       # build locally (host must have the dev deps)
just install           # install the .so + metadata into /usr
just rpm               # build the RPM locally inside the container
```

### Build dependencies

- `fcitx5-devel` (and `extra-cmake-modules`)
- `libcurl-devel` (with SSL/TLS support — default on most distros)
- `nlohmann/json` — provided by the system `json-devel` (Fedora) / `nlohmann-json3-dev` (Debian). If the system package is missing, CMake's `FetchContent` should fetch it from GitHub at configure time (needs `git` + network during `cmake`).

## Installation

`just install` installs 3 files into `/usr`:

- `/usr/lib64/fcitx5/libfcitx5-google-ime.so`
- `/usr/share/fcitx5/addon/google-ime.conf`
- `/usr/share/fcitx5/inputmethod/google-ime.conf`

After installing, restart fcitx5 (`fcitx5 -r -d`) and add the "Google IME" input method in the fcitx5 configuration tool.

## Usage

- Type romanized input (latin letters). The preedit shows what you typed.
- Candidates appear automatically after a short debounce; press `1`-`9` (and `0`) to select one, or `Space` for the highlighted candidate.
- `Up`/`Down` move the highlight through the candidate list, wrapping across pages at the boundaries. `PageUp`/`PageDown` switch candidate pages directly.
- `Enter` commits the raw buffer (the typed latin letters) as an escape hatch.
- `Backspace` deletes the last character of the query and re-queries Google.
- `Escape` cancels the current composition.

## Configuration

The input layout (the Google Input Tools `itc` code, sent as the `&itc=` query parameter) is user-configurable through fcitx5's standard configuration GUI — which on KDE Plasma is the **Input Method** module in KDE Plasma Settings (the `kcm_fcitx5` KCM), and `fcitx5-configtool` elsewhere. The addon metadata sets `Configurable=True`, and the engine exposes a `GoogleIMEConfig` (`src/config.h`) with a single `InputCode` option; fcitx5 introspects that option and generates the text field in the settings UI automatically, with no separate config-description file or KCM-specific code.

To change the layout:

1. Open **KDE Plasma Settings → Input Method** (or run `fcitx5-configtool`).
2. Select the **Google IME** input method in the list and click its **Configure** (gear) button.
3. Edit the **InputCode** field to the Google Input Tools code for the layout you want (see the table below) and apply.

The chosen value is saved to `~/.config/fcitx5/conf/google-ime.conf` and takes effect on the next keystroke (an in-flight request for the previous layout is discarded, so switching layouts mid-composition never commits stale candidates). Leave the field blank to fall back to the compiled-in Cantonese default.

Common `itc` codes:

| Code | Layout |
|---|---|
| `yue-hant-t-i0-und` | Cantonese, Traditional (default) |
| `zh-t-i0-pinyin` | Mandarin pinyin |
| `zh-t-i0-wubi` | Wubi (simplified) |
| `zh-hant-t-i0-cangjie` | Cangjie, Traditional |
| `zh-t-i0-bopomofo` | Bopomofo / Zhuyin |

## Behavior notes

- **Debounce / rate limiting.** Google Input Tools' public endpoint returns no rate-limit headers and does not send `429` for normal use (each request is ~100 ms). The official "try" page coalesces rapid keystrokes client-side rather than firing one request per key, so this plugin does the same: a 150 ms debounce timer is reset on every keystroke, so only the final query is sent. Stale in-flight responses are discarded via a per-input-context generation counter, so fast typing can never show an outdated candidate list.
- **Per-input-context state.** Composition state (buffer, candidates, page, cursor) is stored per input context via fcitx5's `InputContextProperty`, so switching focus between text fields no longer corrupts the composition.

## Notes

`engine.cc` is a feature-complete fcitx5 `InputMethodEngine` that builds against `Fcitx5::Core` and implements the full keypress → async Google query → candidate panel → commit loop.

Known gaps: no result caching across identical queries.
