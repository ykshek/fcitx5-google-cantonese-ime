## Notice

Use rime with https://github.com/rime/rime-jyutping instead. It provides a similar Cantonese typing experience without the need to send your texts online! Of course, you can install both flatpak plugins and switch between the two if one result isn't satisfactory.

## WARNING

This is a mostly vibe-coded project. Code quality is not guaranteed and has the possibility of severe bugs.

This IME thingamajig makes HTTPS requests to Google Input Tools to send your inputs through the Internet to Google, and will come with all the caveats of that (latency, privacy, and dependence on a third-party endpoint you don't control).

## Google and icon disclaimer

This project is independent and is not affiliated with, endorsed by, or sponsored
by Google. Google owns its own names, logos, and Input Tools iconography; this
project does not claim ownership of Google's icon supplied as a visual
reference, and that icon is not included here. The add-on uses the
`fcitx-googlepinyin-panel` icon from the system icon theme (Papirus); no icon
artwork is included in this repository.

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

The Cantonese input code is `yue-hant-t-i0-und` (Traditional Chinese, Cantonese, 廣東話輸入法). You can pass a different `itc` for other Google Input Tools layouts, e.g. `zh-t-i0-pinyin` for Mandarin pinyin. The `itc` is **user-configurable** — see [Configuration](#configuration) — and defaults to Cantonese (`yue-hant-t-i0-und`, compiled in as `GoogleIMEEngine::kInputCode`, used as the fallback when no layout is selected).

## Quick start

### Fcitx5 Flatpak

The repository includes a Flatpak add-on manifest targeting the Flathub
`org.fcitx.Fcitx5` app. With Flatpak and the Flathub remote available, build
and install the extension for your user:

```bash
just install-flatpak
```

This installs the Flatpak-packaged `org.flatpak.Builder` if it is not already
installed, then builds and installs the add-on. It also links the Papirus tray
icon as described in [Tray icon](#tray-icon).

The add-on is installed under the Fcitx5 Flatpak's extension point; it does not
install files into the host's `/usr`. The Fcitx5 Flatpak must be installed and
running for the add-on to be loaded. The engine sends queries to Google Input
Tools, so it requires the network permission already granted by the Fcitx5
Flatpak.

Restart Fcitx5 after installing or updating the add-on, otherwise the already
running instance keeps using the previous extension:

```bash
flatpak kill org.fcitx.Fcitx5   # start it again the way you normally do
```

Tagged releases publish an installable Flatpak bundle and `SHA256SUMS` on the
GitHub Releases page. Install a downloaded bundle with:

```bash
flatpak install --user ./org.fcitx.Fcitx5.Addon.GoogleIME-VERSION-x86_64.flatpak
```

Bundle installations do not run the repository's `just` recipes; run
`just install-tray-icon` once from a clone to get the tray icon (see below).

### Tray icon

The input method declares the Papirus icon name in
[`data/inputmethod/google-ime.conf.in`](data/inputmethod/google-ime.conf.in):

```ini
Icon=fcitx-googlepinyin-panel
```

That is enough for a native Fcitx5 install, because Papirus-Dark provides
`fcitx-googlepinyin-panel` directly. Inside the Flatpak it is not enough:

- fcitx5 rewrites every icon name that starts with `fcitx` to
  `org.fcitx.Fcitx5.fcitx...` before it exports the
  [StatusNotifierItem](https://www.freedesktop.org/wiki/Specifications/StatusNotifierItem/)
  to the desktop (`IconTheme::iconName` in fcitx5). KDE Plasma or GNOME
  therefore asks its icon theme for
  `org.fcitx.Fcitx5.fcitx-googlepinyin-panel`, which Papirus does not ship.
- The official Flatpak add-ons — Rime included — never hit this because the
  Flatpak for the `org.fcitx.Fcitx5` app itself builds their icons. Its
  `fcitx5-all-icons` module checks out every official add-on repository and
  runs its `install_icons` script, which copies
  `fcitx5*/{data,icons,icon}/{16x16,...,scalable}` into
  `/app/share/icons/hicolor/...`. Rime's `data/` directory ships the icon twice,
  once as `fcitx-rime.*` and once as `org.fcitx.Fcitx5.fcitx-rime.*`, and Flatpak
  exports that directory to the host icon theme, so the prefixed name resolves.
- A third-party add-on cannot add files to the main app, and icons installed
  inside the add-on extension (`/app/addons/GoogleIME/share`) are only visible
  *inside* the sandbox (the Flatpak's `fcitx5.sh` wrapper adds
  `/app/addons/*/share` to `XDG_DATA_DIRS`). The host's tray is outside the
  sandbox and never sees them.

So `just install-flatpak` runs `just install-tray-icon` afterwards. That recipe
links the system Papirus icon into the user's hicolor theme under the prefixed
name the desktop asks for:

```
~/.local/share/icons/hicolor/scalable/apps/org.fcitx.Fcitx5.fcitx-googlepinyin-panel.svg
    -> ~/.local/share/icons/Papirus-Dark/24x24/panel/fcitx-googlepinyin-panel.svg
```

It is only a symlink: no artwork is copied or vendored, and it follows the
Papirus theme. If the icon still shows as a generic fallback after restarting
Fcitx5, restart the desktop shell (on Plasma: log out and back in) so it drops
any cached "missing icon" result.

## Building

`just` scripts build and install the Fcitx5 Flatpak add-on using the
Flatpak-packaged builder:

```bash
just build-flatpak     # build and verify the Fcitx5 Flatpak add-on
just install-flatpak   # build, install, and link the tray icon
just install-tray-icon # only (re)link the Papirus tray icon on the host
```

The Flatpak build requires the Flathub remote and installs the
`org.flatpak.Builder` Flatpak when needed. `just build-flatpak` builds and
verifies the add-on without installing it; `just install-flatpak` builds and
installs it for the current user and then links the tray icon.

After installation, restart Fcitx5 (see above) and add the "Google IME" input
method in the Fcitx5 configuration tool.

## Usage

- Type romanized input (latin letters). The preedit shows what you typed.
- Candidates appear automatically after a short debounce; press `1`-`9` to select a visible candidate, or `Space` for the highlighted candidate.
- The candidate panel shows up to the configured number of rows. Navigation advances through every candidate; Google results are fetched in batches of nine. Digits that do not label a visible candidate remain available as romanized/tone input.
- `Up`/`Down` move the highlight through the candidate list, wrapping across pages at the boundaries. `PageUp`/`PageDown` switch candidate pages directly.
- `Enter` commits the raw buffer (the typed latin letters) as an escape hatch.
- `Backspace` deletes the last character of the query and re-queries Google.
- `Escape` cancels the current composition.

## Configuration

The input layout (the Google Input Tools `itc` code, sent as the `&itc=` query parameter) is user-configurable through fcitx5's standard configuration GUI — which on KDE Plasma is the **Input Method** module in KDE Plasma Settings (the `kcm_fcitx5` KCM), and `fcitx5-configtool` elsewhere. The addon metadata sets `Configurable=True`, and the engine exposes a `GoogleIMEConfig` (`src/config.h`) with a **Layout** dropdown and a **CustomInputCode** text field; fcitx5 introspects those options and generates the dropdown + text box in the settings UI automatically, with no separate config-description file or KCM-specific code.

To change the layout:

1. Open **KDE Plasma Settings → Input Method** (or run `fcitx5-configtool`).
2. Select the **Google IME** input method in the list and click its **Configure** (gear) button.
3. Pick a layout from the **Input layout** dropdown. For **Custom**, also fill in the **CustomInputCode** field with the Google Input Tools `itc` code you want, then apply.

The chosen value is saved to `~/.config/fcitx5/conf/google-ime.conf` and takes effect on the next keystroke (an in-flight request for the previous layout is discarded, so switching layouts mid-composition never commits stale candidates). If **Custom** is selected but the code field is left blank, the engine falls back to the compiled-in Cantonese default.

The same settings include **Preedit length limit (characters)** (default 64)
and **When the preedit length limit is reached**. The available behaviors are
to commit the raw romanization and continue, select the first fresh candidate
(waiting for a lookup when necessary and falling back to raw romanization if no
candidates return), or stop accepting additional input until the preedit is
shortened or cleared.

The **When composition is reset** setting controls what happens to unfinished
preedit when Fcitx5 resets the composition, including when switching away from
the IME with Shift or Ctrl+Space. It defaults to **Commit raw preedit**; choose
**Discard preedit** for the previous behavior, or **Select first candidate** to
commit the first fresh candidate. If no fresh candidate is available, the raw
preedit is committed instead; any romanization left over after a partial
candidate is committed raw as well.

**Candidates per page** controls how many rows the candidate panel displays
(default 9, maximum 9). Smaller values shorten each visible page, while
navigation still traverses the complete result list without skipping entries.
Google lookups continue to request results in batches of nine. Number keys
without a visible candidate label are treated as romanized/tone input.

Built-in layouts and their `itc` codes (verified live against `https://inputtools.google.com/request`):

| Layout | `itc` code |
|---|---|
| Cantonese (default) | `yue-hant-t-i0-und` |
| Mandarin Pinyin | `zh-t-i0-pinyin` |
| Wubi | `zh-t-i0-wubi-1986` |
| Cangjie | `zh-hant-t-i0-cangjie-1982` |
| Custom | (your `itc` code in the CustomInputCode field) |

> You can find the `itc` code by using the browser inspect menu inside the Network pane on the Google Input Tools site.

## Behavior notes

- **Debounce / rate limiting.** Google Input Tools' public endpoint returns no rate-limit headers and does not send `429` for normal use (each request is ~100 ms). A `kDebounceMs` (15 ms) debounce timer is reset on every keystroke; that is short enough to coalesce only genuinely-simultaneous events (e.g. IME engines that synthesize multiple key events per physical keystroke) while still firing effectively one request per real keystroke (matching observed Google behavior, where successive requests are ~50-120 ms apart). Stale in-flight responses are discarded via a per-input-context generation counter.
- **Per-input-context state.** Composition state (buffer, candidates, page, cursor) is stored per input context via fcitx5's `InputContextProperty`, so switching focus between text fields does not corrupt the composition.
- **Prediction context.** Comma-like punctuation (including `，` and `、`) remains in the context sent to Google; sentence-ending punctuation clears it.

## Notes

`engine.cc` is a feature-complete fcitx5 `InputMethodEngine` that builds against `Fcitx5::Core` and implements the full keypress → async Google query → candidate panel → commit loop.
