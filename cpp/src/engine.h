#pragma once

#include <fcitx/addonfactory.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>              // must come before candidatelist.h
#include <fcitx/candidatelist.h>     // for CandidateWord / CandidateList
#include <fcitx/inputcontext.h>      // for InputContext
#include <fcitx-utils/event.h>       // for EventSource / EventLoop
#include <fcitx-utils/trackableobject.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "daemon_client.h"  // GoogleCandidate

// Number of candidates shown per page in the candidate panel.
inline constexpr int kPageSize = 9;

// Per-InputContext composing state. This is the single source of truth for
// everything the engine renders: the preedit (raw romanized buffer still
// being typed), the Chinese context already committed by this IME (used only
// to help Google's prediction, never shown), the full candidate set for the
// current query, the current page / cursor within that page, and a
// monotonically increasing "generation" counter used to discard stale async
// Google responses.
//
// Storing this per input context (via fcitx5's InputContextProperty) fixes a
// latent bug in the previous design, which kept a single `buffer_` member on
// the (singleton) engine shared across every input context — so switching focus
// between two text fields would corrupt the composition state.
struct GoogleIMEState : public fcitx::InputContextProperty {
    // Current latin (jyutping) preedit being typed. This is the source of
    // truth for the preedit: it is always rebuilt from `preedit` on every
    // render, never derived from the displayed text.
    std::string preedit;

    // Chinese text already committed by this IME earlier in the current
    // sentence (i.e. by a previous candidate selection), used only as
    // prediction context sent to Google — see buildProbe(). Kept as a sliding
    // window of at most kMaxContextChars codepoints, matching what Google's
    // own web client does. Reset whenever composition ends (Enter/Escape/
    // punctuation-flush/focus change) since at that point there's no more
    // "sentence" for Google to continue predicting from.
    std::string committed;

    // Full candidate list returned by Google for the current query, together
    // with how many bytes of `preedit` (from the front) each one consumes.
    // The panel renders a kPageSize-wide window into this vector starting at
    // `page * kPageSize`.
    //
    // The list is intentionally NOT cleared immediately when the user keeps
    // typing (unlike the old behavior, which hid the list until the next
    // response arrived, making typing feel jumpy and flickery). Instead the
    // old list stays visible as a best-effort preview while the new lookup is
    // in flight, and is replaced atomically when the fresh result lands in
    // applyCandidates(). Because stale candidates can have matchedLength
    // values that are wrong relative to the new (longer) preedit, selection
    // paths consult candidatesFresh() — which compares this probe string to
    // a freshly built probe for the current state — before they commit anything.
    std::vector<GoogleCandidate> candidates;

    // Snapshot of buildProbe() at the time the candidates were last computed.
    // Used by candidatesFresh() to guard selection paths from committing an
    // out-of-date candidate list while a newer lookup is still in flight.
    std::string candidatesProbe;

    // Current candidate page (0-based) and the highlighted candidate index
    // within the current page (-1 = none highlighted).
    int page = 0;
    int cursor = 0;

    // Monotonic counter bumped on every edit. An async Google response
    // carries the generation it was issued for; if it no longer matches the
    // current generation when it arrives, it is discarded. This is what makes
    // fast typing safe: stale in-flight responses can never overwrite newer
    // candidate lists.
    uint64_t generation = 0;

    // A pending debounce timer that will fire the next Google request. Kept
    // as a member so each new keystroke can cancel (replace) the previous
    // timer — this is the rate-limiting / debounce mechanism.
    std::unique_ptr<fcitx::EventSourceTime> debounceTimer;

    // Set when the user types punctuation while a lookup is still in flight
    // (no candidates yet to resolve against). We defer emitting the
    // full-width punctuation until the in-flight response arrives (or fails),
    // to avoid mis-committing raw preedit just because the network was a beat
    // slow. 0 = nothing pending.
    char pendingPunctuation = 0;
};

