#ifdef HAVE_FCITX5

#include "engine.h"
#include "daemon_client.h" // call query_daemon() from daemon_client.cc when needed
#include <thread>
#include <iostream>
#include <memory>
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
        std::cerr << "GoogleIMEEngine: keyEvent invoked (async)\n";
        auto ic = keyEvent.inputContext();
        if (!ic) {
            std::cerr << "GoogleIMEEngine: no input context\n";
            return;
        }

        // TODO: derive probe from actual composition buffer
        std::string probe = "test";

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
                        // clear pendingEvent_ on main thread
                        std::lock_guard<std::mutex> lk(pendingEventMutex_);
                        pendingEvent_.reset();
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
                    std::cerr << "GoogleIMEEngine: setting candidate list\n";
                    panel.setCandidateList(std::move(tmp));
                    auto cl = panel.candidateList();
                    std::cerr << "GoogleIMEEngine: panel.empty()=" << (panel.empty() ? "true" : "false") << ", candidateList=" << (cl ? "present" : "null");
                    if (cl) {
                        if (auto common = dynamic_cast<fcitx::CommonCandidateList*>(cl.get())) {
                            std::cerr << "GoogleIMEEngine: candidateList.size()=" << common->size() << "\n";
                        }
                    }

                    // Use client capability to decide clientPreedit vs preedit like cantonese engine
                    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit)) {
                        panel.setClientPreedit(fcitx::Text(probe));
                    } else {
                        panel.setPreedit(fcitx::Text(probe));
                    }

                    std::cerr << "GoogleIMEEngine: updateUserInterface(InputPanel)\n";
                    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
                    std::cerr << "GoogleIMEEngine: updatePreedit()\n";
                    ICOUT() << "GoogleIMEEngine: ic->hasFocus()=" << (ic->hasFocus() ? "true" : "false") << ", isPreeditEnabled=" << (ic->isPreeditEnabled() ? "true" : "false");
                    ic->updatePreedit();
#endif
                    for (size_t i = 0; i < candidates.size(); ++i) {
                        std::cerr << "  cand[" << i << "]=" << candidates[i] << "\n";
                    }
#endif
                    // clear pendingEvent_ on main thread
                    std::lock_guard<std::mutex> lk(pendingEventMutex_);
                    pendingEvent_.reset();
                    return false; // run once
                });

                if (ev) {
                    std::lock_guard<std::mutex> lk(pendingEventMutex_);
                    pendingEvent_ = std::move(ev);
                    std::cerr << "GoogleIMEEngine: deferred event scheduled and stored\n";
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

// Implement factory create now that AddonManager is a complete type here.
AddonInstance *GoogleIMEFactory::create(AddonManager *manager) {
    return new GoogleIMEEngine(manager ? manager->instance() : nullptr);
}

// Register addon factory using the macro that takes a single identifier
FCITX_ADDON_FACTORY(GoogleIMEFactory)

#endif // HAVE_FCITX5
