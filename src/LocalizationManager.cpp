#include "LocalizationManager.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

namespace fs = std::filesystem;

LocalizationManager& LocalizationManager::GetInstance() {
    static LocalizationManager s_instance;
    return s_instance;
}

LocalizationManager::LocalizationManager()
    : m_configuredLang(L"default"), m_effectiveLang(L"en-US") {
}

std::wstring LocalizationManager::GetWindowsDisplayLanguage() const {
    ULONG numLangs = 0;
    ULONG cchBuffer = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &numLangs, NULL, &cchBuffer) && cchBuffer > 0) {
        std::vector<wchar_t> buffer(cchBuffer);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &numLangs, buffer.data(), &cchBuffer) && numLangs > 0) {
            return std::wstring(buffer.data());
        }
    }

    LANGID langId = GetUserDefaultUILanguage();
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = { 0 };
    if (LCIDToLocaleName(MAKELCID(langId, SORT_DEFAULT), localeName, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        return localeName;
    }

    return L"en-US";
}

std::wstring LocalizationManager::DetectWindowsLanguage() const {
    return GetWindowsDisplayLanguage();
}

void LocalizationManager::Initialize(const std::wstring& baseDir) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!baseDir.empty()) {
        m_langDir = baseDir;
    } else {
        wchar_t exePath[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        fs::path p(exePath);
        m_langDir = (p.parent_path() / L"lang").wstring();
    }

    ScanLanguageFiles();

    // Load en-US as fallback baseline
    fs::path enPath = fs::path(m_langDir) / L"en-US.json";
    std::wstring code, name;
    if (fs::exists(enPath)) {
        LoadLanguageFile(enPath.wstring(), m_fallbackStrings, code, name);
    }

    // Apply configured language
    SetLanguage(m_configuredLang);
}

void LocalizationManager::ScanLanguageFiles() {
    m_availableLanguages.clear();

    // Always offer Follow Windows as the first default option
    m_availableLanguages.push_back({ L"default", L"Follow Windows" });

    if (!fs::exists(m_langDir) || !fs::is_directory(m_langDir)) {
        // Fallback default English item if directory doesn't exist
        m_availableLanguages.push_back({ L"en-US", L"English (United States)" });
        return;
    }

    try {
        for (const auto& entry : fs::directory_iterator(m_langDir)) {
            if (entry.is_regular_file() && entry.path().extension() == L".json") {
                std::unordered_map<std::wstring, std::wstring> dummyMap;
                std::wstring code, name;
                if (LoadLanguageFile(entry.path().wstring(), dummyMap, code, name)) {
                    if (code.empty()) {
                        code = entry.path().stem().wstring();
                    }
                    if (name.empty()) {
                        name = code;
                    }

                    // Check if already present
                    bool found = false;
                    for (const auto& item : m_availableLanguages) {
                        if (_wcsicmp(item.code.c_str(), code.c_str()) == 0) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        m_availableLanguages.push_back({ code, name });
                    }
                }
            }
        }
    } catch (...) {
        // Ignore filesystem scan exceptions
    }

    // Ensure en-US is listed if not already present
    bool hasEn = false;
    for (const auto& item : m_availableLanguages) {
        if (_wcsicmp(item.code.c_str(), L"en-US") == 0) {
            hasEn = true;
            break;
        }
    }
    if (!hasEn) {
        m_availableLanguages.push_back({ L"en-US", L"English (United States)" });
    }
}

bool LocalizationManager::LoadLanguageFile(const std::wstring& filePath,
                                          std::unordered_map<std::wstring, std::wstring>& targetMap,
                                          std::wstring& outCode,
                                          std::wstring& outName) {
    try {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) return false;

        std::string utf8Content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (utf8Content.empty()) return false;

        // Skip UTF-8 BOM if present
        size_t offset = 0;
        if (utf8Content.size() >= 3 &&
            (unsigned char)utf8Content[0] == 0xEF &&
            (unsigned char)utf8Content[1] == 0xBB &&
            (unsigned char)utf8Content[2] == 0xBF) {
            offset = 3;
        }

        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Content.c_str() + offset, (int)(utf8Content.size() - offset), NULL, 0);
        if (wlen <= 0) return false;

        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8Content.c_str() + offset, (int)(utf8Content.size() - offset), &wstr[0], wlen);

        auto jsonObj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::hstring(wstr));
        targetMap.clear();

        for (auto const& pair : jsonObj) {
            std::wstring key = pair.Key().c_str();
            if (pair.Value().ValueType() == winrt::Windows::Data::Json::JsonValueType::String) {
                std::wstring val = pair.Value().GetString().c_str();
                if (key == L"__language_code") {
                    outCode = val;
                } else if (key == L"__language_name") {
                    outName = val;
                } else {
                    targetMap[key] = val;
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

void LocalizationManager::SetLanguage(const std::wstring& langCode) {
    m_configuredLang = langCode.empty() ? L"default" : langCode;
    m_strings.clear();

    std::wstring targetCode = m_configuredLang;
    if (_wcsicmp(targetCode.c_str(), L"default") == 0) {
        targetCode = DetectWindowsLanguage();
    }

    // Try loading exact target code, e.g. "hu-HU.json" or "en-US.json"
    fs::path targetPath = fs::path(m_langDir) / (targetCode + L".json");
    std::wstring code, name;
    bool loaded = false;

    if (fs::exists(targetPath)) {
        loaded = LoadLanguageFile(targetPath.wstring(), m_strings, code, name);
        if (loaded) {
            m_effectiveLang = targetCode;
        }
    }

    // If not found and targetCode has a dash (e.g. "en-GB"), try language prefix (e.g. "en-*.json")
    if (!loaded) {
        size_t dashPos = targetCode.find(L'-');
        if (dashPos != std::wstring::npos) {
            std::wstring prefix = targetCode.substr(0, dashPos);
            try {
                if (fs::exists(m_langDir)) {
                    for (const auto& entry : fs::directory_iterator(m_langDir)) {
                        if (entry.is_regular_file() && entry.path().extension() == L".json") {
                            std::wstring stem = entry.path().stem().wstring();
                            if (stem.rfind(prefix, 0) == 0) {
                                loaded = LoadLanguageFile(entry.path().wstring(), m_strings, code, name);
                                if (loaded) {
                                    m_effectiveLang = stem;
                                    break;
                                }
                            }
                        }
                    }
                }
            } catch (...) {}
        }
    }

    // Fallback to en-US if still not loaded
    if (!loaded) {
        fs::path enPath = fs::path(m_langDir) / L"en-US.json";
        if (fs::exists(enPath)) {
            LoadLanguageFile(enPath.wstring(), m_strings, code, name);
            m_effectiveLang = L"en-US";
        }
    }
}

std::wstring LocalizationManager::Get(const std::wstring& key, const std::wstring& fallback) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_strings.find(key);
    if (it != m_strings.end() && !it->second.empty()) {
        return it->second;
    }

    auto fbIt = m_fallbackStrings.find(key);
    if (fbIt != m_fallbackStrings.end() && !fbIt->second.empty()) {
        return fbIt->second;
    }

    return fallback.empty() ? key : fallback;
}

std::wstring LocalizationManager::GetCurrentLanguage() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_configuredLang;
}

std::wstring LocalizationManager::GetEffectiveLanguage() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_effectiveLang;
}

std::vector<LocalizationManager::LanguageItem> LocalizationManager::GetAvailableLanguages() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_availableLanguages;
}
