#ifdef HAVE_FCITX5

#include "engine.h"
#include "daemon_client.h" // call query_daemon() from daemon_client.cc when needed
#include <thread>
#include <iostream>
#include <memory>
#include <fcitx/candidatelist.h>

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

    // Keep keyEvent lightweight: log invocation and perform a quick
    // synchronous probe (prototype). Replace with event-loop async later.
    try {
        std::cerr << "GoogleIMEEngine: keyEvent invoked\n";
        auto ic = keyEvent.inputContext();
        if (!ic) {
            std::cerr << "GoogleIMEEngine: no input context\n";
            return;
        }

        // TODO: compute actual composition buffer from KeyEvent; use probe
        std::string probe = "test";
        auto candidates = query_daemon(probe, "zh-t-i0-pinyin", 8);
        std::cerr << "GoogleIMEEngine: daemon returned " << candidates.size() << " candidates\n";

#if HAVE_FCITX5_UI
        // Build fcitx5 CandidateList and show in input panel.
        // Use CommonCandidateList (a helper that implements paging and layout)
        auto tmp = std::make_unique<fcitx::CommonCandidateList>();
        for (size_t i = 0; i < candidates.size(); ++i) {
            fcitx::Text t(candidates[i]);
            auto cw = std::make_unique<fcitx::CandidateWord>(t);
            tmp->insert(static_cast<int>(i), std::move(cw));
        }

        auto &panel = ic->inputPanel();
        panel.setClientPreedit(fcitx::Text(probe));
        // CommonCandidateList derives from CandidateList; move-convert unique_ptr.
        panel.setCandidateList(std::move(tmp));
        ic->updatePreedit();
#else
        // UI headers missing: just log the candidate set for debugging.
        for (size_t i = 0; i < candidates.size(); ++i) {
            std::cerr << "  cand[" << i << "]=" << candidates[i] << "\n";
        }
#endif

    } catch (const std::exception &e) {
        std::cerr << "GoogleIMEEngine: exception: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "GoogleIMEEngine: unknown exception\n";
    }
}

// Register addon factory using the macro that takes a single identifier
FCITX_ADDON_FACTORY(GoogleIMEFactory)

#endif // HAVE_FCITX5
