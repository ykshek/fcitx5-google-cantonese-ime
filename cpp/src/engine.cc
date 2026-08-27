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
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>

GoogleIMEEngine::MyCandidateWord::MyCandidateWord(fcitx::Text text)
: text_(std::move(text)) {}

void GoogleIMEEngine::MyCandidateWord::select(fcitx::InputContext* ic) const {
    ic->commitString(text_.toString());   // convert fcitx::Text to std::string
}

fcitx::Text GoogleIMEEngine::MyCandidateWord::text() const {
    return text_;   // returns a copy
}

/*class UpdateUIEvent : public fcitx::Event {
public:
    UpdateUIEvent(fcitx::InputContext* ic,
                  std::vector<std::string> candidates,
                  std::string probe,
                  uint64_t mySeq,
                  GoogleIMEEngine* engine)
    : Event(fcitx::EventType::Post),
    ic_(ic),
    candidates_(std::move(candidates)),
    probe_(std::move(probe)),
    mySeq_(mySeq),
    engine_(engine) {}

    void call(fcitx::EventLoop* ) override {
        engine_->updateUI(ic_, std::move(candidates_), std::move(probe_), mySeq_);
    }

private:
    fcitx::InputContext* ic_;
    std::vector<std::string> candidates_;
    std::string probe_;
    uint64_t mySeq_;
    GoogleIMEEngine* engine_;
};
*/

void GoogleIMEEngine::updateUI(fcitx::InputContext* ic,
                               std::vector<std::string> candidates,
                               std::string probe,
                               uint64_t mySeq) {
    std::cerr << "GoogleIMEEngine(updateUI): running (mySeq=" << mySeq << ", querySeq=" << querySeq << ")\n";

    // Stale result?
    if (querySeq != mySeq) {
        std::cerr << "GoogleIMEEngine: stale result, drop\n";
        return;
    }

    // Buffer cleared? Hide panel.
    if (buffer_.empty()) {
        std::cerr << "GoogleIMEEngine: buffer cleared before result, hiding panel\n";
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    #if HAVE_FCITX5_UI
    std::cerr << "GoogleIMEEngine: populating candidate list (count=" << candidates.size() << ")\n";
    auto tmp = std::make_unique<fcitx::CommonCandidateList>();
    for (size_t i = 0; i < candidates.size(); ++i) {
        fcitx::Text t(candidates[i]);
        std::cerr << "  candidate[" << i << "]=" << t.toString() << "\n";
        auto cw = std::make_unique<MyCandidateWord>(t);
        tmp->insert(static_cast<int>(i), std::move(cw));
    }

    auto &panel = ic->inputPanel();
    std::cerr << "GoogleIMEEngine: reset panel and set preedit\n";
    panel.reset();
    std::cerr << "GoogleIMEEngine: capabilityFlags=" << static_cast<unsigned long>(ic->capabilityFlags()) << "\n";

    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit)) {
        panel.setClientPreedit(fcitx::Text(probe));
        std::cerr << "GoogleIMEEngine: called setClientPreedit and updatePreedit()\n";
        ic->updatePreedit();
    } else {
        panel.setPreedit(fcitx::Text(probe));
        std::cerr << "GoogleIMEEngine: called setPreedit\n";
    }

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

    // Test hook (optional)
    const char *force = std::getenv("GOOGLE_IME_TEST_FORCE_SERVER_PANEL");
    if (force && std::string(force) == "1") {
        std::cerr << "GoogleIMEEngine: TEST_HOOK forcing server-side preedit + candidate UI\n";
        auto testList = std::make_unique<fcitx::CommonCandidateList>();
        for (size_t i = 0; i < candidates.size(); ++i) {
            testList->insert(static_cast<int>(i), std::make_unique<MyCandidateWord>(fcitx::Text(candidates[i])));
        }
        panel.setPreedit(fcitx::Text(probe));
        panel.setCandidateList(std::move(testList));
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }

    // Final buffer check (if user typed more while waiting)
    if (buffer_ != probe) {
        std::cerr << "GoogleIMEEngine: buffer changed (current='" << buffer_ << "', probe='" << probe << "'), ignoring result\n";
        return;
    }
    #endif

    for (size_t i = 0; i < candidates.size(); ++i) {
        std::cerr << "  cand[" << i << "]=" << candidates[i] << "\n";
    }
}

#define HAVE_FCITX5_UI 1

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
        // Check if there is an active candidate list
        #if HAVE_FCITX5_UI
        auto cl = ic->inputPanel().candidateList();
        if (cl && cl->size() > 0) {
            // Space key commits the first candidate (index 0)
            if (sym == 32) {
                const auto& candidate = cl->candidate(0);
                ic->commitString(candidate.text().toString());
                buffer_.clear();
                ic->inputPanel().reset();
                ic->updatePreedit();
                ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
                keyEvent.filterAndAccept();
                return;
            }
            // Keys 1 to 9 commit the corresponding candidate
            else if (sym >= 49 && sym <= 57) {
                int index = sym - 49;
                if (index < cl->size()) {
                    const auto& candidate = cl->candidate(index);
                    ic->commitString(candidate.text().toString());
                    buffer_.clear();
                    ic->inputPanel().reset();
                    ic->updatePreedit();
                    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
                }
                keyEvent.filterAndAccept();
                return;
            }
        }
        #endif
        if (sym >= 32 && sym <= 126) {
            buffer_.push_back(static_cast<char>(sym));
            // consume the key so the client doesn't receive the character
            try { keyEvent.filterAndAccept(); } catch (...) {}
        } else if (sym == 8 || sym == 127) {
            // Backspace: remove last char if present and update UI
            if (!buffer_.empty()) buffer_.pop_back();
            try { keyEvent.filterAndAccept(); } catch (...) {}
        } else if (sym == 13 || sym == 10) {
            // Enter: commit current buffer
            if (!buffer_.empty()) {
                try { ic->commitString(buffer_); } catch (...) {}
                buffer_.clear();
                // hide panel
#if HAVE_FCITX5_UI
                ic->inputPanel().reset();
#endif
                try { ic->updatePreedit(); } catch (...) {}
                try { ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel); } catch (...) {}
                try { keyEvent.filterAndAccept(); } catch (...) {}
            }
            return;
        } else {
            // ignore other keys, let client handle them
            return;
        }

        // Update preedit immediately so user sees typed text while async query runs
#if HAVE_FCITX5_UI
        try {
            auto &panel = ic->inputPanel();
            panel.reset();
            if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit)) {
                panel.setClientPreedit(fcitx::Text(buffer_));
                ic->updatePreedit();
            } else {
                panel.setPreedit(fcitx::Text(buffer_));
                ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
            }
        } catch (...) {}
#endif

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
                loop.addPostEvent([ic, candidates = std::move(candidates), probe = std::move(probe), mySeq, this](fcitx::EventSource* /*src*/) -> bool {
                    this->updateUI(ic, std::move(candidates), std::move(probe), mySeq);
                    return false; // one-shot
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

// Reset engine state when the input context is cleared.
void GoogleIMEEngine::reset(const InputMethodEntry &entry, InputContextEvent &event) {
    (void)entry;
    buffer_.clear();
    auto ic = event.inputContext();
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
