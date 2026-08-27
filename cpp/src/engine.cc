#ifdef HAVE_FCITX5

#include "engine.h"
#include "daemon_client.h" // call query_daemon() from daemon_client.cc when needed
#include <thread>
#include <iostream>
#include <memory>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/addonmanager.h>

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
                auto loop = inst->eventLoop();
                if (!loop) {
                    std::cerr << "GoogleIMEEngine: no event loop\n";
                    return;
                }

                // post deferred event to the main loop
                loop->addDeferEvent([ic, candidates = std::move(candidates), probe = std::move(probe), mySeq, this](fcitx::EventSource *) -> bool {
                    if (querySeq != mySeq) {
                        std::cerr << "GoogleIMEEngine: stale result, drop\n";
                        return false;
                    }
#if HAVE_FCITX5_UI
                    auto tmp = std::make_unique<fcitx::CommonCandidateList>();
                    for (size_t i = 0; i < candidates.size(); ++i) {
                        fcitx::Text t(candidates[i]);
                        auto cw = std::make_unique<fcitx::CandidateWord>(t);
                        tmp->insert(static_cast<int>(i), std::move(cw));
                    }

                    auto &panel = ic->inputPanel();
                    panel.setClientPreedit(fcitx::Text(probe));
                    panel.setCandidateList(std::move(tmp));
                    ic->updatePreedit();
#else
                    for (size_t i = 0; i < candidates.size(); ++i) {
                        std::cerr << "  cand[" << i << "]=" << candidates[i] << "\n";
                    }
#endif
                    return false; // run once
                });

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
