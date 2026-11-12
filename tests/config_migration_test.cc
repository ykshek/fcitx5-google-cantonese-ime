#include "config.h"

#include <string>

int main() {
    {
        GoogleIMEConfig config;
        if (config.candidateListSize.value() != 9 ||
            config.candidateCacheEntries.value() != 64 ||
            config.candidateCacheTtlMinutes.value() != 5 ||
            config.persistentCandidateCache.value() ||
            config.preeditResetBehavior.value() !=
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
    if (effectiveCandidateListSize(9) != 9 ||
        effectiveCandidateListSize(1) != 1 ||
        effectiveCandidateListSize(0) != 1 ||
        effectiveCandidateListSize(-1) != 1 ||
        effectiveCandidateListSize(10) != 9) {
        return 6;
    }
    if (effectiveCandidateCacheEntries(64) != 64 ||
        effectiveCandidateCacheEntries(0) != 0 ||
        effectiveCandidateCacheEntries(-1) != 0 ||
        effectiveCandidateCacheEntries(300) != 256) {
        return 7;
    }
    if (effectiveCandidateCacheTtlMinutes(5) != 5 ||
        effectiveCandidateCacheTtlMinutes(0) != 1 ||
        effectiveCandidateCacheTtlMinutes(2000) != 1440) {
        return 8;
    }
}
