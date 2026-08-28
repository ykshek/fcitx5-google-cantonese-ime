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

// Number of candidates shown per page in the candidate panel.
inline constexpr int kPageSize = 9;

// Per-InputContext composing state. This is the single source of truth for
// everything the engine renders: the raw query buffer (which feeds the
// preedit), the full candidate set for the current query, the current page /
// cursor within that page, and a monotonically increasing "generation"
// counter used to discard stale async Google responses.
//
// Storing this per input context (via fcitx5's InputContextProperty) fixes a
// latent bug in the previous design, which kept a single `buffer_` member on
// the (singleton) engine shared across every input context — so switching focus
// between two text fields would corrupt the composition state.
struct GoogleIMEState : public fcitx::InputContextProperty {
    // Raw romanized input typed by the user (latin letters). This is the
    // source of truth for the preedit: the preedit is always rebuilt from
    // `buffer` on every render, never derived from the displayed text.
    std::string buffer;

    // Full candidate list returned by Google for the current `buffer`. The
    // panel renders a kPageSize-wide window into this vector starting at
    // `page * kPageSize`.
    std::vector<std::string> candidates;

    // Current candidate page (0-based) and the highlighted candidate index
    // within the current page (-1 = none highlighted).
    int page = 0;
    int cursor = 0;

    // Monotonic counter bumped on every buffer edit. An async Google response
    // carries the generation it was issued for; if it no longer matches the
    // current generation when it arrives, it is discarded. This is what makes
    // fast typing safe: stale in-flight responses can never overwrite newer
    // candidate lists.
    uint64_t generation = 0;

    // A pending debounce timer that will fire the next Google request. Kept
    // as a member so each new keystroke can cancel (replace) the previous
    // timer — this is the rate-limiting / debounce mechanism.
    std::unique_ptr<fcitx::EventSourceTime> debounceTimer;
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
    // the result if it is stale (generation / buffer changed since the
    // request was issued).
    void applyCandidates(fcitx::InputContext *ic,
                         std::vector<std::string> candidates,
                         std::string probe, uint64_t gen);

    // Commit a chosen candidate and reset composition state for `ic`.
    void commitCandidate(fcitx::InputContext *ic, const std::string &text);

private:
    // Candidate word. The display text MUST be handed to the
    // fcitx::CandidateWord base constructor: CandidateWord::text() is
    // non-virtual and returns exactly that stored text, and the UI reads the
    // candidate text through the base class.
    class MyCandidateWord : public fcitx::CandidateWord {
    public:
        MyCandidateWord(fcitx::Text text, GoogleIMEEngine *engine)
            : fcitx::CandidateWord(std::move(text)), engine_(engine) {}
        void select(fcitx::InputContext *ic) const override;

    private:
        GoogleIMEEngine *engine_ = nullptr;
    };

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

    // Debounce window (ms). Mirrors the behavior of the official Google Input
    // Tools "try" page, which coalesces rapid keystrokes client-side rather
    // than firing one network request per key. Google's public endpoint
    // returns no rate-limit headers and does not return 429 for normal use,
    // so this debounce (plus the generation guard that discards stale
    // responses) is the right rate-limiting measure.
    static constexpr int kDebounceMs = 150;
};

class GoogleIMEFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override;
};
