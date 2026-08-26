#pragma once

#ifdef HAVE_FCITX5

#include <fcitx/addonfactory.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

using namespace fcitx;

class GoogleIMEEngine : public InputMethodEngine {
public:
    GoogleIMEEngine(Instance *instance) : InputMethodEngine(instance) {}
    ~GoogleIMEEngine() override = default;

    const std::string &id() const override { static std::string i = "google-ime"; return i; }
    const std::string &name() const override { static std::string n = "Google Input Tools (prototype)"; return n; }

    // Called when engine is activated for an input context
    void activate(InputContext *ic) override;
    void deactivate(InputContext *ic) override;

    // Handle key events. Return true if the event is swallowed by engine.
    bool keyEvent(InputContext *ic, KeyEvent &keyEvent) override;

    // Reset engine state for the context
    void reset(InputContext *ic) override;
};

class GoogleIMEFactory : public AddonFactory {
public:
    AddonInstance *create(Instance *instance) override { return new GoogleIMEEngine(instance); }
};

#endif // HAVE_FCITX5
