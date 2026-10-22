#include "config.h"

#include <string>

int main() {
    {
        GoogleIMEConfig config;
        if (config.preeditResetBehavior.value() !=
                PreeditResetBehavior::CommitRaw ||
            PreeditResetBehaviorToString(PreeditResetBehavior::CommitRaw) !=
                "Commit raw preedit" ||
            PreeditResetBehaviorToString(PreeditResetBehavior::Discard) !=
                "Discard preedit" ||
            PreeditResetBehaviorToString(PreeditResetBehavior::SelectFirst) !=
                "Select first candidate") {
            return 5;
        }
    }

    {
        fcitx::RawConfig raw;
        raw.setValueByPath("InputCode", "zh-t-i0-pinyin");
        if (!migrateLegacyInputCode(raw) ||
            !raw.valueByPath("Layout") ||
            *raw.valueByPath("Layout") != "Mandarin Pinyin" ||
            raw.valueByPath("CustomInputCode") ||
            !raw.valueByPath("InputCode") ||
            !raw.valueByPath("InputCode")->empty()) {
            return 1;
        }
    }
    {
        fcitx::RawConfig raw;
        raw.setValueByPath("InputCode", "custom-itc");
        if (!migrateLegacyInputCode(raw) ||
            !raw.valueByPath("Layout") ||
            *raw.valueByPath("Layout") != "Custom" ||
            !raw.valueByPath("CustomInputCode") ||
            *raw.valueByPath("CustomInputCode") != "custom-itc") {
            return 2;
        }
    }
    {
        fcitx::RawConfig raw;
        raw.setValueByPath("InputCode", "zh-t-i0-pinyin");
        raw.setValueByPath("Layout", "Cantonese");
        if (migrateLegacyInputCode(raw) ||
            !raw.valueByPath("InputCode") ||
            *raw.valueByPath("InputCode") != "zh-t-i0-pinyin") {
            return 3;
        }
    }

    if (effectivePreeditLimit(64) != 64 ||
        effectivePreeditLimit(0) != 64 ||
        effectivePreeditLimit(-1) != 64) {
        return 4;
    }
}
