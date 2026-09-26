/*
    nvpwr_ui_state.cpp — language helpers for the shared state model.
*/

#include "nvpwr_ui_state.h"

namespace nvpwr {

const wchar_t* LangToIniValue(Lang lang) {
    switch (lang) {
    case Lang::Chinese: return L"zh";
    case Lang::Russian: return L"ru";
    default:            return L"en";
    }
}

/*
    Accepts the 1.9.0 three-valued form ("en"/"zh"/"ru") and, for migration, the
    1.8.0 boolean [Interface] Russian key that callers pass through as "0"/"1".
    Anything unrecognised yields English rather than an arbitrary default.
*/
Lang LangFromIniValue(const std::wstring& value) {
    if (value == L"zh" || value == L"zh-CN" || value == L"chinese") return Lang::Chinese;
    if (value == L"ru" || value == L"russian") return Lang::Russian;
    if (value == L"en" || value == L"english") return Lang::English;

    /* 1.8.0 migration: "1" meant Russian, "0" meant English. */
    if (value == L"1") return Lang::Russian;
    if (value == L"0") return Lang::English;

    /* No stored preference: this build's primary audience reads Chinese. */
    return Lang::Chinese;
}

} /* namespace nvpwr */
