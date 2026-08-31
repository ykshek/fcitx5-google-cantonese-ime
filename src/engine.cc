#include "engine.h"
#include "config.h"       // GoogleIMEConfig (user-configurable itc layout)
#include "daemon_client.h"  // get_candidates() queries Google Input Tools directly

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

#include <fcitx-config/iniparser.h>  // readAsIni / safeSaveAsIni
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/userinterface.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/macros.h>
#include <fcitx/text.h>

// Punctuation mapping for Cantonese (Traditional, Hong Kong).
//
// This table mirrors fcitx5's own `punc.mb.zh_HK` punctuation profile — the
// standard ASCII→CJK punctuation mapping fcitx5 ships for the zh_HK locale —
// so this IME produces the same full-width punctuation a user gets from
// fcitx5's built-in Chinese engines in Hong Kong. Source:
//   https://github.com/fcitx/fcitx5-chinese-addons/blob/master/modules/punctuation/punc.mb.zh_HK
//
// Each entry maps one ASCII key to its CJK equivalent. A few keys — the
// paired quotes and corner brackets — map to TWO forms: an opening form
// (primary) and a closing form (secondary). For those, punctuationFor()
// alternates between the two on successive presses, which is the standard
// CJK behavior: typing `"` twice yields “ then ”, `]` twice yields 「 then 」.
//
// Digits (0-9) and letters are intentionally NOT in this table: digits always
// stay half-width (they are candidate selectors / pass-through), and letters
// go to the romanization preedit. Punctuation not listed here (e.g. `@`, `#`)
// passes through to the client unchanged.
struct PunctMapping {
    char ascii;
    const char* primary;
    const char* secondary;  // nullptr for non-paired punctuation
    uint8_t bit;            // bitmask into state->pairedNextClosing (0 if not paired)
};

inline constexpr PunctMapping kPunctMappings[] = {
    {'!',  "\xEF\xBC\x81", nullptr, 0},              // ！
    {'"',  "\xE2\x80\x9C", "\xE2\x80\x9D", 0x01},  // “ ”
    {'$',  "\xEF\xBC\x84", nullptr, 0},              // ＄
    {'\'', "\xE2\x80\x98", "\xE2\x80\x99", 0x02},  // ‘ ’
    {'(',  "\xEF\xBC\x88", nullptr, 0},              // （
    {')',  "\xEF\xBC\x89", nullptr, 0},              // ）
    {',',  "\xEF\xBC\x8C", nullptr, 0},              // ，
    {'.',  "\xE3\x80\x82", nullptr, 0},              // 。
    {'<',  "\xE3\x80\x8A", nullptr, 0},              // 《
    {'>',  "\xE3\x80\x8B", nullptr, 0},              // 》
    {'?',  "\xEF\xBC\x9F", nullptr, 0},              // ？
    {'[',  "\xC2\xB7",    nullptr, 0},              // ·
    {']',  "\xE3\x80\x8C", "\xE3\x80\x8D", 0x04},  // 「 」
    {'\\', "\xE3\x80\x81", nullptr, 0},             // 、
    {'^',  "\xE2\x80\xA6\xE2\x80\xA6", nullptr, 0}, // ……
    {'_',  "\xE2\x80\x94\xE2\x80\x94", nullptr, 0}, // ——
    {'~',  "\xEF\xBD\x9E", nullptr, 0},              // ～
    {':',  "\xEF\xBC\x9A", nullptr, 0},              // ：
    {';',  "\xEF\xBC\x9B", nullptr, 0},              // ；
};

static const PunctMapping* findPunct(char c) {
    for (const auto& m : kPunctMappings) {
        if (m.ascii == c) return &m;
    }
    return nullptr;
}

// Returns the full-width CJK string to emit for `c`. For paired punctuation
// (quotes / corner brackets) it alternates between the opening and closing
// form across successive presses, toggling per-IC state — the standard CJK
// behavior. For non-punctuation it returns the char unchanged (half-width).
static std::string punctuationFor(char c, GoogleIMEState* state) {
    const PunctMapping* m = findPunct(c);
    if (!m) return std::string(1, c);
    const char* out = m->primary;
    if (m->secondary && state) {
        if (state->pairedNextClosing & m->bit) {
            out = m->secondary;            // emit closing, next is opening
            state->pairedNextClosing &= ~m->bit;
        } else {
            out = m->primary;              // emit opening, next is closing
            state->pairedNextClosing |= m->bit;
        }
    }
    return out;
}

void GoogleIMEEngine::MyCandidateWord::select(
    fcitx::InputContext *ic) const {
    // Mouse-click / keyboard selection both funnel through this one path,
    // which carries matchedLength so partial (word-by-word) selection works.
    //
    // Because we now keep old candidates visible as a preview while a newer
    // lookup is in flight, the candidate object the user clicked may belong to
    // an earlier keystroke and have a matchedLength that's wrong for the
    // current (changed) preedit. Guard against BOTH staleness cases:
    //   (a) this word's render-time probe no longer matches the state's
    //       candidatesProbe — a newer list has since replaced this one; and
    //   (b) the current visible list itself is stale (candidatesFresh() false)
    //       — the list is still the old one because the new lookup hasn't
    //       returned yet, but the preedit has changed since it was computed.
    // In either case, do NOT commit; fire a fresh lookup for the current
    // preedit so the user gets up-to-date candidates to choose from.
    auto *state = ic ? ic->propertyFor(&engine_->stateFactory_) : nullptr;
    if (!state || probe_ != state->candidatesProbe ||
        !engine_->candidatesFresh(state)) {
        if (state && !state->preedit.empty()) {
            if (state->debounceTimer) state->debounceTimer.reset();
            engine_->lookupNow(ic, state);
        }
        return;
    }
    engine_->selectCandidate(ic, candText_, matchedLength_);
}

