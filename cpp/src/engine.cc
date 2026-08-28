#include "engine.h"
#include "daemon_client.h" // call query_daemon() from daemon_client.cc when needed
#include <thread>
#include <iostream>
#include <memory>
#include <cstdlib>
#include <algorithm>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/addonmanager.h>
#include <fcitx/userinterface.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>

// Google Input Tools returns candidates as JSON-style strings in which every
// non-ASCII character is escaped as a literal "\uXXXX" sequence (e.g. the
// Chinese character 从 arrives as the six ASCII bytes '\','u','4','e','c','e'
// instead of its 3-byte UTF-8 encoding). fcitx::Text(std::string) treats its
// argument as already-valid UTF-8, so without this decoding the candidate box
// shows the raw "\u4ece" text and selecting it commits that same escape text.
// This converts JSON \uXXXX escapes (including UTF-16 surrogate pairs) and the
// common JSON short escapes into real UTF-8 bytes.
static std::string unescapeJsonUnicode(const std::string &in) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto appendUtf8 = [](std::string &out, uint32_t cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\') {
            out.push_back(in[i]);
            continue;
        }
        if (i + 1 >= in.size()) {
            out.push_back('\\');
            break;
        }
        char e = in[i + 1];
        if (e == 'u' && i + 5 < in.size()) {
            int d0 = hex(in[i+2]), d1 = hex(in[i+3]), d2 = hex(in[i+4]), d3 = hex(in[i+5]);
            if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0) {
                out.push_back('\\');
                continue;
            }
            uint32_t cp = (d0 << 12) | (d1 << 8) | (d2 << 4) | d3;
            // UTF-16 surrogate pair: \uD800-\uDBFF followed by \uDC00-\uDFFF
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 11 < in.size() &&
                in[i+6] == '\\' && in[i+7] == 'u') {
                int e0 = hex(in[i+8]), e1 = hex(in[i+9]), e2 = hex(in[i+10]), e3 = hex(in[i+11]);
                if (e0 >= 0 && e1 >= 0 && e2 >= 0 && e3 >= 0) {
                    uint32_t lo = (e0 << 12) | (e1 << 8) | (e2 << 4) | e3;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        appendUtf8(out, cp);
                        i += 11;
                        continue;
                    }
                }
            }
            appendUtf8(out, cp);
            i += 5;
            continue;
        }
        switch (e) {
            case '"': out.push_back('"'); i += 1; break;
            case '\\': out.push_back('\\'); i += 1; break;
            case '/': out.push_back('/'); i += 1; break;
            case 'n': out.push_back('\n'); i += 1; break;
            case 'r': out.push_back('\r'); i += 1; break;
            case 't': out.push_back('\t'); i += 1; break;
            case 'b': out.push_back('\b'); i += 1; break;
            case 'f': out.push_back('\f'); i += 1; break;
            default: out.push_back('\\'); break;
        }
    }
    return out;
}

// The candidate text is stored in the fcitx::CandidateWord base class (via the
// base constructor). CandidateWord::text() is non-virtual, so the UI and
// CommonCandidateList obtain the displayed string through the base text() --
// which now actually contains the candidate string.
void GoogleIMEEngine::MyCandidateWord::select(fcitx::InputContext* ic) const {
    ic->commitString(text().toString());
    if (engine_) {
        engine_->finishCandidate(ic);
    }
}

