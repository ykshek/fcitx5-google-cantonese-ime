#ifdef HAVE_FCITX5

#include "engine.h"
#include "daemon_client.h" // call query_daemon() from daemon_client.cc when needed
#include <thread>
#include <iostream>
#include <memory>

// Some systems may not have the full fcitx5 development headers installed
// even when the runtime libraries are present. Use __has_include to detect
// availability of the UI headers and fall back to a no-UI prototype when
// they are missing so the project remains buildable on minimal systems.
#if defined(__has_include)
#  if __has_include(<fcitx/inputpanel.h>) && __has_include(<fcitx/candidate.h>) && __has_include(<fcitx/text.h>)
#    include <fcitx/inputpanel.h>
#    include <fcitx/candidate.h>
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
    // Minimal runtime diagnostics and prototype candidate fetch.
    // Keep the method non-intrusive: log the call and query the local daemon
    // for quick feedback. This is a synchronous prototype and will be
    // replaced by an asynchronous implementation later.
    (void)entry;
    (void)keyEvent;

    try {
        std::cerr << "GoogleIMEEngine: keyEvent invoked (scheduling async query)\n";
        // Synchronously query the daemon and update the input panel UI.
        // This is a prototype: it's simpler and ensures the candidate UI
        // appears while we implement proper async/event-loop integration.
        auto ic = keyEvent.inputContext();
        if (!ic) {
            std::cerr << "GoogleIMEEngine: no input context\n";
        } else {
            try {
                std::string probe = "test"; // TODO: use composition buffer
                auto candidates = query_daemon(probe, "zh-t-i0-pinyin", 8);
                std::cerr << "GoogleIMEEngine: sync daemon returned " << candidates.size() << " candidates\n";

                auto candList = std::make_unique<fcitx::CandidateList>();
                for (size_t i = 0; i < candidates.size(); ++i) {
                    fcitx::Text t(candidates[i]);
                    auto cw = std::make_unique<fcitx::CandidateWord>(t);
                    candList->insert(static_cast<int>(i), std::move(cw));
                }

                auto &panel = ic->inputPanel();
                panel.setClientPreedit(fcitx::Text(probe));
                panel.setCandidateList(std::move(candList));
                ic->updatePreedit();
            } catch (const std::exception &e) {
                std::cerr << "GoogleIMEEngine: exception: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "GoogleIMEEngine: unknown exception scheduling sync\n";
            }
        }
    } catch (const std::exception &e) {
        std::cerr << "GoogleIMEEngine: exception: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "GoogleIMEEngine: unknown exception scheduling async\n";
    }
}

// Register addon factory using the macro that takes a single identifier
FCITX_ADDON_FACTORY(GoogleIMEFactory)

#endif // HAVE_FCITX5