std::string GoogleIMEEngine::buildProbe(const GoogleIMEState *state) const {
    // Google's own web client sends `|<committed>,<preedit>` so prior composed
    // text acts as prediction context without being re-converted. With no
    // context yet we send the preedit alone.
    if (state->committed.empty()) return state->preedit;
    return "|" + state->committed + "," + state->preedit;
}

void GoogleIMEEngine::trimCommitted(GoogleIMEState *state) const {
    auto &str = state->committed;
    if (str.empty()) return;
    int cp = 0;
    size_t i = str.size();
    while (i > 0) {
        --i;
        // back up over any UTF-8 continuation bytes to the lead byte of a codepoint
        while (i > 0 && (str[i] & 0xC0) == 0x80) --i;
        ++cp;
        if (cp == kMaxContextChars) {
            // keep from this lead byte onward
            if (i > 0) str.erase(0, i);
            return;
        }
    }
}

bool GoogleIMEEngine::candidatesFresh(const GoogleIMEState *state) const {
    // A list that was never populated is not "fresh" in the sense selection
    // paths care about (there is nothing to select from), so treat empty lists
    // as unselectable rather than accidentally-usable.
    if (state->candidates.empty()) return false;
    // Fresh requires BOTH that the probe (committed context + preedit) still
    // matches the current state, AND that the input layout has not changed
    // since the list was computed. The layout check makes any candidates still
    // visible from a previous layout unselectable after the user switches
    // layouts, forcing a fresh lookup with the new layout instead of letting
    // Space / a digit commit an old-layout candidate.
    return state->candidatesProbe == buildProbe(state) &&
           state->candidatesConfigGeneration == configGeneration_.load();
}

void GoogleIMEEngine::selectCandidate(fcitx::InputContext *ic,
                                      std::string text,
                                      int matchedLength) {
    auto *state = ic ? ic->propertyFor(&stateFactory_) : nullptr;
    if (state) {
        // Invalidate any in-flight async results so a stale candidate list
        // cannot reappear after the user already selected a word.
        ++state->generation;
        state->pendingPunctuation = 0;
        if (state->debounceTimer) {
            state->debounceTimer.reset();
        }
        // Extend the committed context with this selection (so subsequent
        // words get predicted with it as context, matching Google's behavior)
        // and remove the portion of the preedit that this candidate consumed.
        state->committed += text;
        trimCommitted(state);
        int ml = matchedLength;
        if (ml < 0) ml = 0;
        if (ml > static_cast<int>(state->preedit.size()))
            ml = static_cast<int>(state->preedit.size());
        if (ml > 0) state->preedit.erase(0, ml);
        state->candidates.clear();
        state->candidatesProbe.clear();
        state->page = 0;
        state->cursor = 0;
    }
    if (ic) {
        ic->commitString(text);
        if (!state || state->preedit.empty()) {
            // Composition consumed entirely (or no state): hide the panel but
            // KEEP committed context so the next word can use it as context.
            ic->inputPanel().reset();
            ic->updatePreedit();
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        } else {
            // Preedit remains (partial selection): re-render the reduced
            // preedit immediately and re-query the leftover for fresh
            // candidates.
            renderPanel(ic);
            scheduleRequest(ic, state);
        }
    }
}

