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
    void reset(const InputMethodEntry &entry, InputContextEvent &event) override;

private:
    // Sequence id for in-flight queries. Each new query increments the id;
    // deferred result handlers check it to decide whether their result is
    // stale and should be discarded.
    std::atomic<uint64_t> querySeq{0};
    // Instance pointer injected by factory so addon can post to the main loop
    // without relying on InputContext API variability.
    Instance *instance_{nullptr};

    // Composition buffer tracking typed text
    std::string buffer_;

    // Keep pending EventSource objects alive until their deferred callbacks run.
    // Use a container so multiple in-flight events do not cancel each other by
    // overwriting a single unique_ptr instance.
    std::vector<std::unique_ptr<fcitx::EventSource>> pendingEvents_;
    std::mutex pendingEventMutex_;

    // Helper: remove an EventSource* from pendingEvents_ (called on main thread)
    void removePendingEvent(fcitx::EventSource *src);
};

class GoogleIMEFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override;
};

#endif // HAVE_FCITX5
