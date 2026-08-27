#pragma once

#include <fcitx/addonfactory.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>              // must come before candidatelist.h
#include <fcitx/candidatelist.h>     // for CandidateWord
#include <fcitx/inputcontext.h>      // for InputContext
#include <fcitx-utils/event.h>       // for EventSource
#include <atomic>
#include <mutex>
#include <memory>

class GoogleIMEEngine : public fcitx::InputMethodEngine {
public:
    explicit GoogleIMEEngine(fcitx::Instance *instance = nullptr) : instance_(instance) {}
    ~GoogleIMEEngine() override = default;

    void keyEvent(const fcitx::InputMethodEntry &entry, fcitx::KeyEvent &keyEvent) override;
    void reset(const fcitx::InputMethodEntry &entry, fcitx::InputContextEvent &event) override;
    void updateUI(fcitx::InputContext* ic,
                  std::vector<std::string> candidates,
                  std::string probe,
                  uint64_t mySeq);

private:
    // Private nested candidate word class.
    class MyCandidateWord : public fcitx::CandidateWord {
    public:
        explicit MyCandidateWord(fcitx::Text text);
        void select(fcitx::InputContext* ic) const override;
        fcitx::Text text() const;   // remove override
    private:
        fcitx::Text text_;
    };

    std::atomic<uint64_t> querySeq{0};
    fcitx::Instance *instance_{nullptr};
    std::string buffer_;
    std::vector<std::unique_ptr<fcitx::EventSource>> pendingEvents_;
    std::mutex pendingEventMutex_;
};

class GoogleIMEFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override;
};