void GoogleIMEEngine::renderPanel(fcitx::InputContext *ic) {
    if (!ic) return;
    auto *state = ic->propertyFor(&stateFactory_);
    if (!state) return;

    auto &panel = ic->inputPanel();
    panel.reset();

    if (state->preedit.empty()) {
        // Nothing to compose: show a blank panel.
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    // Preedit is always rebuilt directly from `preedit` — never from the
    // displayed text or any derived field — so editing the buffer (including
    // backspace) is always reflected immediately and consistently.
    //
    // NOTE: we show ONLY `preedit` here, never `committed + preedit`. The
    // committed context has already been committed to the client via
    // commitString(), so including it in the preedit would duplicate it in
    // clients that render a client preedit (e.g. 你你hou). `committed` stays
    // internal — it's used solely to build Google's prediction-context probe.
    const fcitx::Text preedit(state->preedit);

    // Candidate window: render a kPageSize-wide window into the full candidate
    // vector starting at the current page.
    const int total = static_cast<int>(state->candidates.size());
    const int start = state->page * kPageSize;
    const int end = std::min(start + kPageSize, total);

    auto cl = std::make_unique<GoogleIMECandidateList>();
    if (end > start) {
        // Clamp the cursor to the visible page so the highlight always lands
        // on a real candidate.
        if (state->cursor < 0) state->cursor = 0;
        if (state->cursor >= end - start) state->cursor = end - start - 1;
        for (int i = start; i < end; ++i) {
            const auto &c = state->candidates[i];
            fcitx::Text t(c.text);
            cl->append(std::make_unique<MyCandidateWord>(
                           std::move(t), this, c.text, c.matchedLength,
                           state->candidatesProbe, c.annotation),
                       state->cursor);
        }
    }
    // When no candidates are available yet (request still in flight, or Google
    // returned nothing), we simply do not set a candidate list: the panel
    // shows the preedit alone until the debounced request returns.

    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit)) {
        panel.setClientPreedit(preedit);
    } else {
        panel.setPreedit(preedit);
    }
    if (cl->size() > 0) {
        panel.setCandidateList(std::move(cl));
    }

    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void GoogleIMEEngine::applyCandidates(fcitx::InputContext *ic,
                                      std::vector<GoogleCandidate> candidates,
                                      std::string probe, uint64_t gen,
                                      uint64_t serial, int num, int targetPage,
                                      uint64_t configGen) {
    if (!ic) return;
    auto *state = ic->propertyFor(&stateFactory_);
    if (!state) return;

    // Drop stale results: a newer edit happened (generation changed), the probe
    // (committed context + preedit) changed, a newer request superseded this
    // one (serial changed — e.g. a "fetch more" with a larger num was issued
    // for the same unchanged preedit), OR the user changed the input layout
    // since this request was dispatched (configGen changed). That last guard is
    // what stops a request issued for the OLD layout (e.g. Cantonese) from
    // overwriting the panel / being committed after the user has switched to a
    // new layout (e.g. pinyin). Expected and frequent, so not logged.
    if (gen != state->generation || probe != buildProbe(state) ||
        serial != state->requestSerial ||
        configGen != configGeneration_.load()) {
        return;
    }

    state->candidates = std::move(candidates);
    state->candidatesProbe = probe;
    state->candidatesConfigGeneration = configGen;
    state->candidatesNum = num;
    state->moreFetchPage = -1;  // this in-flight fetch (if any) is now resolved
    // Land on the page this result was meant to populate: 0 for a normal
    // keystroke lookup (preedit changed -> restart at first page), or the page
    // the user paged into for a "fetch more". Clamp to the last valid page in
    // case Google returned fewer candidates than requested (partial set -> no
    // more pages beyond what we got).
    const int total = static_cast<int>(state->candidates.size());
    const int maxPage = total > 0 ? (total - 1) / kPageSize : 0;
    int tp = targetPage;
    if (tp < 0) tp = 0;
    if (tp > maxPage) tp = maxPage;
    state->page = tp;
    state->cursor = 0;

    // Deferred punctuation: the user typed punctuation while no candidates
    // were available yet; we deferred resolving the composition until the
    // in-flight response arrived. Resolve it now and then emit the
    // punctuation. This prevents committing raw romanization just because the
    // network was a beat slow (e.g. "nei," -> "你，" rather than "nei，").
    if (state->pendingPunctuation) {
        char pw = state->pendingPunctuation;
        std::string out = punctuationFor(pw, state);
        state->pendingPunctuation = 0;
        std::string committedText;
        if (!state->candidates.empty()) {
            // Prefer the first full-consumption candidate (matchedLength covers
            // the whole preedit); fall back to the top-ranked candidate.
            size_t pick = 0;
            const int full = static_cast<int>(state->preedit.size());
            for (size_t i = 0; i < state->candidates.size(); ++i) {
                if (state->candidates[i].matchedLength == full) {
                    pick = i;
                    break;
                }
            }
            // Preserve all typed input (same rationale as commitPunctuation).
            const auto &picked = state->candidates[pick];
            std::string remainder = state->preedit;
            int ml = picked.matchedLength;
            if (ml > static_cast<int>(remainder.size()))
                ml = static_cast<int>(remainder.size());
            if (ml > 0) remainder.erase(0, ml);
            committedText = picked.text;
            // Append the unconsumed remainder to committedText so it's
            // committed in one shot below.
            committedText += remainder;
        } else {
            // No candidates came back: commit the romanization as-is.
            committedText = state->preedit;
        }
        // End the sentence: commit the chosen text + the full-width
        // punctuation, then clear composition state (including committed
        // context — there's no longer a continuous sentence).
        ++state->generation;
        if (state->debounceTimer) state->debounceTimer.reset();
        ic->commitString(committedText);
        ic->commitString(out);
        state->preedit.clear();
        state->committed.clear();
        state->candidates.clear();
        state->candidatesProbe.clear();
        state->page = 0;
        state->cursor = 0;
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    renderPanel(ic);
}

void GoogleIMEEngine::scheduleRequest(fcitx::InputContext *ic,
                                      GoogleIMEState *state) {
    if (!instance_) return;
    auto &loop = instance_->eventLoop();

    // Cancel any previously scheduled request by replacing the timer.
    // addTimeEvent is one-shot (callback returns false); creating a new timer
    // and dropping the old unique_ptr disarms the previous one.
    const auto now =
        fcitx::now(CLOCK_MONOTONIC);
    const uint64_t fire = now + kDebounceMs * 1000ULL;

    state->debounceTimer = loop.addTimeEvent(
        CLOCK_MONOTONIC, fire, 1000,
        [this, ic, gen = state->generation](fcitx::EventSourceTime *,
                                           uint64_t) -> bool {
            auto *st = ic ? ic->propertyFor(&stateFactory_) : nullptr;
            if (!st) return false;
            // Another keystroke may have arrived since this timer was armed;
            // only fire if we are still the latest request.
            if (st->generation != gen) return false;
            lookupNow(ic, st);
            return false;  // one-shot
        });
}

void GoogleIMEEngine::dispatchRequest(fcitx::InputContext *ic,
                                     GoogleIMEState *state,
                                     int num, int targetPage, bool isMore) {
    if (!instance_) return;

    // Bump the per-request serial on every dispatch. This is stricter than the
    // generation counter: two requests can share a generation + probe yet have
    // different `num` (an initial num=9 lookup racing a num=18 "fetch more"
    // for the same unchanged preedit). The serial makes the LATER request win —
    // an older num=9 response can never overwrite a newer num=18 list.
    ++state->requestSerial;
    // Record which page an in-flight "fetch more" is meant to populate, so a
    // second page-down while the first is still pending doesn't queue a
    // duplicate request. A normal (non-paging) lookup clears this.
    state->moreFetchPage = isMore ? targetPage : -1;

    // Shared, heap-allocated result struct. The worker thread writes the
    // Google response into it, then calls send() on the async source (which
    // is thread-safe); the main-thread callback reads it back. The shared_ptr
    // keeps the data alive across the thread handoff.
    struct AsyncResult {
        std::vector<GoogleCandidate> candidates;
        std::string probe;
        uint64_t gen = 0;
        uint64_t serial = 0;
        int num = 0;
        int targetPage = 0;
        // Google Input Tools `itc` code snapshotted on the main thread at
        // dispatch time (effectiveInputCode()). The worker thread reads only
        // this copy, never the live Configuration object, so no mutex is needed
        // for the config read itself.
        std::string inputCode;
        // configGeneration_ snapshot at dispatch time. If the user changes the
        // input layout while this request is in flight, this no longer matches
        // configGeneration_ when the response arrives and applyCandidates drops
        // it — preventing an old-layout candidate list from overwriting the
        // panel or being committed after a layout switch.
        uint64_t configGen = 0;
        fcitx::TrackableObjectReference<fcitx::InputContext> icRef;
    };
    auto result = std::make_shared<AsyncResult>();
    // `buildProbe` snapshots the committed context + preedit at dispatch time;
    // it's what applyCandidates compares against to detect stale results.
    result->probe = buildProbe(state);
    result->gen = state->generation;
    result->serial = state->requestSerial;
    result->num = num;
    result->targetPage = targetPage;
    // Snapshot the configured layout NOW, on the main thread: the worker must
    // not touch config_ (not mutex-guarded) and must use a single consistent
    // itc for its request even if the user changes the layout mid-flight.
    result->inputCode = effectiveInputCode();
    result->configGen = configGeneration_.load();
    result->icRef = ic->watch();  // weak ref: safe even if the IC is destroyed

    auto *engine = this;
    auto &loop = instance_->eventLoop();

    // Create the async event source on the main thread. Event sources must be
    // created/destroyed on the event-loop thread; only send() is safe to call
    // from another thread. The previous implementation created a defer event
    // from inside the worker thread, which is NOT thread-safe — that was the
    // root cause of the "candidate list not updated until I press Down"
    // symptom (the deferred UI refresh was sometimes never scheduled).
    auto source = loop.addAsyncEvent(
        [engine, result](fcitx::EventSource *src) -> bool {
            auto *ic = result->icRef.get();
            if (ic) {
                try {
                    engine->applyCandidates(ic, result->candidates,
                                           result->probe, result->gen,
                                           result->serial, result->num,
                                           result->targetPage,
                                           result->configGen);
                } catch (const std::exception &e) {
                    std::cerr << "GoogleIMEEngine(applyCandidates): exception: "
                              << e.what() << "\n";
                } catch (...) {
                    std::cerr << "GoogleIMEEngine(applyCandidates): unknown exception\n";
                }
            }
            // Self-remove from pendingEvents_ now that it has fired.
            std::lock_guard<std::mutex> lk(engine->pendingEventMutex_);
            for (auto it = engine->pendingEvents_.begin();
                 it != engine->pendingEvents_.end(); ++it) {
                if (it->get() == src) {
                    engine->pendingEvents_.erase(it);
                    break;
                }
            }
            return false;  // one-shot
        });

    fcitx::EventSourceAsync *raw = source.get();
    if (raw) {
        std::lock_guard<std::mutex> lk(pendingEventMutex_);
        pendingEvents_.push_back(std::move(source));
    }

    // The HTTP GET blocks for ~100ms; do it on a worker thread so the main
    // event loop (and all of fcitx5's input handling) is never blocked.
    std::thread worker([engine, result, raw, num]() {
        try {
            result->candidates =
                get_candidates(result->probe, result->inputCode, num);
        } catch (const std::exception &e) {
            std::cerr << "GoogleIMEEngine(worker): get_candidates exception: "
                      << e.what() << "\n";
        } catch (...) {
            std::cerr << "GoogleIMEEngine(worker): get_candidates unknown exception\n";
        }
        // Wake the main loop to apply the result. send() is the thread-safe
        // trigger; the callback above runs on the main thread.
        if (raw) raw->send();
    });
    worker.detach();
}

void GoogleIMEEngine::keyEvent(const fcitx::InputMethodEntry &entry,
                              fcitx::KeyEvent &keyEvent) {
    (void)entry;

    auto *ic = keyEvent.inputContext();
    if (!ic) return;
    auto *state = ic->propertyFor(&stateFactory_);
    if (!state) return;

    // Ignore key releases — only act on key presses.
    if (keyEvent.isRelease()) return;

    const fcitx::Key &key = keyEvent.key();
    const fcitx::KeySym sym = key.sym();

    // --- Resolve any pending deferred punctuation FIRST, before any branch
    // below mutates state->preedit ---
    // If a punctuation commit is waiting on an in-flight Google lookup
    // (commitPunctuation's deferred path) and the user types/edits before
    // that lookup returns, we must not let the async response apply the
    // punctuation to preedit text the user has since changed (e.g. typing
    // "nei" -> "," pending -> then typing "h" before the response arrives
    // must NOT let a later response attach "，" after "neih"). Resolve
    // synchronously right now using the raw preedit as it stands (no
    // candidates are available yet in this race, by definition), then
    // continue processing the current key against the now-clean state. The
    // generation bump also makes the original async response a no-op when
    // it eventually arrives.
    if (state->pendingPunctuation != 0) {
        const char pending = state->pendingPunctuation;
        const std::string raw = state->preedit;
        ++state->generation;
        if (state->debounceTimer) state->debounceTimer.reset();
        state->preedit.clear();
        state->committed.clear();
        state->candidates.clear();
        state->candidatesProbe.clear();
        state->page = 0;
        state->cursor = 0;
        state->pendingPunctuation = 0;
        if (!raw.empty()) ic->commitString(raw);
        ic->commitString(punctuationFor(pending, state));
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        // Fall through: the current key is now handled against fresh state.
    }

    // --- Modifier-guard: pass Ctrl/Alt/Super/Hyper combos straight through to
    // the client. Without this, Ctrl+A (whose X11 keysym is the uppercase 'A')
    // would fall into the printable-input branch below and push a capital 'A'
    // into the romanization preedit — which was the root cause of "ctrl and
    // alt keys write capital letters". We must NOT consume these events: the
    // client almost always owns them (Ctrl+S to save, Ctrl+C to copy, ...),
    // so returning without filter() lets them reach the client untouched.
    const fcitx::KeyStates mods = key.states();
    const fcitx::KeyStates kModifierMask{
        fcitx::KeyState::Ctrl, fcitx::KeyState::Alt,
        fcitx::KeyState::Super, fcitx::KeyState::Hyper};
    if (mods.testAny(kModifierMask)) {
        return;
    }
    // --- Candidate navigation (only meaningful when there are candidates) ---
    if (!state->candidates.empty() && state->cursor >= 0) {
        const int perPage = kPageSize;
        const int totalPages =
            (static_cast<int>(state->candidates.size()) + perPage - 1) /
            perPage;

        switch (sym) {
            case FcitxKey_Up: {
                // Move cursor up within the page; wrap to previous page when
                // at the top of the current page.
                if (state->cursor > 0) {
                    state->cursor--;
                } else if (state->page > 0) {
                    state->page--;
                    state->cursor = perPage - 1;
                }
                renderPanel(ic);
                keyEvent.filterAndAccept();
                return;
            }
            case FcitxKey_Down: {
                const int pageStart = state->page * perPage;
                const int pageEnd =
                    std::min(pageStart + perPage,
                             static_cast<int>(state->candidates.size()));
                const int pageLen = pageEnd - pageStart;
                if (state->cursor < pageLen - 1) {
                    state->cursor++;
                } else if (state->page < totalPages - 1) {
                    state->page++;
                    state->cursor = 0;
                } else if (candidatesFresh(state) &&
                           state->moreFetchPage < 0 &&
                           static_cast<int>(state->candidates.size()) ==
                               state->candidatesNum) {
                    // At the bottom of the last page, and the last fetch
                    // returned a full set (Google might have more): re-request
                    // the same preedit with a larger `num` so the next page is
                    // populated. This mirrors Google's own web client, which
                    // re-requests the same text with a larger `num` when the
                    // user pages for more candidates. The result lands on
                    // `targetPage` (page+1) when it arrives.
                    const int targetPage = state->page + 1;
                    dispatchRequest(ic, state, numForPage(targetPage),
                                   targetPage, true);
                }
                renderPanel(ic);
                keyEvent.filterAndAccept();
                return;
            }
            case FcitxKey_Page_Up:
            case FcitxKey_Page_Down: {
                if (sym == FcitxKey_Page_Up && state->page > 0) {
                    state->page--;
                } else if (sym == FcitxKey_Page_Down &&
                           state->page < totalPages - 1) {
                    state->page++;
                } else if (sym == FcitxKey_Page_Down && candidatesFresh(state) &&
                           state->moreFetchPage < 0 &&
                           static_cast<int>(state->candidates.size()) ==
                               state->candidatesNum) {
                    // Paging past the last available candidate: fetch more for
                    // the next page (see FcitxKey_Down above for rationale).
                    const int targetPage = state->page + 1;
                    dispatchRequest(ic, state, numForPage(targetPage),
                                   targetPage, true);
                }
                const int pageStart = state->page * perPage;
                const int pageEnd =
                    std::min(pageStart + perPage,
                             static_cast<int>(state->candidates.size()));
                state->cursor = std::min(state->cursor, pageEnd - pageStart - 1);
                if (state->cursor < 0) state->cursor = 0;
                renderPanel(ic);
                keyEvent.filterAndAccept();
                return;
            }
            default:
                break;
        }
    }

    // --- Digit selection (1-9, 0): works whenever candidates exist AND are
    // fresh (i.e. they were computed for the current preedit). If the list is
    // stale (a newer keystroke changed the preedit but the new lookup hasn't
    // returned yet), we cannot safely commit from it — the candidate's
    // matchedLength is relative to the OLD, shorter preedit and would corrupt
    // the current one. So if stale, cancel the debounce and force a fresh
    // lookup immediately instead of committing a possibly-wrong candidate. ---
    if (!state->candidates.empty()) {
        const int idx = key.digitSelection();
        if (idx >= 0) {
            const int perPage = kPageSize;
            const int pageStart = state->page * perPage;
            const int global = pageStart + idx;
            if (candidatesFresh(state) &&
                global < static_cast<int>(state->candidates.size())) {
                const auto &c = state->candidates[global];
                selectCandidate(ic, c.text, c.matchedLength);
            } else {
                // Stale list (or out of range): treat like Space-with-no-
                // candidates — fire the lookup for the current preedit now.
                if (state->debounceTimer) {
                    state->debounceTimer.reset();
                }
                lookupNow(ic, state);
            }
            keyEvent.filterAndAccept();
            return;
        }
    }

    // --- Space: commit the highlighted candidate, or - if no (fresh)
    // candidates have arrived yet - force the lookup now. This keeps Space from
    // leaking a literal space into the client while the IME is still
    // composing. ---
    if (sym == FcitxKey_space && !state->preedit.empty()) {
        const int perPage = kPageSize;
        const int global = state->page * perPage + state->cursor;
        if (candidatesFresh(state) && global >= 0 &&
            global < static_cast<int>(state->candidates.size())) {
            const auto &c = state->candidates[global];
            selectCandidate(ic, c.text, c.matchedLength);
        } else {
            // No fresh candidates (request in flight / not fired, or the visible
            // list is stale): cancel the debounce and fire the lookup
            // immediately. The generation guard discards any duplicate stale
            // response.
            if (state->debounceTimer) {
                state->debounceTimer.reset();
            }
            lookupNow(ic, state);
        }
        keyEvent.filterAndAccept();
        return;
    }

    // --- Printable ASCII: split into letters (romanization), digits (tone
    // numbers / pass-through), and symbols (everything else). Symbols are
    // treated like punctuation: they flush any active composition and emit
    // directly, NEVER entering the romanization preedit — which is what stops
    // "," / "." etc. from getting stuck in the buffer until the user presses
    // Enter. Mapped symbols become full-width CJK punctuation; symbols not in
    // the zh_HK table (e.g. `@`, `#`, `=`, `+`) emit as their half-width ASCII
    // self. This mirrors fcitx5's own zh_HK punctuation handling and avoids the
    // old regression where an unmapped symbol like `/` would get stuck in the
    // preedit. ---
    char c = static_cast<char>(sym);
    // Jyutping romanization is all lowercase. Normalize any uppercase ASCII
    // letter (whether from Shift or CapsLock) down to lowercase before it
    // enters the preedit, so neither can poison the lookup Google receives.
    if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
    const bool asciiPrintable = sym > 0x20 && sym <= 0x7e;
    const bool isDigit = (sym >= '0' && sym <= '9');
    const bool isLetter =
        (sym >= 'A' && sym <= 'Z') || (sym >= 'a' && sym <= 'z');
    const bool composing = !state->preedit.empty() || !state->candidates.empty() || state->pendingPunctuation;

    if (asciiPrintable && !isDigit && !isLetter) {
        // Symbol: flush the composition (if any) and emit the symbol directly,
        // full-width if the zh_HK table maps it, half-width otherwise.
        commitPunctuation(ic, state, static_cast<char>(sym));
        keyEvent.filterAndAccept();
        return;
    }

    // Digits ('0'-'9') and tone-number digits are part of jyutping romanization
    // and must reach the preedit while composing (e.g. "si3" with the tone
    // number). When there's no active composition, digits should NOT be
    // buffered or sent to Google — they should pass through to the client as
    // ordinary ASCII digits (half-width), exactly as if the IME were off.
    if (isDigit && !composing) {
        // Pass digits straight through, half-width, no buffering. The
        // client now owns this digit directly, so our internal committed
        // prediction context (which only reflects what THIS IME committed) is
        // stale — clear it to avoid feeding Google a wrong context later.
        if (!state->committed.empty()) {
            state->committed.clear();
            state->generation++;
        }
        return;
    }
    if (asciiPrintable) {
        // Letters (and tone digits while composing) go to the preedit.
        state->preedit.push_back(c);
        ++state->generation;
        // Intentionally do NOT clear state->candidates here: we want the old
        // (now stale) list to stay visible as a best-effort preview while the
        // new lookup is in flight, rather than making the panel flicker to empty
        // on every keystroke. The generation guard in applyCandidates discards
        // stale async results, and candidatesFresh() guards the selection paths
        // from committing a candidate that's wrong for the current preedit.
        // The panel re-renders immediately (so the new preedit character shows
        // up at once); the list visually updates when the fresh response lands.
        renderPanel(ic);
        scheduleRequest(ic, state);
        keyEvent.filterAndAccept();
        return;
    }

    // --- Backspace: edit the preedit directly ---
    // Only intercept Backspace while we are actively composing; otherwise let
    // the client handle it (e.g. delete selected text in the editor).
    if (sym == FcitxKey_BackSpace && !state->preedit.empty()) {
        state->preedit.pop_back();
        ++state->generation;
        // Same rationale as the printable-input branch above: keep showing the
        // old candidate list while the new lookup is in flight.
        if (state->preedit.empty()) {
            // Preedit emptied: hide the panel (but keep committed context —
            // the user might resume typing the next word of the sentence)
            // and cancel any pending request.
            if (state->debounceTimer) {
                state->debounceTimer.reset();
            }
            state->candidates.clear();
            state->candidatesProbe.clear();
            state->page = 0;
            state->cursor = 0;
            renderPanel(ic);
        } else {
            renderPanel(ic);
            scheduleRequest(ic, state);
        }
        keyEvent.filterAndAccept();
        return;
    }

    // --- Enter: commit the raw preedit (latin escape hatch) and reset ---
    // Only meaningful while composing; otherwise let the client insert a newline.
    if ((sym == FcitxKey_Return || sym == FcitxKey_KP_Enter) && !state->preedit.empty()) {
        ic->commitString(state->preedit);
        ++state->generation;
        state->preedit.clear();
        state->committed.clear();
        state->candidates.clear();
        state->candidatesProbe.clear();
        state->page = 0;
        state->cursor = 0;
        state->pendingPunctuation = 0;
        if (state->debounceTimer) {
            state->debounceTimer.reset();
        }
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        keyEvent.filterAndAccept();
        return;
    }

    // Escape: reset composition (only while composing).
    if (sym == FcitxKey_Escape && (!state->preedit.empty() || !state->candidates.empty())) {
        ++state->generation;
        state->preedit.clear();
        state->committed.clear();
        state->candidates.clear();
        state->candidatesProbe.clear();
        state->page = 0;
        state->cursor = 0;
        state->pendingPunctuation = 0;
        if (state->debounceTimer) {
            state->debounceTimer.reset();
        }
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        keyEvent.filterAndAccept();
        return;
    }

    // --- Other client-editing/navigation keys: pass through to the client ---
    // Only clear stale committed-context here when we are NOT mid-composition
    // (preedit empty implies candidates empty too, by construction elsewhere).
    // If preedit is still non-empty (e.g. an unhandled arrow key pressed while
    // composing, before candidates have arrived), leave committed alone —
    // the user is still composing, not editing client text directly.
    // Otherwise: the user is editing the client text directly (Delete,
    // Return, arrows, Home/End, etc.) with no active composition. Our
    // internal `committed` prediction context would then be stale — it
    // reflects only what this IME committed, not the client's actual current
    // text — so clear it to avoid feeding Google a wrong prediction context
    // for the next composition.
    if (state->preedit.empty() && !state->committed.empty()) {
        state->committed.clear();
        state->generation++;
    }
}

void GoogleIMEEngine::reset(const fcitx::InputMethodEntry &entry,
                            fcitx::InputContextEvent &event) {
    (void)entry;
    auto *ic = event.inputContext();
    if (!ic) return;
    auto *state = ic->propertyFor(&stateFactory_);
    if (!state) return;

    ++state->generation;
    state->preedit.clear();
    state->committed.clear();
    state->candidates.clear();
    state->candidatesProbe.clear();
    state->candidatesNum = 0;
    state->moreFetchPage = -1;
    state->page = 0;
    state->cursor = 0;
    state->pendingPunctuation = 0;
    if (state->debounceTimer) {
        state->debounceTimer.reset();
    }

    ic->inputPanel().reset();
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);

    // NOTE: we intentionally do NOT clear pendingEvents_ here. A worker thread
    // may still hold a raw EventSourceAsync* and be about to call send();
    // destroying the source out from under it would be a use-after-free.
    // Instead, bumping the generation above makes any in-flight callback a
    // no-op (generation mismatch in applyCandidates), and the source then
    // self-removes from pendingEvents_ when its callback fires.
}

