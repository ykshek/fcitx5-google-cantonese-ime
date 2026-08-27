#ifdef HAVE_FCITX5

#include "engine.h"
#include "daemon_client.h" // call query_daemon() from daemon_client.cc when needed
#include <thread>
#include <iostream>
#include <memory>
#include <cstdlib>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/addonmanager.h>
#include <fcitx/userinterface.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>

// Probe for common fcitx5 UI header locations and include what exists.
// This keeps the code portable across distros and header layout variations.
#if defined(__has_include)
#  if __has_include(<fcitx/inputpanel.h>) && __has_include(<fcitx/candidate.h>) && __has_include(<fcitx/text.h>)
#    include <fcitx/inputpanel.h>
#    include <fcitx/candidate.h>
#    include <fcitx/text.h>
#    define HAVE_FCITX5_UI 1
#  elif __has_include(<fcitx/inputpanel.h>) && __has_include(<fcitx-utils/candidate-common.h>) && __has_include(<fcitx/text.h>)
#    include <fcitx/inputpanel.h>
#    include <fcitx-utils/candidate-common.h>
#    include <fcitx/text.h>
#    define HAVE_FCITX5_UI 1
#  elif __has_include(<fcitx/inputpanel.h>) && __has_include(<fcitx/module/candidateList.h>) && __has_include(<fcitx/text.h>)
#    include <fcitx/inputpanel.h>
#    include <fcitx/module/candidateList.h>
#    include <fcitx/text.h>
#    define HAVE_FCITX5_UI 1
#  else
#    define HAVE_FCITX5_UI 0
#  endif
#else
#  define HAVE_FCITX5_UI 0
#endif

using namespace fcitx;

