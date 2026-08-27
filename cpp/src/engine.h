#pragma once

#ifdef HAVE_FCITX5

#include <fcitx/addonfactory.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

using namespace fcitx;

// Forward declare EventSource so engine.h can hold a unique_ptr without
// including the eventloop interface header here.
namespace fcitx { struct EventSource; }

// Minimal compatibility layer: implement the small set of virtuals required by
// the platform's InputMethodEngine interface. This keeps the engine concrete
// and lets fcitx5 discover and load the addon. The implementation is a
// no-op prototype; later it can be extended to use query_daemon().

class GoogleIMEEngine : public InputMethodEngine {
public:
    explicit GoogleIMEEngine(Instance *instance = nullptr) : instance_(instance) {}
    ~GoogleIMEEngine() override = default;

    // Match the platform API: keyEvent signature is library-specific.
    void keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) override;

    // Reset is called when an InputContext is cleared (focus out / cancel).
    // Implementations should clear any composition buffer and hide candidates.
    void reset(InputContext *ic) { (void)ic; }

private:
    // Sequence id for in-flight queries. Each new query increments the id;
    // deferred result handlers check it to decide whether their result is
    // stale and should be discarded.
    std::atomic<uint64_t> querySeq{0};
    // Instance pointer injected by factory so addon can post to the main loop
    // without relying on InputContext API variability.
    Instance *instance_{nullptr};

    // Keep pending EventSource alive until the deferred callback has run.
    std::unique_ptr<fcitx::EventSource> pendingEvent_;
    std::mutex pendingEventMutex_;
};

class GoogleIMEFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override;
};

#endif // HAVE_FCITX5