void GoogleIMEEngine::commitPunctuation(fcitx::InputContext *ic,
                                        GoogleIMEState *state,
                                        char halfWidth) {
    // NOTE: punctuationFor() toggles the per-IC paired-quote state as a side
    // effect of producing the opening/closing form. It must therefore be called
    // EXACTLY ONCE, at the moment the punctuation is actually emitted — never
    // speculatively up front. The deferred path below does NOT emit here (the
    // async response in applyCandidates emits later), so it must not toggle
    // either; otherwise the toggle would run twice (once here, discarded, and
    // once in applyCandidates) and the emitted quote would flip to the wrong
    // form. Each emit path below computes `out` for itself at emit time.

    if (state->preedit.empty()) {
        // No composition active: emit the punctuation directly. Punctuation
        // conventionally ends a sentence, so we deliberately clear the
        // committed prediction context here (below) rather than keep it
        // across the punctuation — the next composition should start a fresh
        // context, not continue predicting off the pre-punctuation sentence.
        const std::string out = punctuationFor(halfWidth, state);
        ic->commitString(out);
        ++state->generation;
        state->committed.clear();
        state->candidates.clear();
        state->candidatesProbe.clear();
        state->page = 0;
        state->cursor = 0;
        state->pendingPunctuation = 0;
        if (state->debounceTimer) {
            state->debounceTimer.reset();
        }
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    // Composition is active: resolve it before emitting punctuation.
    if (candidatesFresh(state)) {
        // Prefer the first full-consumption candidate (matchedLength covers
        // the whole preedit); fall back to the top-ranked candidate.
        size_t pick = 0;
        const int full = static_cast<int>(state->preedit.size());
        for (size_t i = 0; i < state->candidates.size(); ++i) {
            if (state->candidates[i].matchedLength == full) {
                pick = i;
                break;
            }
        }
        // Preserve all typed input: commit the candidate, then whatever
        // preedit the candidate did NOT consume (the remainder), then the
        // punctuation. E.g. preedit "singsida" picking "城市" (matchedLength
        // 6) yields "城市da，" rather than dropping "da".
        const auto &picked = state->candidates[pick];
        std::string remainder = state->preedit;
        int ml = picked.matchedLength;
        if (ml > static_cast<int>(remainder.size()))
            ml = static_cast<int>(remainder.size());
        if (ml > 0) remainder.erase(0, ml);
        ++state->generation;
        if (state->debounceTimer) state->debounceTimer.reset();
        ic->commitString(picked.text);
        if (!remainder.empty()) ic->commitString(remainder);
        const std::string out = punctuationFor(halfWidth, state);
        ic->commitString(out);
        state->preedit.clear();
        state->committed.clear();
        state->candidates.clear();
        state->candidatesProbe.clear();
        state->page = 0;
        state->cursor = 0;
        state->pendingPunctuation = 0;
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    // Candidates are not available yet (request in flight or not yet fired).
    // Defer: cancel any pending debounce and fire the lookup immediately so
    // the deferred-resolution path in applyCandidates handles it as soon as
    // results arrive. We do NOT call punctuationFor() here: the toggle must
    // happen exactly once, at emit time, which is inside applyCandidates when
    // the deferred punctuation is finally resolved and emitted.
    state->pendingPunctuation = halfWidth;
    if (state->debounceTimer) {
        state->debounceTimer.reset();
    }
    lookupNow(ic, state);
}

GoogleIMEEngine::GoogleIMEEngine(fcitx::Instance *instance) : instance_(instance) {
    if (instance_) {
        instance_->inputContextManager().registerProperty("googleImeState",
                                                          &stateFactory_);
    }
    // Load the user's configured input layout (if any) before the first
    // keystroke so the addon starts in the layout the user picked in the KCM.
    reloadConfig();
}

GoogleIMEEngine::~GoogleIMEEngine() = default;

void GoogleIMEEngine::reloadConfig() {
    // readAsIni resolves "conf/google-ime.conf" against fcitx5's PkgConfig
    // search path (~/.config/fcitx5 then the system data dir), exactly where
    // the KCM / fcitx5-configtool writes the user's edited options.
    readAsIni(config_, configFile);
    // Layout changed: any in-flight request that used the previous layout is
    // now stale and must not be allowed to commit candidates (see configGen
    // guard in applyCandidates).
    ++configGeneration_;
}

void GoogleIMEEngine::setConfig(const fcitx::RawConfig &config) {
    // Called by fcitx5's config D-Bus (the KCM / fcitx5-configtool) when the
    // user edits an option. load(partial=true) keeps un-edited options at their
    // current value, then we persist and bump the layout generation.
    config_.load(config, true);
    safeSaveAsIni(config_, configFile);
    ++configGeneration_;
}

std::string GoogleIMEEngine::effectiveInputCode() const {
    // The Google Input Tools `itc` code to send. Falls back to the
    // compiled-in Cantonese default (kInputCode) when the user left the field
    // blank, so the addon keeps working out of the box with no config file.
    std::string code = config_.inputCode.value();
    if (code.empty()) return kInputCode;
    return code;
}

fcitx::AddonInstance *GoogleIMEFactory::create(fcitx::AddonManager *manager) {
    return new GoogleIMEEngine(manager ? manager->instance() : nullptr);
}

FCITX_ADDON_FACTORY(GoogleIMEFactory)