void GoogleIMEEngine::keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) {
    (void)entry;

    // Async query: bump sequence, run daemon call in background, then
    // post UI update to the main EventLoop so UI changes happen on the
    // main thread.
    try {
        std::cerr << "GoogleIMEEngine: keyEvent invoked\n";
        auto ic = keyEvent.inputContext();
        if (!ic) {
            std::cerr << "GoogleIMEEngine: no input context\n";
            return;
        }

        // Ignore key releases
        if (keyEvent.isRelease()) {
            return;
        }

        // Try to obtain a printable keysym from the low-level Key object.
        // Many fcitx frontends populate keyEvent.key().sym() with an ASCII
        // code for printable keys.
        int sym = 0;
        try {
            sym = keyEvent.key().sym();
        } catch (...) {
            sym = 0;
        }

        if (sym >= 32 && sym <= 126) {
            buffer_.push_back(static_cast<char>(sym));
            // consume the key so the client doesn't receive the character
            try { keyEvent.filterAndAccept(); } catch (...) {}
        } else {
            // TODO: handle Backspace / Enter / Arrow keys by inspecting sym
            return;
        }

        // Use the current composition buffer as probe
        std::string probe = buffer_;

        uint64_t mySeq = ++querySeq;

        std::thread worker([probe = std::move(probe), mySeq, ic, this]() mutable {
            try {
                auto candidates = query_daemon(probe, "zh-t-i0-pinyin", 8);
                std::cerr << "GoogleIMEEngine(worker): daemon returned " << candidates.size() << " candidates\n";

                // Use the Instance pointer injected by factory. Fallback to
                // InputContext-derived instance only if needed.
                auto inst = instance_;
                if (!inst) {
                    std::cerr << "GoogleIMEEngine: no instance available to post result\n";
                    return;
                }
                auto &loop = inst->eventLoop();

                // post deferred event to the main loop. Keep returned EventSource
                // alive in the engine until the callback runs so it is not destroyed
                // prematurely.
                auto ev = loop.addDeferEvent([ic, candidates = std::move(candidates), probe = std::move(probe), mySeq, this](fcitx::EventSource *src) -> bool {
                    std::cerr << "GoogleIMEEngine(deferred): callback running (mySeq=" << mySeq << ", querySeq=" << querySeq << ")\n";
                    if (querySeq != mySeq) {
                        std::cerr << "GoogleIMEEngine: stale result, drop\n";
                        // clear this EventSource from pendingEvents_ on main thread
                        std::lock_guard<std::mutex> lk(pendingEventMutex_);
                        removePendingEvent(src);
                        return false;
                    }

                    // If composition buffer has been cleared meanwhile, hide panel
                    if (buffer_.empty()) {
                        std::cerr << "GoogleIMEEngine: buffer cleared before result, hiding panel\n";
#if HAVE_FCITX5_UI
                        ic->inputPanel().reset();
#endif
                        ic->updatePreedit();
                        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
                        removePendingEvent(src);
                        return false;
                    }
#if HAVE_FCITX5_UI
                    std::cerr << "GoogleIMEEngine: populating candidate list (count=" << candidates.size() << ")\n";
                    auto tmp = std::make_unique<fcitx::CommonCandidateList>();
                    for (size_t i = 0; i < candidates.size(); ++i) {
                        fcitx::Text t(candidates[i]);
                        std::cerr << "  candidate[" << i << "]=" << t.toString() << "\n";
                        auto cw = std::make_unique<fcitx::CandidateWord>(t);
                        tmp->insert(static_cast<int>(i), std::move(cw));
                    }

                    auto &panel = ic->inputPanel();
                    std::cerr << "GoogleIMEEngine: reset panel and set preedit\n";
                    panel.reset();
                    std::cerr << "GoogleIMEEngine: capabilityFlags=" << ic->capabilityFlags().to_ulong() << "\n";
                    // Set preedit first (match zhuyin/pinyin engines)
                    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit)) {
                        panel.setClientPreedit(fcitx::Text(probe));
                        std::cerr << "GoogleIMEEngine: called setClientPreedit and updatePreedit()\n";
                        ic->updatePreedit();
                    } else {
                        panel.setPreedit(fcitx::Text(probe));
                        std::cerr << "GoogleIMEEngine: called setPreedit\n";
                    }

                    // Then set candidate list and finally update the UI
                    std::cerr << "GoogleIMEEngine: setting candidate list\n";
                    panel.setCandidateList(std::move(tmp));
                    auto cl = panel.candidateList();
                    std::cerr << "GoogleIMEEngine: panel.empty()=" << (panel.empty() ? "true" : "false") << ", candidateList=" << (cl ? "present" : "null") << "\n";
                    if (cl) {
                        if (auto common = dynamic_cast<fcitx::CommonCandidateList*>(cl.get())) {
                            std::cerr << "GoogleIMEEngine: candidateList.size()=" << common->size() << "\n";
                        }
                    }

                    std::cerr << "GoogleIMEEngine: updateUserInterface(InputPanel)\n";
                    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);

                    // Developer test hook: force server-side preedit + candidate UI when
                    // GOOGLE_IME_TEST_FORCE_SERVER_PANEL=1 is set in the environment.
                    // This is strictly diagnostic and disabled by default so no fallback
                    // behavior is introduced in normal runs.
                    const char *force = std::getenv("GOOGLE_IME_TEST_FORCE_SERVER_PANEL");
                    if (force && std::string(force) == "1") {
                        std::cerr << "GoogleIMEEngine: TEST_HOOK forcing server-side preedit + candidate UI\n";
                        // Rebuild a CommonCandidateList from the candidate strings
                        auto testList = std::make_unique<fcitx::CommonCandidateList>();
                        for (size_t i = 0; i < candidates.size(); ++i) {
                            testList->insert(static_cast<int>(i), std::make_unique<fcitx::CandidateWord>(fcitx::Text(candidates[i])));
                        }
                        // Force server-side preedit and candidate list update
                        panel.setPreedit(fcitx::Text(probe));
                        panel.setCandidateList(std::move(testList));
                        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
                    }

                    // When a result arrives, ensure the current buffer still matches the
                    // probe used for the query. If the user continued typing and buffer
                    // differs, don't overwrite the newer composition with stale UI.
                    if (buffer_ != probe) {
                        std::cerr << "GoogleIMEEngine: buffer changed (current='" << buffer_ << "', probe='" << probe << "'), ignoring result\n";
                        removePendingEvent(src);
                        return false;
                    }
#endif
                    for (size_t i = 0; i < candidates.size(); ++i) {
                        std::cerr << "  cand[" << i << "]=" << candidates[i] << "\n";
                    }
                    // clear this EventSource from pendingEvents_ on main thread
                    std::lock_guard<std::mutex> lk(pendingEventMutex_);
                    removePendingEvent(src);
                    return false; // run once
                });

                if (ev) {
                    std::lock_guard<std::mutex> lk(pendingEventMutex_);
                    pendingEvents_.push_back(std::move(ev));
                    std::cerr << "GoogleIMEEngine: deferred event scheduled and stored (pendingEvents=" << pendingEvents_.size() << ")\n";
                } else {
                    std::cerr << "GoogleIMEEngine: addDeferEvent returned null\n";
                }

            } catch (const std::exception &e) {
                std::cerr << "GoogleIMEEngine(worker): exception: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "GoogleIMEEngine(worker): unknown exception\n";
            }
        });
        worker.detach();

    } catch (const std::exception &e) {
        std::cerr << "GoogleIMEEngine: exception scheduling worker: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "GoogleIMEEngine: unknown exception scheduling worker\n";
    }
}

// Helper: removes an EventSource pointer from pendingEvents_.
void GoogleIMEEngine::removePendingEvent(fcitx::EventSource *src) {
    for (auto it = pendingEvents_.begin(); it != pendingEvents_.end(); ++it) {
        if (it->get() == src) {
            pendingEvents_.erase(it);
            return;
        }
    }
}

// Reset engine state when the input context is cleared.
void GoogleIMEEngine::reset(InputContext *ic) {
    std::lock_guard<std::mutex> lk(pendingEventMutex_);
    pendingEvents_.clear();
    buffer_.clear();
    if (ic) {
#if HAVE_FCITX5_UI
        ic->inputPanel().reset();
#endif
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }
}

// Implement factory create now that AddonManager is a complete type here.
AddonInstance *GoogleIMEFactory::create(AddonManager *manager) {
    return new GoogleIMEEngine(manager ? manager->instance() : nullptr);
}

// Register addon factory using the macro that takes a single identifier
FCITX_ADDON_FACTORY(GoogleIMEFactory)

#endif // HAVE_FCITX5
