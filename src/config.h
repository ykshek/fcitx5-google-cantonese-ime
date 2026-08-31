#pragma once

#include <fcitx-config/configuration.h>  // FCITX_CONFIGURATION, Configuration
#include <fcitx-config/enum.h>  // FCITX_CONFIG_ENUM_NAME_WITH_I18N
#include <fcitx-config/option.h>          // Option, OptionWithAnnotation
#include <fcitx-utils/i18n.h>             // N_() / _() for dropdown labels

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

// The Google Input Tools input layouts offered in the configuration dropdown.
// Each value maps (see googleLayoutItc() below) to a verified `itc` code sent
// as the &itc= query parameter to inputtools.google.com.
//
// The codes were tested live against
//   https://inputtools.google.com/request
// Shape-based schemes need their year suffix (e.g. wubi-1986, cangjie-1982);
// the bare codes (zh-t-i0-wubi, zh-hant-t-i0-cangjie) are rejected by Google
// with INVALID_INPUT_METHOD_NAME. "Custom" defers to the user-supplied string
// (customInputCode option below).
//
// Note: Zhuyin / Bopomofo is intentionally absent. No working `itc` code
// was found for Google's transliteration endpoint — the tested variants
// (zh-t-i0-zhuyin, zh-hant-t-i0-zhuyin, *-t-i0-bopomofo) all return
// INVALID_INPUT_METHOD_NAME. Use the "Custom" layout with a working code if you
// have one.
enum class GoogleInputLayout {
    Cantonese,       // yue-hant-t-i0-und
    MandarinPinyin,  // zh-t-i0-pinyin
    Wubi,            // zh-t-i0-wubi-1986
    Cangjie,         // zh-hant-t-i0-cangjie-1982
    Custom,          // customInputCode
};

// Generates GoogleInputLayoutI18NAnnotation (dropdown labels) and the
// marshall/unmarshall helpers (the label string is also the value persisted to
// the config file, so google-ime.conf stays human-readable).
FCITX_CONFIG_ENUM_NAME_WITH_I18N(GoogleInputLayout,
    N_("Cantonese"),
    N_("Mandarin Pinyin"),
    N_("Wubi"),
    N_("Cangjie"),
    N_("Custom"));

FCITX_CONFIGURATION(
    GoogleIMEConfig,

    // Which Google Input Tools input layout to query. Rendered as a dropdown
    // by the config UI; the selected entry's itc code is resolved in
    // effectiveInputCode() (engine.cc) via googleLayoutItc().
    fcitx::OptionWithAnnotation<GoogleInputLayout,
                                GoogleInputLayoutI18NAnnotation>
        layout{this, "Layout", _("Input layout"),
               GoogleInputLayout::Cantonese};

    // The Google Input Tools `itc` code to send when Layout is "Custom".
    // Ignored for the other layouts. Defaults to the Cantonese code so the
    // addon keeps working even if the user switches to Custom and hasn't
    // filled this in yet.
    fcitx::Option<std::string> customInputCode{
        this, "CustomInputCode",
        _("Custom Google Input Tools code (the &itc= value, used only when "
          "Layout is Custom). Examples: yue-hant-t-i0-und (Cantonese), "
          "zh-t-i0-pinyin (Mandarin pinyin)"),
        "yue-hant-t-i0-und"};
)

// Returns the itc code for a layout. For Custom, returns the user-supplied
// customCode verbatim (which may be empty — the caller falls back to the
// compiled-in Cantonese default in that case).
inline std::string googleLayoutItc(GoogleInputLayout layout,
                                   const std::string &customCode) {
    switch (layout) {
    case GoogleInputLayout::Cantonese:
        return "yue-hant-t-i0-und";
    case GoogleInputLayout::MandarinPinyin:
        return "zh-t-i0-pinyin";
    case GoogleInputLayout::Wubi:
        return "zh-t-i0-wubi-1986";
    case GoogleInputLayout::Cangjie:
        return "zh-hant-t-i0-cangjie-1982";
    case GoogleInputLayout::Custom:
        return customCode;
    }
    return {};
}

// Reverse mapping used only by the one-time config migration in
// reloadConfig(): given a legacy free-text itc code, returns the matching
// built-in layout, or Custom if it isn't one of the known codes (so a user's
// previously-configured value is preserved as CustomInputCode rather than
// silently reset to Cantonese).
inline GoogleInputLayout googleLayoutFromItc(const std::string &code) {
    if (code == "yue-hant-t-i0-und") return GoogleInputLayout::Cantonese;
    if (code == "zh-t-i0-pinyin") return GoogleInputLayout::MandarinPinyin;
    if (code == "zh-t-i0-wubi-1986") return GoogleInputLayout::Wubi;
    if (code == "zh-hant-t-i0-cangjie-1982") return GoogleInputLayout::Cangjie;
    return GoogleInputLayout::Custom;
}
