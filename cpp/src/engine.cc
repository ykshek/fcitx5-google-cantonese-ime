#ifdef HAVE_FCITX5

#include "engine.h"
#include "fcitx5_google_ime.cc" // reuse query_daemon function
#include <fcitx/inputcontext.h>
#include <fcitx/candidatelist.h>
#include <fcitx/userinterface.h>

using namespace fcitx;

void GoogleIMEEngine::activate(InputContext *ic) {
    // No special activation behaviour for prototype
}

void GoogleIMEEngine::deactivate(InputContext *ic) {
    // Clear any preedit or candidates
    ic->inputPanel().reset();
}

bool GoogleIMEEngine::keyEvent(InputContext *ic, KeyEvent &keyEvent) {
    // Simplified behaviour: when printable keys are typed, update preedit and
    // query candidates. Commit on Enter or selection.

    // Only handle key release events here
    if (keyEvent.isRelease()) return false;

    // If Enter, commit current preedit
    if (keyEvent.key() == Key::Enter) {
        auto &composing = ic->client().inputContextManager().fref();
        // commit first candidate if available
        // Fallback: commit the preedit string (not implemented fully)
        ic->commitString(ic->preedit().toString());
        reset(ic);
        return true;
    }

    // For simplicity, handle printable ascii
    if (keyEvent.key().isPrintable()) {
        // update preedit
        ic->preedit().append(std::string(1, (char)keyEvent.key().sym()));
        ic->updatePreedit();
        std::string composed = ic->preedit().toString();
        auto cands = query_daemon(composed);
        CandidateList list;
        for (size_t i = 0; i < cands.size(); ++i) {
            CandidateWord w;
            w.text = cands[i];
            list.appendCandidateItem(i + 1, w.text);
        }
        ic->userInterface().showCandidateList(list);
        return true;
    }

    return false;
}

void GoogleIMEEngine::reset(InputContext *ic) {
    ic->preedit().reset();
    ic->updatePreedit();
    ic->userInterface().hideCandidateList();
}

// Register addon factory
FCITX_ADDON_FACTORY("google-ime", GoogleIMEFactory)

#endif // HAVE_FCITX5
