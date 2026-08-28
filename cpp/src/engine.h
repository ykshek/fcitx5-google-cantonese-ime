#pragma once

#include <fcitx/addonfactory.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>              // must come before candidatelist.h
#include <fcitx/candidatelist.h>     // for CandidateWord / CandidateList
#include <fcitx/inputcontext.h>      // for InputContext
#include <fcitx-utils/event.h>       // for EventSource
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// A minimal CandidateList that does NOT implement the Bulk / Pageable /
// Cursor / Actionable interfaces. classicui and kimpanel render the panel
// using only size()/candidate(i)/label(i)/cursorIndex(), all of which we
// bounds-check ourselves with checkIndex (which throws "invalid index",
// *never* the "invalid global index" that CommonCandidateList::checkGlobalIndex
// throws). Since we never call setBulk()/setPageable()/setCursorModifiable()/...
// in the constructor, toBulk()/toPageable()/toCursorModifiable()/... all return
// nullptr, so classicui's prev/next/cursor-move code paths are never entered.
//
// This sidesteps a recurring crash where CommonCandidateList defaulted to a
// global cursor of -1 and a deferred classicui render path ended up calling
// checkGlobalIndex(-1) ("CommonCandidateList: invalid global index") and
// aborting the whole fcitx5 process. Selection (click / number keys / space)
// still works because the per-candidate MyCandidateWord::select() does the
// real commit and state reset.
class GoogleIMECandidateList : public fcitx::CandidateList {
public:
    GoogleIMECandidateList() = default;

    void append(std::unique_ptr<fcitx::CandidateWord> word) {
        if (words_.empty()) {
            // first candidate is the highlighted one
            cursorIndex_ = 0;
        }
        const auto idx = static_cast<int>(words_.size());
        words_.push_back(std::move(word));
        // default labels: "1." "2." ... up to "9." then "0."
        const char digit = (idx < 9) ? ('1' + idx) : '0';
        labels_.emplace_back(std::string(1, digit) + ".");
    }

    int size() const override { return static_cast<int>(words_.size()); }

    const fcitx::CandidateWord &candidate(int idx) const override {
        checkIndex(idx);
        return *words_[idx];
    }

    const fcitx::Text &label(int idx) const override {
        if (idx < 0 || idx >= static_cast<int>(labels_.size())) {
            static const fcitx::Text empty;
            return empty;
        }
        return labels_[idx];
    }

    int cursorIndex() const override { return cursorIndex_; }

    fcitx::CandidateLayoutHint layoutHint() const override {
        return fcitx::CandidateLayoutHint::Vertical;
    }

private:
    void checkIndex(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(words_.size())) {
            throw std::invalid_argument("GoogleIMECandidateList: invalid index");
        }
    }
    std::vector<std::unique_ptr<fcitx::CandidateWord>> words_;
    std::vector<fcitx::Text> labels_;
    int cursorIndex_ = -1;
};

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
    // Candidate word. The display text MUST be handed to the
    // fcitx::CandidateWord base constructor: CandidateWord::text() is
    // *non-virtual* and returns exactly that stored text, and the UI as well
    // as CommonCandidateList read the candidate text through the base class.
    // Keeping a separate text_ member + a shadowing text() (as the previous
    // code did) leaves the base text empty -> empty candidate box and
    // selection committing nothing.
    class MyCandidateWord : public fcitx::CandidateWord {
    public:
        MyCandidateWord(fcitx::Text text, GoogleIMEEngine* engine)
            : fcitx::CandidateWord(std::move(text)), engine_(engine) {}
        void select(fcitx::InputContext* ic) const override;
    private:
        GoogleIMEEngine* engine_ = nullptr;
    };

    // Commit the chosen candidate text is done by MyCandidateWord::select();
    // this resets the composition state afterwards (clear buffer, hide panel,
    // invalidate in-flight async results).
    void finishCandidate(fcitx::InputContext* ic);

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