// A minimal CandidateList that owns only the candidates for the CURRENT page.
//
// It deliberately does NOT implement the Pageable / CursorMovable interfaces.
// Doing so would make classicui intercept Up/Down/PageUp/PageDown itself and
// drive navigation through those interfaces — but classicui's render path for
// CommonCandidateList crashed this addon in the past (global cursor -1 ->
// checkGlobalIndex abort). Instead the engine handles Up/Down/PageUp/PageDown
// directly in keyEvent and rebuilds this list each time, which keeps full
// control and avoids that crash entirely.
//
// cursorIndex() returns the highlighted candidate so classicui highlights it;
// selection still happens through MyCandidateWord::select() (number keys /
// space are handled in keyEvent).
class GoogleIMECandidateList : public fcitx::CandidateList {
public:
    GoogleIMECandidateList() = default;

    void append(std::unique_ptr<fcitx::CandidateWord> word, int cursor) {
        const auto idx = static_cast<int>(words_.size());
        words_.push_back(std::move(word));
        // default labels: "1." "2." ... up to "9."
        const char digit = (idx < 9) ? ('1' + idx) : '0';
        labels_.emplace_back(std::string(1, digit) + ".");
        if (idx == cursor) {
            cursorIndex_ = idx;
        }
    }

    int size() const override { return static_cast<int>(words_.size()); }

    void setCursorIndex(int idx) { cursorIndex_ = idx; }

    const fcitx::CandidateWord &candidate(int idx) const override {
        checkIndex(idx);
        return *words_[idx];
    }

    const fcitx::Text &label(int idx) const override {
        if (idx < 0 || idx >= static_cast<int>(labels_.size())) {
            static const fcitx::Text empty;
            return empty;
        }
        return labels_[idx];
    }

    int cursorIndex() const override { return cursorIndex_; }

    fcitx::CandidateLayoutHint layoutHint() const override {
        return fcitx::CandidateLayoutHint::Vertical;
    }

private:
    void checkIndex(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(words_.size())) {
            throw std::invalid_argument("GoogleIMECandidateList: invalid index");
        }
    }
    std::vector<std::unique_ptr<fcitx::CandidateWord>> words_;
    std::vector<fcitx::Text> labels_;
    int cursorIndex_ = -1;
};

class GoogleIMEEngine : public fcitx::InputMethodEngine {
public:
    explicit GoogleIMEEngine(fcitx::Instance *instance = nullptr);
    ~GoogleIMEEngine() override;

    void keyEvent(const fcitx::InputMethodEntry &entry,
                  fcitx::KeyEvent &keyEvent) override;
    void reset(const fcitx::InputMethodEntry &entry,
               fcitx::InputContextEvent &event) override;

    // Rebuild the input panel (preedit + candidate window) from the current
    // per-IC state. Called on the main thread after every buffer edit and
    // after every candidate-navigation key.
    void renderPanel(fcitx::InputContext *ic);

    // Apply a freshly fetched candidate set to the given input context. Runs
    // on the main thread (posted from the worker via an async event). Drops
    // the result if it is stale (generation / probe changed since the
    // request was issued).
    void applyCandidates(fcitx::InputContext *ic,
                         std::vector<GoogleCandidate> candidates,
                         std::string probe, uint64_t gen);

    // Select a candidate: commit its text, extend the committed context,
    // consume `matchedLength` bytes from the front of the preedit, and
    // (if any preedit remains) re-query the remainder — this is what makes
    // word-by-word / partial candidate selection work.
    void selectCandidate(fcitx::InputContext *ic, const std::string &text,
                         int matchedLength);

private:
    // Candidate word. The display text MUST be handed to the
    // fcitx::CandidateWord base constructor: CandidateWord::text() is
    // non-virtual and returns exactly that stored text, and the UI reads the
    // candidate text through the base class. We additionally stash the raw
    // candidate text + matchedLength (display text may differ once we add
    // annotations later, and matchedLength is never part of the display).
    class MyCandidateWord : public fcitx::CandidateWord {
    public:
        MyCandidateWord(fcitx::Text text, GoogleIMEEngine *engine,
                        std::string candText, int matchedLength,
                        std::string probe)
            : fcitx::CandidateWord(std::move(text)), engine_(engine),
              candText_(std::move(candText)), matchedLength_(matchedLength),
              probe_(std::move(probe)) {}
        void select(fcitx::InputContext *ic) const override;

