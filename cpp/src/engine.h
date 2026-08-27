#pragma once

#ifdef HAVE_FCITX5

#include <fcitx/addonfactory.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

using namespace fcitx;

// Minimal compatibility layer: implement the small set of virtuals required by
// the platform's InputMethodEngine interface. This keeps the engine concrete
// and lets fcitx5 discover and load the addon. The implementation is a
// no-op prototype; later it can be extended to use query_daemon().

class GoogleIMEEngine : public InputMethodEngine {
public:
    GoogleIMEEngine() = default;
    ~GoogleIMEEngine() override = default;

    // Match the platform API: keyEvent signature is library-specific.
    void keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) override;

    // Reset is called when an InputContext is cleared (focus out / cancel).
    // Implementations should clear any composition buffer and hide candidates.
    void reset(InputContext *ic) override { (void)ic; }

private:
    // Sequence id for in-flight queries. Each new query increments the id;
    // deferred result handlers check it to decide whether their result is
    // stale and should be discarded.
    std::atomic<uint64_t> querySeq{0};
};

class GoogleIMEFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override { (void)manager; return new GoogleIMEEngine(); }
};

#endif // HAVE_FCITX5
