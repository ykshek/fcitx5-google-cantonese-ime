#include "engine.h"
#include "daemon_client.h"  // get_candidates() queries Google Input Tools directly

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

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

// Google Input Tools input code for Cantonese, Traditional.
static constexpr char kInputCode[] = "yue-hant-t-i0-und";
static constexpr int kNumCandidates = 18;  // fetch a page or two at once

void GoogleIMEEngine::MyCandidateWord::select(
    fcitx::InputContext *ic) const {
    engine_->commitCandidate(ic, text().toString());
}

void GoogleIMEEngine::commitCandidate(fcitx::InputContext *ic,
                                     const std::string &text) {
    auto *state = ic ? ic->propertyFor(&stateFactory_) : nullptr;
    if (state) {
        // Invalidate any in-flight async results so a stale candidate list
        // cannot reappear after the user already selected a word.
        ++state->generation;
        state->buffer.clear();
        state->candidates.clear();
        state->page = 0;
        state->cursor = 0;
        if (state->debounceTimer) {
            state->debounceTimer.reset();
        }
    }
    if (ic) {
        ic->commitString(text);
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }
}

void GoogleIMEEngine::renderPanel(fcitx::InputContext *ic) {
    if (!ic) return;
    auto *state = ic->propertyFor(&stateFactory_);
    if (!state) return;

    auto &panel = ic->inputPanel();
    panel.reset();

    if (state->buffer.empty()) {
        // Nothing to compose: show a blank panel.
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    // Preedit is always rebuilt directly from the buffer — never from the
    // displayed text or any derived field — so editing the buffer (including
    // backspace) is always reflected immediately and consistently.
    const fcitx::Text preedit(state->buffer);

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
            fcitx::Text t(state->candidates[i]);
            cl->append(std::make_unique<MyCandidateWord>(std::move(t), this),
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
                                      std::vector<std::string> candidates,
                                      std::string probe, uint64_t gen) {
    if (!ic) return;
    auto *state = ic->propertyFor(&stateFactory_);
    if (!state) return;

    // Drop stale results: either a newer edit happened (generation changed)
    // or the buffer changed since this request was issued.
    if (gen != state->generation || state->buffer != probe) {
        std::cerr << "GoogleIMEEngine: dropping stale result (gen=" << gen
                  << " vs " << state->generation << ")\n";
        return;
    }

    state->candidates = std::move(candidates);
    state->page = 0;
    state->cursor = 0;
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
            dispatchRequest(ic, st);
            return false;  // one-shot
        });
}

