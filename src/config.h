#pragma once

#include <fcitx-config/configuration.h>  // FCITX_CONFIGURATION, Configuration
#include <fcitx-config/option.h>          // Option

#include <string>

// User-configurable options for the Google IME addon.
//
// The addon metadata (data/addon/google-ime.conf.in) sets Configurable=True, so
// fcitx5's configuration GUI — fcitx5-configtool, or the KDE Plasma Settings
// "Input Method" module (kcm_fcitx5) on Fedora / KDE Plasma — calls the
// engine's getConfig() / setConfig() / reloadConfig() overrides (see
// GoogleIMEEngine in engine.h). fcitx5 introspects the Option members declared
// here and generates the matching UI controls automatically; there is no
// separate hand-written config-description file and no KCM-specific code.
//
// The user-edited values are persisted by fcitx5 to
//   ~/.config/fcitx5/conf/google-ime.conf
// and read back on addon load (reloadConfig() is called from the engine
// constructor).
FCITX_CONFIGURATION(
    GoogleIMEConfig,

    // Google Input Tools "itc" code that identifies the input layout. This is
    // sent verbatim as the &itc= query parameter in the HTTPS GET to
    // inputtools.google.com, so changing it is exactly what switches the engine
    // between Cantonese, Mandarin pinyin, etc. — without any other code change.
    //
    // Common codes:
    //   yue-hant-t-i0-und      Cantonese, Traditional (default)
    //   zh-t-i0-pinyin          Mandarin pinyin
    //   zh-t-i0-wubi            Wubi (simplified)
    //   zh-hant-t-i0-cangjie    Cangjie, Traditional
    //   zh-t-i0-bopomofo        Bopomofo / Zhuyin
    //
    // Left blank, the engine falls back to the Cantonese code compiled in as
    // GoogleIMEEngine::kInputCode, so the addon keeps working with no config.
    fcitx::Option<std::string> inputCode{
        this, "InputCode",
        "Google Input Tools input layout code (the &itc= value). Examples: "
        "yue-hant-t-i0-und (Cantonese), zh-t-i0-pinyin (Mandarin pinyin), "
        "zh-hant-t-i0-cangjie (Cangjie)",
        "yue-hant-t-i0-und"};)