void GoogleIMEEngine::finishCandidate(fcitx::InputContext* ic) {
    // Invalidate any in-flight async results so a stale candidate list cannot
    // reappear after the user already selected a word.
    ++querySeq;
    buffer_.clear();
    ic->inputPanel().reset();
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void GoogleIMEEngine::updateUI(fcitx::InputContext* ic,
                               std::vector<std::string> candidates,
                               std::string probe,
                               uint64_t mySeq) {
    std::cerr << "GoogleIMEEngine(updateUI): entered (mySeq=" << mySeq << ", querySeq=" << querySeq << ")\n";

    if (querySeq != mySeq) {
        std::cerr << "GoogleIMEEngine: stale result, drop\n";
        return;
    }

    if (buffer_.empty()) {
        std::cerr << "GoogleIMEEngine: buffer cleared before result, hiding panel\n";
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    std::cerr << "GoogleIMEEngine: populating candidate list (count=" << candidates.size() << ")" << std::endl;
    // Use our own minimal CandidateList (not CommonCandidateList). It does NOT
    // implement the Bulk/Pageable/Cursor interfaces, so the classicui/kimpanel
    // render path can never reach CommonCandidateList::checkGlobalIndex, which
    // was throwing "invalid global index" (cursor -1) and aborting fcitx5.
    auto tmp = std::make_unique<GoogleIMECandidateList>();
    for (size_t i = 0; i < candidates.size(); ++i) {
        std::string text = unescapeJsonUnicode(candidates[i]);
        std::cerr << "  candidate[" << i << "]=" << text << "" << std::endl;
        fcitx::Text t(std::move(text));
        tmp->append(std::make_unique<MyCandidateWord>(std::move(t), this));
    }
    std::cerr << "GoogleIMEEngine: appended " << tmp->size() << " candidates" << std::endl;

    auto &panel = ic->inputPanel();
    std::cerr << "GoogleIMEEngine: reset panel and set preedit" << std::endl;
    panel.reset();

    // Set preedit
    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit)) {
        panel.setClientPreedit(fcitx::Text(probe));
        ic->updatePreedit();
        std::cerr << "GoogleIMEEngine: called setClientPreedit and updatePreedit()" << std::endl;
    } else {
        panel.setPreedit(fcitx::Text(probe));
        std::cerr << "GoogleIMEEngine: called setPreedit" << std::endl;
    }

    std::cerr << "GoogleIMEEngine: setting candidate list" << std::endl;
    panel.setCandidateList(std::move(tmp));
    auto cl = panel.candidateList();
    std::cerr << "GoogleIMEEngine: panel.empty()=" << (panel.empty() ? "true" : "false")
              << ", candidateList=" << (cl ? "present" : "null")
              << ", clSize=" << (cl ? cl->size() : -1) << "" << std::endl;

    // Force UI update: call both updatePreedit and updateUserInterface
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    std::cerr << "GoogleIMEEngine: updateUserInterface(InputPanel) called" << std::endl;

    // Optional test hook
    const char *force = std::getenv("GOOGLE_IME_TEST_FORCE_SERVER_PANEL");
    if (force && std::string(force) == "1") {
        std::cerr << "GoogleIMEEngine: TEST_HOOK forcing server-side preedit + candidate UI" << std::endl;
        auto testList = std::make_unique<GoogleIMECandidateList>();
        for (size_t i = 0; i < candidates.size(); ++i) {
            std::string text = unescapeJsonUnicode(candidates[i]);
            testList->append(std::make_unique<MyCandidateWord>(fcitx::Text(std::move(text)), this));
        }
        panel.setPreedit(fcitx::Text(probe));
        panel.setCandidateList(std::move(testList));
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }

    // Final buffer check
    if (buffer_ != probe) {
        std::cerr << "GoogleIMEEngine: buffer changed (current='" << buffer_ << "', probe='" << probe << "'), ignoring result\n";
        return;
    }
}

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
        auto cl = ic->inputPanel().candidateList();
        if (cl && cl->size() > 0) {
            // Space key commits the first candidate (index 0)
            if (sym == 32) {
                cl->candidate(0).select(ic);
                keyEvent.filterAndAccept();
                return;
            }
            // Keys 1 to 9 commit the corresponding candidate
            else if (sym >= 49 && sym <= 57) {
                int index = sym - 49;
                if (index < cl->size()) {
                    cl->candidate(index).select(ic);
                }
                keyEvent.filterAndAccept();
                return;
            }
        }
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
                ic->inputPanel().reset();
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
        try {
            auto &panel = ic->inputPanel();
            // FIX: Do NOT reset the panel for every key press; only clear when buffer is empty.
            if (buffer_.empty()) {
                panel.reset();
            } else {
                if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit)) {
                    panel.setClientPreedit(fcitx::Text(buffer_));
                    ic->updatePreedit();
                } else {
                    panel.setPreedit(fcitx::Text(buffer_));
                    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
                }
            }
        } catch (...) {}

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
                auto source = loop.addDeferEvent([ic, candidates = std::move(candidates), probe = std::move(probe), mySeq, this](fcitx::EventSource* src) -> bool {
                    std::cerr << "GoogleIMEEngine: deferEvent callback running\n";
                    // Guard the UI update: an exception here would otherwise
                    // abort the entire fcitx5 process. Log and continue.
                    try {
                        this->updateUI(ic, std::move(candidates), std::move(probe), mySeq);
                    } catch (const std::exception &e) {
                        std::cerr << "GoogleIMEEngine(updateUI): exception: " << e.what() << "\n";
                    } catch (...) {
                        std::cerr << "GoogleIMEEngine(updateUI): unknown exception\n";
                    }
                    // Remove ourselves from pendingEvents_
                    std::lock_guard<std::mutex> lk(pendingEventMutex_);
                    for (auto it = pendingEvents_.begin(); it != pendingEvents_.end(); ++it) {
                        if (it->get() == src) {
                            pendingEvents_.erase(it);
                            break;
                        }
                    }
                    return false; // one-shot
                });

                {
                    std::lock_guard<std::mutex> lk(pendingEventMutex_);
                    pendingEvents_.push_back(std::move(source));
                }
                (void)source; // suppress unused variable warning

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
    std::lock_guard<std::mutex> lk(pendingEventMutex_);
    pendingEvents_.clear();
    buffer_.clear();
    auto ic = event.inputContext();
    if (ic) {
        ic->inputPanel().reset();
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