    private:
        GoogleIMEEngine *engine_ = nullptr;
        std::string candText_;
        int matchedLength_ = 0;
        // Snapshot of buildProbe(state) at render time. select() compares this
        // against the current state's candidatesProbe to refuse committing an
        // out-of-date candidate object that was rendered for an earlier keystroke
        // but is still on screen when the user clicks it. This matters most
        // when the list is kept visible as a preview during in-flight updates.
        std::string probe_;
    };

    // Build the probe string sent to Google: just the preedit when there is
    // no committed context yet, otherwise Google's own "|<committed>,<preedit>"
    // format (leading pipe, comma-separated), which is what lets Google use
    // prior composed text as prediction context without treating it as part
    // of the query to be converted.
    std::string buildProbe(const GoogleIMEState *state) const;

    // Trim `committed` down to the last kMaxContextChars UTF-8 codepoints,
    // matching the sliding window Google's own client uses.
    void trimCommitted(GoogleIMEState *state) const;

    // True if the candidate list in `state` was computed from the *current*
    // committed context + preedit (i.e. buildProbe(state) matches
    // state->candidatesProbe). When false, the visible candidates belong to an
    // earlier keystroke and their matchedLength values are stale relative to
    // the (now longer) preedit — selection paths must treat the list as
    // unselectable and force a fresh lookup instead of committing a stale
    // candidate.
    bool candidatesFresh(const GoogleIMEState *state) const;

    // Emit one full-width punctuation character (see kPunctuationMap in
    // engine.cc). If there is an active composition, first resolves it: picks
    // the best full-consumption candidate if one is available, otherwise
    // commits the raw preedit. If candidates are still in flight, defers via
    // state->pendingPunctuation and resolves once they arrive (see
    // applyCandidates).
    void commitPunctuation(fcitx::InputContext *ic, GoogleIMEState *state,
                           char halfWidth);

    // Schedule (or reschedule) the debounced Google request for `ic`. Each
    // call cancels any previous pending timer, so rapid keystrokes coalesce
    // into a single request fired kDebounceMs after the last keystroke.
    void scheduleRequest(fcitx::InputContext *ic, GoogleIMEState *state);

    // Fire the actual (blocking) Google request in a background worker
    // thread, then post the result back to the main event loop via a
    // thread-safe async event.
    void dispatchRequest(fcitx::InputContext *ic, GoogleIMEState *state);

    // Per-input-context state property factory. SimpleInputContextPropertyFactory
    // default-constructs (it does `new GoogleIMEState` per input context).
    fcitx::SimpleInputContextPropertyFactory<GoogleIMEState> stateFactory_;
    fcitx::Instance *instance_{nullptr};

    // Outstanding async event sources (one per in-flight request). Kept alive
    // here so the source is not destroyed before the worker thread calls
    // send() and the main-thread callback runs. Guarded by a mutex because
    // the worker thread adds to it and the main-thread callback removes from
    // it.
    std::vector<std::unique_ptr<fcitx::EventSource>> pendingEvents_;
    std::mutex pendingEventMutex_;

    // Debounce window (ms). A HAR capture of the official Google Input Tools
    // "try" page shows it issues one network request per keystroke with no
    // meaningful client-side coalescing (successive requests ~50-120ms apart
    // as the user types normally) — so a debounce anywhere near the previous
    // 150ms is far more conservative than the real product. We lower it to
    // 15ms: this still coalesces genuinely-simultaneous events (e.g. IME
    // engines that synthesize multiple key events per physical keystroke)
    // while firing a request on effectively every real keystroke, matching
    // observed Google behavior. Google's public endpoint returns no
    // rate-limit headers and does not return 429 for normal use, so this
    // debounce (plus the generation guard that discards stale responses) is
    // still the right rate-limiting measure.
    static constexpr int kDebounceMs = 15;

    // Google's own web client caps the committed-context it sends at 20
    // Unicode codepoints (confirmed from a HAR capture: the `committed`
    // portion of the `text` query param never exceeds 20 codepoints, sliding
    // forward and dropping older characters as composition continues). We
    // mirror that cap.
    static constexpr int kMaxContextChars = 20;

    static constexpr char kInputCode[] = "yue-hant-t-i0-und";
    static constexpr int kNumCandidates = 13;  // matches Google's own client
};

class GoogleIMEFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override;
};
