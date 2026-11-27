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
    LoadBuiltInDefaults();
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
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!baseDir.empty()) {
        m_langDir = baseDir;
    } else {
        wchar_t exePath[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        fs::path p(exePath);
        m_langDir = (p.parent_path() / L"lang").wstring();
    }

    ScanLanguageFiles();

    // Load en-US from disk if available to override/update fallback baseline
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
        } else {
            m_strings = m_fallbackStrings;
        }
        m_effectiveLang = L"en-US";
    }
}

std::wstring LocalizationManager::Get(const std::wstring& key, const std::wstring& fallback) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

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
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_configuredLang;
}

std::wstring LocalizationManager::GetEffectiveLanguage() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_effectiveLang;
}

std::vector<LocalizationManager::LanguageItem> LocalizationManager::GetAvailableLanguages() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_availableLanguages;
}

void LocalizationManager::LoadBuiltInDefaults() {
    m_fallbackStrings = {
        { L"AppTitle", L"ClickBrightness settings" },
        { L"Nav_General", L"General" },
        { L"Nav_Monitors", L"Monitors" },
        { L"Nav_Timings", L"Timings & Global" },
        { L"Nav_Shortcuts", L"Shortcuts" },
        { L"Monitors_Title", L"Monitors & Displays" },
        { L"Monitors_Subtitle", L"Configure main display, sync/separate modes, offsets, and per-monitor controls" },
        { L"Sec_MonitorSubmenus", L"Customization & Controls" },
        { L"Desc_SubmenuOsd", L"Overlay accent color for this display" },
        { L"Desc_SubmenuFlyout", L"Flyout scrolling and slider accent color" },
        { L"Opt_MainMonitor", L"Main Display" },
        { L"Desc_MainMonitor", L"Primary reference display used for brightness offsets and Sync mode" },
        { L"Opt_SyncLaptopFnKeys", L"Sync External Monitors with Laptop Fn Keys" },
        { L"Desc_SyncLaptopFnKeys", L"Automatically sync external Sync-mode monitors when adjusting internal display brightness via laptop Fn keys" },
        { L"Sec_MonitorConfig", L"Display Configuration" },
        { L"Opt_ConfigMonitor", L"Selected Display" },
        { L"Desc_ConfigMonitor", L"Choose a detected display to configure its mode, name, and controls" },
        { L"Opt_MonitorEnabled", L"Brightness Control" },
        { L"Desc_MonitorEnabled", L"Enable or disable brightness adjustments for this display" },
        { L"Opt_MonitorCustomName", L"Monitor Name" },
        { L"Desc_MonitorCustomName", L"Display label shown in the OSD banner and flyout slider (max 20 characters)" },
        { L"Opt_MonitorControlMode", L"Control Mode" },
        { L"Desc_MonitorControlMode", L"Sync follows the Main Display with an offset; Separate uses independent controls" },
        { L"MonitorMode_Sync", L"Sync" },
        { L"MonitorMode_Separate", L"Separate" },
        { L"Opt_MonitorOffset", L"Brightness Offset (%)" },
        { L"Desc_MonitorOffset", L"Relative brightness offset applied compared to the Main Display" },
        { L"Opt_SyncOverscrollMax", L"Overscroll when a display is at 100%" },
        { L"Desc_SyncOverscrollMax", L"Continue increasing other synced displays even when at least one synced display is already at 100%" },
        { L"Opt_SyncOverscrollMin", L"Overscroll when a display is at 0%" },
        { L"Desc_SyncOverscrollMin", L"Continue decreasing other synced displays even when at least one synced display is already at 0%" },
        { L"Opt_SyncSeparateTrayIcon", L"Separate Tray Icon" },
        { L"Desc_SyncSeparateTrayIcon", L"Show a dedicated system tray brightness icon for this Sync-mode display" },
        { L"Monitor_SyncNotice", L"This display is in Sync mode and uses the Main Display's shortcuts and wheel controls. You can customize its OSD color, Flyout color, and optional tray icon below." },
        { L"InternalBadge", L"Internal" },
        { L"Timings_Title", L"Timings & Global" },
        { L"Timings_Subtitle", L"Global hardware delays, OSD appearance, and flyout keyboard navigation" },
        { L"Sec_GlobalOsdFlyout", L"OSD & Flyout Global" },
        { L"Sec_TimingsSubmenus", L"Feature Settings & Delays" },
        { L"Desc_SubmenuTimingsShortcuts", L"Hotkey debounce delay and key hold repeat interval" },
        { L"Desc_SubmenuTimingsOsd", L"OSD banner theme and auto-hide duration" },
        { L"Desc_SubmenuTimingsTray", L"Mouse wheel scrolling debounce delay on the tray icon" },
        { L"Desc_SubmenuTimingsFlyout", L"Keyboard controls, scrolling delay, and button debounce delay" },
        { L"Nav_OSD", L"On-Screen Display" },
        { L"Nav_Tray", L"System Tray" },
        { L"Nav_OverlayTray", L"OSD & Tray" },
        { L"Nav_FlyoutSlider", L"Flyout Slider" },
        { L"Nav_About", L"About" },
        { L"Search_Placeholder", L"Search settings" },
        { L"Search_NoResults", L"No results found" },
        { L"General_Title", L"General Settings" },
        { L"General_Subtitle", L"Windows 11 DDC/CI Hardware Monitor Brightness Control" },
        { L"Opt_ResetDefaults", L"Reset Settings" },
        { L"Desc_ResetDefaults", L"Restore all settings to their default values" },
        { L"Btn_ResetDefaults", L"Reset Defaults" },
        { L"Confirm_ResetDefaults", L"All settings will be reset. Do you want to continue?" },
        { L"Btn_ConfirmResetDefaults", L"Yes, reset settings" },
        { L"Confirm_MainMonitor", L"Changing the main display will update Sync mode and tray icons. Do you want to continue?" },
        { L"Btn_ConfirmMainMonitor", L"Yes, change main display" },
        { L"Tip_ResetDefaults", L"Reset all settings to default values" },
        { L"Status_Ready_Title", L"Hardware DDC/CI Ready" },
        { L"Status_Ready_Msg", L"DDC/CI communication active. Hardware monitor brightness will update directly." },
        { L"Status_Offline_Title", L"DDC/CI Driver Offline" },
        { L"Status_Offline_Msg", L"No DDC/CI compatible monitor answered, or driver is still initializing." },
        { L"Status_Summary_Ready", L"DDC/CI driver active. Hardware brightness control enabled." },
        { L"Status_Summary_Offline", L"DDC/CI communication unavailable. Using fallback state." },
        { L"Status_Checking", L"Checking display hardware..." },
        { L"Sec_Appearance", L"Appearance" },
        { L"Opt_AppTheme", L"App Theme" },
        { L"Desc_AppTheme", L"Visual appearance of the settings window" },
        { L"Theme_FollowWindows", L"Follow Windows (app)" },
        { L"Theme_Dark", L"Dark" },
        { L"Theme_Light", L"Light" },
        { L"Opt_TransparencyEffects", L"Transparency Effects" },
        { L"Desc_TransparencyEffects", L"Enable Mica and Acrylic backdrop transparency across all app windows" },
        { L"Opt_Language", L"Language" },
        { L"Desc_Language", L"Select application display language. If the Windows language is unsupported, English (United States) is used." },
        { L"Lang_WindowsDefault", L"Follow Windows" },
        { L"Sec_StartupDisplays", L"Startup & Displays" },
        { L"Opt_StartWithWindows", L"Start with Windows" },
        { L"Desc_StartWithWindows", L"Launch ClickBrightness automatically on system logon" },
        { L"Desc_StartWithWindows_NotInstalled", L"Launch ClickBrightness automatically on system logon (Available when installed in Program Files)" },
        { L"Opt_PhysicalMonitors", L"Physical Monitors" },
        { L"Btn_Redetect", L"Re-detect" },
        { L"Opt_MonitorsToControl", L"Monitors to Control" },
        { L"Desc_MonitorsToControl", L"Select which monitors have their brightness adjusted" },
        { L"Monitors_All", L"All Monitors" },
        { L"Monitors_NoneDetected", L"No DDC/CI monitors detected yet." },
        { L"Monitors_DisplayFormat", L"Display %s: %s" },
        { L"PrimaryBadge", L"Primary" },
        { L"Shortcuts_Title", L"Keyboard Shortcuts" },
        { L"Sec_ShortcutsAdjust", L"Brightness Stepping Shortcuts" },
        { L"Opt_HotkeyAdjust", L"Increase & Decrease Shortcuts" },
        { L"Desc_HotkeyAdjust", L"Enable global keyboard shortcuts to raise and lower brightness" },
        { L"Opt_KeyUp", L"Increase Brightness" },
        { L"Desc_KeyUp", L"Global shortcut to raise hardware brightness" },
        { L"Opt_KeyDown", L"Decrease Brightness" },
        { L"Desc_KeyDown", L"Global shortcut to lower hardware brightness" },
        { L"Opt_Step", L"Brightness Step" },
        { L"Desc_Step", L"Percentage adjustment per hotkey press" },
        { L"Opt_HotkeyAdjustShowOsd", L"Show OSD Banner" },
        { L"Desc_HotkeyAdjustShowOsd", L"Display the on-screen brightness banner when pressing stepping hotkeys" },
        { L"Sec_ShortcutsPresets", L"Preset Shortcuts" },
        { L"Opt_HotkeyPresets", L"Preset Shortcuts" },
        { L"Desc_HotkeyPresets", L"Enable global keyboard shortcuts to jump to brightness presets" },
        { L"Opt_KeyPreset1", L"Preset 1 Shortcut" },
        { L"Desc_KeyPreset1", L"Global shortcut to set brightness to Preset 1" },
        { L"Opt_Preset1Brightness", L"Preset 1 Brightness (%)" },
        { L"Desc_Preset1Brightness", L"Target brightness level applied by the Preset 1 shortcut" },
        { L"Opt_KeyPreset2", L"Preset 2 Shortcut" },
        { L"Desc_KeyPreset2", L"Global shortcut to set brightness to Preset 2" },
        { L"Opt_Preset2Brightness", L"Preset 2 Brightness (%)" },
        { L"Desc_Preset2Brightness", L"Target brightness level applied by the Preset 2 shortcut" },
        { L"Opt_HotkeyPresetsShowOsd", L"Show OSD Banner" },
        { L"Desc_HotkeyPresetsShowOsd", L"Display the on-screen brightness banner when switching presets" },
        { L"Sec_ShortcutsPresetToggle", L"Preset Toggle Shortcut" },
        { L"Opt_HotkeyPresetToggle", L"Preset Toggle Shortcut" },
        { L"Desc_HotkeyPresetToggle", L"Toggle between two brightness presets using a single shortcut" },
        { L"Opt_KeyPresetToggle", L"Toggle Shortcut Key" },
        { L"Desc_KeyPresetToggle", L"Press shortcut to toggle between Preset 1 and Preset 2" },
        { L"Opt_PresetToggle1Brightness", L"Preset 1 Brightness (%)" },
        { L"Desc_PresetToggle1Brightness", L"First target brightness level toggled by the shortcut" },
        { L"Opt_PresetToggle2Brightness", L"Preset 2 Brightness (%)" },
        { L"Desc_PresetToggle2Brightness", L"Second target brightness level toggled by the shortcut" },
        { L"Opt_HotkeyPresetToggleShowOsd", L"Show OSD Banner" },
        { L"Desc_HotkeyPresetToggleShowOsd", L"Display the on-screen brightness banner when toggling presets" },
        { L"Opt_ResetShortcuts", L"Reset Shortcuts" },
        { L"Desc_ResetShortcuts", L"Restore hotkeys to defaults (F14, F13, F15, F16, F17)" },
        { L"Btn_ResetHotkeys", L"Reset Hotkeys" },
        { L"PressAnyKey", L"Press any key..." },
        { L"Sec_TimingDelay", L"Timing & Hardware Delay" },
        { L"Opt_DelayEnabled", L"Delayed Brightness Change" },
        { L"Desc_DelayEnabled", L"Debounce rapid key presses before hardware adjustments" },
        { L"Opt_WaitTime", L"Wait Time (ms)" },
        { L"Desc_WaitTime", L"Hardware debounce delay before updating monitor" },
        { L"Opt_HoldSpeed", L"Hold Speed (ms)" },
        { L"Desc_HoldSpeed", L"Key hold interval for smooth stepping (higher = slower)" },
        { L"OSD_Title", L"On-Screen Display (OSD)" },
        { L"OverlayTray_Title", L"On-Screen Display (OSD)" },
        { L"Opt_OverlayTheme", L"OSD Banner Theme" },
        { L"Desc_OverlayTheme", L"Visual appearance and color theme of the brightness overlay banner" },
        { L"Theme_FollowWindowsSystem", L"Follow Windows (system)" },
        { L"Sec_Timing", L"Timing" },
        { L"Opt_OverlayTimeout", L"Hide Time (ms)" },
        { L"Desc_OverlayTimeout", L"Duration the brightness overlay stays visible after adjustments" },
        { L"Sec_OverlayAccent", L"Accent Color" },
        { L"Opt_OverlayUseWindowsAccent", L"Use Windows Accent Color" },
        { L"Desc_OverlayUseWindowsAccent", L"Match the on-screen display indicator bar to Windows accent" },
        { L"Opt_OverlayCustomAccent", L"Custom Accent Color" },
        { L"Desc_OverlayCustomAccent", L"Fill color for OSD indicator bar (configured when Windows accent is off)" },
        { L"Tray_Title", L"System Tray Icon" },
        { L"Sec_TrayAppearance", L"System Tray Icon" },
        { L"Opt_TrayShowPercentage", L"Tray Icon" },
        { L"Desc_TrayShowPercentage", L"Choose how the brightness percentage is displayed on the system tray icon" },
        { L"Opt_TrayPctFollowWindows", L"Follow Windows" },
        { L"Opt_TrayPctColored", L"Colored" },
        { L"Opt_TrayPctDisabled", L"Disabled" },
        { L"Opt_TrayIconColor", L"Icon Color" },
        { L"Desc_TrayIconColor", L"Custom color for the system tray percentage text" },
        { L"Opt_TrayIconColorLight", L"Light Mode Color" },
        { L"Opt_TrayIconDualColor", L"Separate Dark & Light Colors" },
        { L"Desc_TrayIconDualColor", L"Use a different tray icon color when Windows is in dark mode" },
        { L"Opt_TrayIconColorDark", L"Dark Mode Color" },
        { L"Desc_TrayIconColorDark", L"Custom color for the system tray percentage text in dark mode" },
        { L"Sec_TrayTypography", L"System Tray Icon Typography" },
        { L"Opt_TrayFont", L"Tray Icon Font" },
        { L"Desc_TrayFont", L"Font family used to render the taskbar tray brightness number" },
        { L"Opt_TrayFontWeight", L"Font Weight" },
        { L"Desc_TrayFontWeight", L"Stroke thickness from 100 to 900 (e.g. 300 Light, 350 Semilight, 400 Regular)" },
        { L"Opt_TrayFontSize", L"Tray Font Size (%)" },
        { L"Desc_TrayFontSize", L"Relative scale for 1-2 digits (1%-99%)" },
        { L"Opt_TrayFontSize100", L"100% Width (%)" },
        { L"Desc_TrayFontSize100", L"Condensed horizontal scale for 3 digits (100%)" },
        { L"Opt_TrayYOffset", L"Vertical Offset (px)" },
        { L"Desc_TrayYOffset", L"Baseline vertical adjustment (e.g. 0.0, -0.5, 1.5)" },
        { L"Sec_TrayWheel", L"System Tray Mouse Wheel Control" },
        { L"Opt_TrayWheel", L"Scroll on Tray Icon" },
        { L"Desc_TrayWheel", L"Adjust brightness by scrolling the mouse wheel directly over the notification tray icon" },
        { L"Opt_TrayWheelStep", L"Brightness Step" },
        { L"Desc_TrayWheelStep", L"Percentage adjustment per mouse wheel notch" },
        { L"Opt_TrayWheelShowOsd", L"Show OSD When Scrolling" },
        { L"Desc_TrayWheelShowOsd", L"Display the on-screen brightness banner while scrolling over the tray icon" },
        { L"Opt_TrayWheelDelay", L"Scrolling Hardware Delay" },
        { L"Desc_TrayWheelDelay", L"Debounce rapid mouse wheel scrolling before updating monitor hardware" },
        { L"Opt_TrayWheelWaitTime", L"Scrolling Wait Time (ms)" },
        { L"Desc_TrayWheelWaitTime", L"Delay in milliseconds before sending hardware brightness commands to the display" },
        { L"Sec_TrayMiddleClick", L"System Tray Middle-Click Action" },
        { L"Opt_TrayMiddleClick", L"Middle-Click Quick Action" },
        { L"Desc_TrayMiddleClick", L"Set a specific brightness level when clicking the middle mouse button on the tray icon" },
        { L"Opt_TrayMiddleClickShowOsd", L"Show OSD On Middle-Click" },
        { L"Desc_TrayMiddleClickShowOsd", L"Display the on-screen brightness banner when middle-clicking the tray icon" },
        { L"Opt_TrayMiddleClickBrightness", L"Target Brightness (%)" },
        { L"Desc_TrayMiddleClickBrightness", L"Brightness percentage applied when middle-clicking the tray icon" },
        { L"Flyout_Title", L"Flyout Slider" },
        { L"Opt_FlyoutScrollEnabled", L"Slider Scrolling" },
        { L"Desc_FlyoutScrollEnabled", L"Adjust brightness by scrolling the mouse wheel over the flyout slider" },
        { L"Opt_FlyoutScrollStep", L"Scrolling Brightness Step" },
        { L"Desc_FlyoutScrollStep", L"Percentage adjustment per mouse wheel notch on the flyout slider" },
        { L"Opt_FlyoutKeyboardEnabled", L"Keyboard Controls" },
        { L"Desc_FlyoutKeyboardEnabled", L"Adjust brightness using keyboard arrow keys, Page Up/Down, and Home/End" },
        { L"Sec_FlyoutTimingDelay", L"Timing & Hardware Delay" },
        { L"Opt_FlyoutScrollDelay", L"Slider Scrolling Delay" },
        { L"Desc_FlyoutScrollDelay", L"Debounce rapid mouse wheel scrolling before hardware adjustments" },
        { L"Opt_FlyoutScrollWaitTime", L"Scrolling Wait Time (ms)" },
        { L"Desc_FlyoutScrollWaitTime", L"Hardware debounce delay after scrolling before updating monitor" },
        { L"Opt_FlyoutButtonDelay", L"Button Clicking Delay" },
        { L"Desc_FlyoutButtonDelay", L"Debounce rapid preset button clicks before hardware adjustments" },
        { L"Opt_FlyoutButtonWaitTime", L"Button Wait Time (ms)" },
        { L"Desc_FlyoutButtonWaitTime", L"Hardware debounce delay after clicking buttons before updating monitor" },
        { L"Sec_FlyoutAccent", L"Accent Color" },
        { L"Opt_FlyoutUseWindowsAccent", L"Use Windows Accent Color" },
        { L"Desc_FlyoutUseWindowsAccent", L"Apply system accent color to the flyout slider filled track" },
        { L"Opt_FlyoutCustomAccent", L"Custom Accent Color" },
        { L"Desc_FlyoutCustomAccent", L"Fill color for flyout slider track (configured when Windows accent is off)" },
        { L"Flyout_Brightness", L"Brightness %d%%" },
        { L"Tray_Options", L"Options..." },
        { L"Tray_About", L"ClickBrightness v2.0.0" },
        { L"Tray_Exit", L"Exit" },
        { L"Tray_Tooltip", L"Brightness: %d%%" },
        { L"Shortcuts_Subtitle", L"Global hotkeys for brightness stepping and preset jumping" },
        { L"OSD_Subtitle", L"Visual overlay popup for real-time brightness adjustments" },
        { L"Tray_Subtitle", L"Taskbar indicator, live percentage typography, and mouse actions" },
        { L"FlyoutSlider_Subtitle", L"Hardware brightness slider popup with mouse wheel control" },
        { L"About_Title", L"About ClickBrightness" },
        { L"About_Subtitle", L"Lightweight, modern Windows 11 DDC/CI monitor brightness utility" },
        { L"About_AppInfo", L"ClickBrightness" },
        { L"About_Version", L"Version 2.0.0 (x64 Release)" },
        { L"About_AppType", L"Native WinUI 3 Desktop Application" },
        { L"About_Copyright", L"Copyright (C) 2026 andris155" },
        { L"About_DescTitle", L"Description" },
        { L"About_Description", L"A modern, lightweight Windows 11 utility for controlling hardware monitor brightness via DDC/CI (VCP code 0x10). Supports on-screen display (OSD), system tray live indicator with mouse wheel stepping, and a smooth flyout slider." },
        { L"About_LicenseTitle", L"License" },
        { L"About_License", L"Free and Open Source under the MIT License." },
        { L"Common_On", L"On" },
        { L"Common_Off", L"Off" }
    };
}