void GoogleIMEEngine::dispatchRequest(fcitx::InputContext *ic,
                                     GoogleIMEState *state) {
    if (!instance_) return;

    // Shared, heap-allocated result struct. The worker thread writes the
    // Google response into it, then calls send() on the async source (which
    // is thread-safe); the main-thread callback reads it back. The shared_ptr
    // keeps the data alive across the thread handoff.
    struct AsyncResult {
        std::vector<std::string> candidates;
        std::string probe;
        uint64_t gen = 0;
        fcitx::TrackableObjectReference<fcitx::InputContext> icRef;
    };
    auto result = std::make_shared<AsyncResult>();
    result->probe = state->buffer;
    result->gen = state->generation;
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
                                           result->probe, result->gen);
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
    std::thread worker([engine, result, raw]() {
        try {
            result->candidates =
                get_candidates(result->probe, kInputCode, kNumCandidates);
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

    // --- Digit selection (1-9, 0): works whenever candidates exist ---
    if (!state->candidates.empty()) {
        const int idx = key.digitSelection();
        if (idx >= 0) {
            const int perPage = kPageSize;
            const int pageStart = state->page * perPage;
            const int global = pageStart + idx;
            if (global < static_cast<int>(state->candidates.size())) {
                commitCandidate(ic, state->candidates[global]);
            }
            keyEvent.filterAndAccept();
            return;
        }
    }

    // --- Space: commit the highlighted candidate, or - if no candidates have
    // arrived yet - force the lookup now. This keeps Space from leaking a
    // literal space into the client while the IME is still composing. ---
    if (sym == FcitxKey_space && !state->buffer.empty()) {
        if (!state->candidates.empty()) {
            const int perPage = kPageSize;
            const int global = state->page * perPage + state->cursor;
            if (global >= 0 &&
                global < static_cast<int>(state->candidates.size())) {
                commitCandidate(ic, state->candidates[global]);
            }
        } else {
            // No candidates yet (request in flight / not fired): cancel the
            // debounce and fire the lookup immediately. The generation guard
            // discards any duplicate stale response.
            if (state->debounceTimer) {
                state->debounceTimer.reset();
            }
            dispatchRequest(ic, state);
        }
        keyEvent.filterAndAccept();
        return;
    }

    // --- Printable input: append to buffer ---
    if (sym > 0x20 && sym <= 0x7e) {
        state->buffer.push_back(static_cast<char>(sym));
        ++state->generation;
        // The old candidate set was for a different query: clear it so the
        // panel shows the updated preedit alone until the new debounced
        // request returns.
        state->candidates.clear();
        state->page = 0;
        state->cursor = 0;
        // Show the updated preedit immediately; the candidate list will be
        // refreshed when the debounced request returns.
        renderPanel(ic);
        scheduleRequest(ic, state);
        keyEvent.filterAndAccept();
        return;
    }

    // --- Backspace: edit the buffer (the query) directly ---
    // Only intercept Backspace while we are actively composing; otherwise let
    // the client handle it (e.g. delete selected text in the editor).
    if (sym == FcitxKey_BackSpace && !state->buffer.empty()) {
        state->buffer.pop_back();
        ++state->generation;
        // The candidate set is now stale: clear it so the panel shows the
        // preedit alone until the new request returns.
        state->candidates.clear();
        state->page = 0;
        state->cursor = 0;
        if (state->buffer.empty()) {
            // Buffer emptied: hide the panel and cancel any pending request.
            if (state->debounceTimer) {
                state->debounceTimer.reset();
            }
            renderPanel(ic);
        } else {
            renderPanel(ic);
            scheduleRequest(ic, state);
        }
        keyEvent.filterAndAccept();
        return;
    }

    // --- Enter: commit the raw buffer (latin escape hatch) and reset ---
    // Only meaningful while composing; otherwise let the client insert a newline.
    if ((sym == FcitxKey_Return || sym == FcitxKey_KP_Enter) && !state->buffer.empty()) {
        ic->commitString(state->buffer);
        ++state->generation;
        state->buffer.clear();
        state->candidates.clear();
        state->page = 0;
        state->cursor = 0;
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
    if (sym == FcitxKey_Escape && (!state->buffer.empty() || !state->candidates.empty())) {
        ++state->generation;
        state->buffer.clear();
        state->candidates.clear();
        state->page = 0;
        state->cursor = 0;
        if (state->debounceTimer) {
            state->debounceTimer.reset();
        }
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        keyEvent.filterAndAccept();
        return;
    }

    // Other keys (arrows other than up/down, etc.): let the client handle them.
}

void GoogleIMEEngine::reset(const fcitx::InputMethodEntry &entry,
                            fcitx::InputContextEvent &event) {
    (void)entry;
    auto *ic = event.inputContext();
    if (!ic) return;
    auto *state = ic->propertyFor(&stateFactory_);
    if (!state) return;

    ++state->generation;
    state->buffer.clear();
    state->candidates.clear();
    state->page = 0;
    state->cursor = 0;
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

GoogleIMEEngine::GoogleIMEEngine(fcitx::Instance *instance) : instance_(instance) {
    if (instance_) {
        instance_->inputContextManager().registerProperty("googleImeState",
                                                          &stateFactory_);
    }
}

GoogleIMEEngine::~GoogleIMEEngine() = default;

fcitx::AddonInstance *GoogleIMEFactory::create(fcitx::AddonManager *manager) {
    return new GoogleIMEEngine(manager ? manager->instance() : nullptr);
}

FCITX_ADDON_FACTORY(GoogleIMEFactory)
