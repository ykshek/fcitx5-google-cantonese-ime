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

    // Match the platform API: keyEvent signature is library-specific. Use the
    // signature shown by the build errors: keyEvent(const InputMethodEntry&, KeyEvent&).
    void keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) override;

    // Provide a simple reset hook. Signature may vary; provide a generic one.
    void reset(InputContext *ic) { }
};

class GoogleIMEFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override { (void)manager; return new GoogleIMEEngine(); }
};

#endif // HAVE_FCITX5
