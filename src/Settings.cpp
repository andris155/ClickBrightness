#include "Settings.h"
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <shlobj.h>
#include <filesystem>

#pragma comment(lib, "shell32.lib")

AppSettings g_Settings;

namespace {
struct IniFile {
    std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>> sections;

    static std::wstring ToLower(std::wstring s) {
        for (wchar_t& ch : s) {
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
        }
        return s;
    }

    static void Trim(std::wstring& s) {
        size_t start = 0;
        while (start < s.size() && (s[start] == L' ' || s[start] == L'\t' || s[start] == L'\r' || s[start] == L'\n')) start++;
        size_t end = s.size();
        while (end > start && (s[end - 1] == L' ' || s[end - 1] == L'\t' || s[end - 1] == L'\r' || s[end - 1] == L'\n')) end--;
        if (start > 0 || end < s.size()) s = s.substr(start, end - start);
    }

    bool LoadFromFile(const std::wstring& path) {
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > 4 * 1024 * 1024) {
            CloseHandle(hFile);
            return fileSize.QuadPart == 0;
        }
        std::string raw(static_cast<size_t>(fileSize.QuadPart), '\0');
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hFile, raw.data(), static_cast<DWORD>(raw.size()), &bytesRead, NULL);
        CloseHandle(hFile);
        if (!ok || bytesRead == 0) return false;
        raw.resize(bytesRead);

        std::wstring text;
        if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE) {
            size_t wcharCount = (raw.size() - 2) / sizeof(wchar_t);
            text.resize(wcharCount);
            if (wcharCount > 0) memcpy(text.data(), raw.data() + 2, wcharCount * sizeof(wchar_t));
        } else {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
            if (wlen > 0) {
                text.resize(wlen);
                MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), text.data(), wlen);
                if (!text.empty() && text[0] == L'\xFEFF') text.erase(0, 1);
            }
        }

        std::wstring currentSec;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t eol = text.find(L'\n', pos);
            if (eol == std::wstring::npos) eol = text.size();
            std::wstring line = text.substr(pos, eol - pos);
            pos = eol + 1;
            Trim(line);
            if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
            if (line.front() == L'[' && line.back() == L']') {
                currentSec = ToLower(line.substr(1, line.size() - 2));
                Trim(currentSec);
                continue;
            }
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos && !currentSec.empty()) {
                std::wstring key = line.substr(0, eq);
                std::wstring val = line.substr(eq + 1);
                Trim(key);
                Trim(val);
                if (!key.empty()) {
                    sections[currentSec][ToLower(std::move(key))] = std::move(val);
                }
            }
        }
        return true;
    }

    int GetInt(const wchar_t* sec, const wchar_t* key, int defVal) const {
        auto sit = sections.find(ToLower(sec));
        if (sit == sections.end()) return defVal;
        auto kit = sit->second.find(ToLower(key));
        if (kit == sit->second.end() || kit->second.empty()) return defVal;
        wchar_t* endPtr = nullptr;
        long val = wcstol(kit->second.c_str(), &endPtr, 10);
        return (endPtr != kit->second.c_str()) ? static_cast<int>(val) : defVal;
    }

    std::wstring GetString(const wchar_t* sec, const wchar_t* key, const wchar_t* defVal) const {
        auto sit = sections.find(ToLower(sec));
        if (sit == sections.end()) return defVal ? defVal : L"";
        auto kit = sit->second.find(ToLower(key));
        if (kit == sit->second.end()) return defVal ? defVal : L"";
        return kit->second;
    }
};
} // namespace

const std::wstring& AppSettings::GetIniFilePath() {
    static std::wstring s_normalIniPath;
    static std::wstring s_simIniPath;
    if (s_normalIniPath.empty()) {
        // If running in portable mode (settings.ini, simulated_settings.ini, or portable.txt exists next to exe), keep settings app-local
        wchar_t exePath[MAX_PATH] = { 0 };
        DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
            std::filesystem::path localIni = exeDir / L"settings.ini";
            std::filesystem::path localSimIni = exeDir / L"simulated_settings.ini";
            std::filesystem::path portableMarker = exeDir / L"portable.txt";
            if (std::filesystem::exists(localIni) || std::filesystem::exists(localSimIni) || std::filesystem::exists(portableMarker)) {
                s_normalIniPath = localIni.wstring();
                s_simIniPath = localSimIni.wstring();
            }
        }

        if (s_normalIniPath.empty()) {
            PWSTR localAppData = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &localAppData))) {
                std::filesystem::path dir = std::filesystem::path(localAppData) / L"ClickBrightness";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                s_normalIniPath = (dir / L"settings.ini").wstring();
                s_simIniPath = (dir / L"simulated_settings.ini").wstring();
                CoTaskMemFree(localAppData);
            }
        }

        if (s_normalIniPath.empty()) {
            wchar_t exePath2[MAX_PATH];
            DWORD len2 = GetModuleFileNameW(NULL, exePath2, MAX_PATH);
            if (len2 > 0 && len2 < MAX_PATH) {
                exePath2[len2] = L'\0';
                wchar_t* lastSlash = wcsrchr(exePath2, L'\\');
                if (lastSlash) {
                    *(lastSlash + 1) = L'\0';
                    s_normalIniPath = std::wstring(exePath2) + L"settings.ini";
                    s_simIniPath = std::wstring(exePath2) + L"simulated_settings.ini";
                }
            }
            if (s_normalIniPath.empty()) {
                s_normalIniPath = L"settings.ini";
                s_simIniPath = L"simulated_settings.ini";
            }
        }
    }
    return g_Settings.IsSimulatedMode() ? s_simIniPath : s_normalIniPath;
}

void AppSettings::ResetToDefaults() {
    int savedCachedBrightness = cachedBrightness;
    bool savedStartWithWindows = startWithWindows;
    int savedSimCount = simulateMonitorsCount;
    int savedCliSimOverride = cliSimulateMonitorsOverride;
    std::wstring savedMainMonitorId = mainMonitorId;
    auto savedConfigs = monitorConfigs;

    *this = AppSettings();

    cachedBrightness = savedCachedBrightness;
    startWithWindows = savedStartWithWindows;
    simulateMonitorsCount = savedSimCount;
    cliSimulateMonitorsOverride = savedCliSimOverride;
    mainMonitorId = savedMainMonitorId;
    monitorConfigs = std::move(savedConfigs);

    for (auto& [monId, cfg] : monitorConfigs) {
        MonitorControlConfig fresh;
        fresh.customName = cfg.customName;
        fresh.cachedBrightness = cfg.cachedBrightness;
        CopyCustomizationToMonitor(fresh);
        if (!mainMonitorId.empty() && _wcsicmp(monId.c_str(), mainMonitorId.c_str()) != 0) {
            ClearHotkeys(fresh);
            fresh.trayPercentageMode = 3;
        }
        cfg = std::move(fresh);
    }
}

void AppSettings::Load() {
    std::wstring ini = GetIniFilePath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ResetToDefaults();
        startWithWindows = IsStartWithWindowsEnabled();
        Save();
        return;
    }

    IniFile iniFile;
    if (!iniFile.LoadFromFile(ini)) {
        ResetToDefaults();
        startWithWindows = IsStartWithWindowsEnabled();
        Save();
        return;
    }

    hotkeyAdjustEnabled = (iniFile.GetInt(L"Hotkeys", L"AdjustEnabled", 0) != 0);
    hotkeyAdjustShowOsd = (iniFile.GetInt(L"Hotkeys", L"AdjustShowOsd", 1) != 0);
    hotkeyUpKey = iniFile.GetInt(L"Hotkeys", L"KeyUp", VK_F14);
    hotkeyUpMods = iniFile.GetInt(L"Hotkeys", L"ModsUp", 0);

    hotkeyDownKey = iniFile.GetInt(L"Hotkeys", L"KeyDown", VK_F13);
    hotkeyDownMods = iniFile.GetInt(L"Hotkeys", L"ModsDown", 0);

    hotkeyPresetsEnabled = (iniFile.GetInt(L"Hotkeys", L"PresetsEnabled", 0) != 0);
    hotkeyPresetsShowOsd = (iniFile.GetInt(L"Hotkeys", L"PresetsShowOsd", 1) != 0);
    hotkeyPreset1Key = iniFile.GetInt(L"Hotkeys", L"KeyPreset1", VK_F15);
    hotkeyPreset1Mods = iniFile.GetInt(L"Hotkeys", L"ModsPreset1", 0);
    hotkeyPreset1Brightness = (std::clamp)(iniFile.GetInt(L"Hotkeys", L"Preset1Brightness", 30), 0, 100);

    hotkeyPreset2Key = iniFile.GetInt(L"Hotkeys", L"KeyPreset2", VK_F16);
    hotkeyPreset2Mods = iniFile.GetInt(L"Hotkeys", L"ModsPreset2", 0);
    hotkeyPreset2Brightness = (std::clamp)(iniFile.GetInt(L"Hotkeys", L"Preset2Brightness", 80), 0, 100);

    hotkeyPresetToggleEnabled = (iniFile.GetInt(L"Hotkeys", L"PresetToggleEnabled", 0) != 0);
    hotkeyPresetToggleShowOsd = (iniFile.GetInt(L"Hotkeys", L"PresetToggleShowOsd", 1) != 0);
    hotkeyPresetToggleKey = iniFile.GetInt(L"Hotkeys", L"KeyPresetToggle", VK_F17);
    hotkeyPresetToggleMods = iniFile.GetInt(L"Hotkeys", L"ModsPresetToggle", 0);
    hotkeyPresetToggle1Brightness = (std::clamp)(iniFile.GetInt(L"Hotkeys", L"PresetToggle1Brightness", 30), 0, 100);
    hotkeyPresetToggle2Brightness = (std::clamp)(iniFile.GetInt(L"Hotkeys", L"PresetToggle2Brightness", 80), 0, 100);

    hotkeyStep = iniFile.GetInt(L"Hotkeys", L"Step", 5);
    if (hotkeyStep < 1 || hotkeyStep > 50) hotkeyStep = 5;

    hotkeyRepeatIntervalMs = iniFile.GetInt(L"Hotkeys", L"RepeatIntervalMs", 90);
    if (hotkeyRepeatIntervalMs < 20 || hotkeyRepeatIntervalMs > 500) hotkeyRepeatIntervalMs = 90;

    hotkeyDelayEnabled = (iniFile.GetInt(L"Delay", L"Enabled", 1) != 0);
    hotkeyDelayMs = iniFile.GetInt(L"Delay", L"WaitTimeMs", 1000);
    if (hotkeyDelayMs < 100 || hotkeyDelayMs > 3000) hotkeyDelayMs = 1000;

    int pctMode = iniFile.GetInt(L"Tray", L"PercentageMode", 0);
    trayPercentageMode = (pctMode >= 0 && pctMode <= 4) ? pctMode : 0;

    trayIconColor = ParseColor(iniFile.GetString(L"Tray", L"IconColor", L"#0078D4"), RGB(0, 120, 212));
    trayIconUseDualColors = (iniFile.GetInt(L"Tray", L"UseDualColors", 0) != 0);
    trayIconColorDark = ParseColor(iniFile.GetString(L"Tray", L"IconColorDark", L"#60CDFF"), RGB(96, 205, 255));

    trayIconUseDualCustomIcons = (iniFile.GetInt(L"Tray", L"UseDualCustomIcons", 0) != 0);
    trayCustomIcon = iniFile.GetString(L"Tray", L"CustomIcon", L"preset:sun");
    if (trayCustomIcon.empty()) trayCustomIcon = L"preset:sun";
    trayCustomIconDark = iniFile.GetString(L"Tray", L"CustomIconDark", L"preset:sun-blue");
    if (trayCustomIconDark.empty()) trayCustomIconDark = L"preset:sun-blue";

    trayFont = iniFile.GetString(L"Tray", L"Font", L"Segoe UI Variable Text");
    if (trayFont.empty()) trayFont = L"Segoe UI Variable Text";

    int loadedWeight = iniFile.GetInt(L"Tray", L"FontWeight", 375);
    trayFontWeight = (loadedWeight >= 100 && loadedWeight <= 900) ? loadedWeight : 375;

    trayFontSize = iniFile.GetInt(L"Tray", L"FontSize", 102);
    if (trayFontSize < 40 || trayFontSize > 150) trayFontSize = 102;

    trayFontSize100 = iniFile.GetInt(L"Tray", L"FontSize100", 63);
    if (trayFontSize100 < 30 || trayFontSize100 > 130) trayFontSize100 = 63;

    trayYOffset = ParseYOffset(iniFile.GetString(L"Tray", L"YOffset", L"-0.5"), &trayYOffsetUseComma);

    trayWheelEnabled = (iniFile.GetInt(L"Tray", L"WheelEnabled", 0) != 0);
    trayWheelStep = iniFile.GetInt(L"Tray", L"WheelStep", 5);
    if (trayWheelStep < 1 || trayWheelStep > 50) trayWheelStep = 5;
    trayWheelShowOsd = (iniFile.GetInt(L"Tray", L"WheelShowOsd", 1) != 0);
    trayWheelDelayEnabled = (iniFile.GetInt(L"Tray", L"WheelDelayEnabled", 1) != 0);
    trayWheelDelayMs = iniFile.GetInt(L"Tray", L"WheelWaitTimeMs", 1000);
    if (trayWheelDelayMs < 100 || trayWheelDelayMs > 3000) trayWheelDelayMs = 1000;

    trayMiddleClickEnabled = (iniFile.GetInt(L"Tray", L"MiddleClickEnabled", 0) != 0);
    trayMiddleClickShowOsd = (iniFile.GetInt(L"Tray", L"MiddleClickShowOsd", 1) != 0);
    trayMiddleClickBrightness = (std::clamp)(iniFile.GetInt(L"Tray", L"MiddleClickBrightness", 50), 0, 100);

    appTheme = iniFile.GetInt(L"General", L"AppTheme", 0);
    if (appTheme < 0 || appTheme > 2) appTheme = 0;

    transparencyEnabled = (iniFile.GetInt(L"General", L"TransparencyEnabled", 1) != 0);

    language = iniFile.GetString(L"General", L"Language", L"default");
    if (language.empty()) language = L"default";

    overlayTheme = iniFile.GetInt(L"Overlay", L"Theme", 0);
    if (overlayTheme < 0 || overlayTheme > 2) overlayTheme = 0;

    overlayTimeoutMs = (std::clamp)(iniFile.GetInt(L"Overlay", L"TimeoutMs", 2000), 500, 10000);
    osdSuppressOnDnd = (iniFile.GetInt(L"Overlay", L"SuppressOnDnd", 0) != 0);

    overlayUseWindowsAccent = (iniFile.GetInt(L"Overlay", L"UseWindowsAccent", 1) != 0);
    overlayAccentColor = ParseColor(iniFile.GetString(L"Overlay", L"AccentColor", L"#0078D4"), RGB(0, 120, 212));
    overlayUseDualColors = (iniFile.GetInt(L"Overlay", L"UseDualColors", 0) != 0);
    overlayAccentColorDark = ParseColor(iniFile.GetString(L"Overlay", L"AccentColorDark", L"#60CDFF"), RGB(96, 205, 255));

    flyoutUseWindowsAccent = (iniFile.GetInt(L"Flyout", L"UseWindowsAccent", 1) != 0);
    flyoutAccentColor = ParseColor(iniFile.GetString(L"Flyout", L"AccentColor", L"#0078D4"), RGB(0, 120, 212));
    flyoutUseDualColors = (iniFile.GetInt(L"Flyout", L"UseDualColors", 0) != 0);
    flyoutAccentColorDark = ParseColor(iniFile.GetString(L"Flyout", L"AccentColorDark", L"#60CDFF"), RGB(96, 205, 255));

    flyoutScrollEnabled = (iniFile.GetInt(L"Flyout", L"ScrollEnabled", 1) != 0);
    flyoutScrollStep = iniFile.GetInt(L"Flyout", L"ScrollStep", 5);
    if (flyoutScrollStep < 1 || flyoutScrollStep > 50) flyoutScrollStep = 5;

    flyoutKeyboardEnabled = (iniFile.GetInt(L"Flyout", L"KeyboardEnabled", 0) != 0);

    flyoutScrollDelayEnabled = (iniFile.GetInt(L"Flyout", L"ScrollDelayEnabled", 1) != 0);
    flyoutScrollDelayMs = iniFile.GetInt(L"Flyout", L"ScrollWaitTimeMs", 1000);
    if (flyoutScrollDelayMs < 100 || flyoutScrollDelayMs > 3000) flyoutScrollDelayMs = 1000;

    flyoutButtonDelayEnabled = (iniFile.GetInt(L"Flyout", L"ButtonDelayEnabled", 0) != 0);
    flyoutButtonDelayMs = iniFile.GetInt(L"Flyout", L"ButtonWaitTimeMs", 500);
    if (flyoutButtonDelayMs < 100 || flyoutButtonDelayMs > 3000) flyoutButtonDelayMs = 500;

    cachedBrightness = (std::clamp)(iniFile.GetInt(L"State", L"Brightness", 45), 0, 100);

    simulateMonitorsCount = (std::clamp)(iniFile.GetInt(L"Debug", L"SimulateMonitors", 0), 0, 16);
    static int s_iniForcedSimCount = 0;
    if (s_iniForcedSimCount > 0) {
        simulateMonitorsCount = s_iniForcedSimCount;
    } else if (cliSimulateMonitorsOverride < 0 && simulateMonitorsCount > 0) {
        s_iniForcedSimCount = simulateMonitorsCount;
        Load();
        return;
    }

    // Multi-Monitor Sync & Separate Configs
    mainMonitorId = iniFile.GetString(L"Monitors", L"MainId", L"");
    syncExternalWithLaptopFnKeys = (iniFile.GetInt(L"Monitors", L"SyncLaptopFnKeys", 0) != 0);
    showSeparateInMultiFlyout = (iniFile.GetInt(L"Monitors", L"ShowSeparateInMultiFlyout", 1) != 0);
    disableTurnedOffControl = (iniFile.GetInt(L"Monitors", L"DisableTurnedOffControl", 0) != 0);

    monitorConfigs.clear();
    std::wstring cfgIdsStr = iniFile.GetString(L"Monitors", L"ConfiguredIds", L"");
    size_t pos = 0;
    while (pos < cfgIdsStr.length()) {
        size_t end = cfgIdsStr.find(L';', pos);
        if (end == std::wstring::npos) end = cfgIdsStr.length();
        std::wstring monId = cfgIdsStr.substr(pos, end - pos);
        pos = end + 1;
        if (monId.empty()) continue;

        std::wstring secStr = L"Monitor_" + monId;
        const wchar_t* sec = secStr.c_str();
        MonitorControlConfig cfg;
        CopyCustomizationToMonitor(cfg);
        if (!mainMonitorId.empty() && _wcsicmp(monId.c_str(), mainMonitorId.c_str()) != 0) {
            ClearHotkeys(cfg);
        }
        cfg.enabled = (iniFile.GetInt(sec, L"Enabled", 1) != 0);
        cfg.customName = SanitizeMonitorName(iniFile.GetString(sec, L"CustomName", L""));
        cfg.controlMode = iniFile.GetInt(sec, L"ControlMode", 0);
        if (cfg.controlMode < 0 || cfg.controlMode > 1) cfg.controlMode = 0;
        cfg.offset = (std::clamp)(iniFile.GetInt(sec, L"Offset", 0), -100, 100);
        cfg.syncOverscrollMax = (iniFile.GetInt(sec, L"SyncOverscrollMax", 1) != 0);
        cfg.syncOverscrollMin = (iniFile.GetInt(sec, L"SyncOverscrollMin", 1) != 0);

        // Shortcuts
        cfg.hotkeyAdjustEnabled = (iniFile.GetInt(sec, L"HotkeyAdjustEnabled", cfg.hotkeyAdjustEnabled ? 1 : 0) != 0);
        cfg.hotkeyAdjustShowOsd = (iniFile.GetInt(sec, L"HotkeyAdjustShowOsd", cfg.hotkeyAdjustShowOsd ? 1 : 0) != 0);
        cfg.hotkeyUpMods = iniFile.GetInt(sec, L"HotkeyUpMods", cfg.hotkeyUpMods);
        cfg.hotkeyUpKey = iniFile.GetInt(sec, L"HotkeyUpKey", cfg.hotkeyUpKey);
        cfg.hotkeyDownMods = iniFile.GetInt(sec, L"HotkeyDownMods", cfg.hotkeyDownMods);
        cfg.hotkeyDownKey = iniFile.GetInt(sec, L"HotkeyDownKey", cfg.hotkeyDownKey);
        cfg.hotkeyPresetsEnabled = (iniFile.GetInt(sec, L"HotkeyPresetsEnabled", cfg.hotkeyPresetsEnabled ? 1 : 0) != 0);
        cfg.hotkeyPresetsShowOsd = (iniFile.GetInt(sec, L"HotkeyPresetsShowOsd", cfg.hotkeyPresetsShowOsd ? 1 : 0) != 0);
        cfg.hotkeyPreset1Mods = iniFile.GetInt(sec, L"HotkeyPreset1Mods", cfg.hotkeyPreset1Mods);
        cfg.hotkeyPreset1Key = iniFile.GetInt(sec, L"HotkeyPreset1Key", cfg.hotkeyPreset1Key);
        cfg.hotkeyPreset1Brightness = (std::clamp)(iniFile.GetInt(sec, L"HotkeyPreset1Brightness", cfg.hotkeyPreset1Brightness), 0, 100);
        cfg.hotkeyPreset2Mods = iniFile.GetInt(sec, L"HotkeyPreset2Mods", cfg.hotkeyPreset2Mods);
        cfg.hotkeyPreset2Key = iniFile.GetInt(sec, L"HotkeyPreset2Key", cfg.hotkeyPreset2Key);
        cfg.hotkeyPreset2Brightness = (std::clamp)(iniFile.GetInt(sec, L"HotkeyPreset2Brightness", cfg.hotkeyPreset2Brightness), 0, 100);
        cfg.hotkeyPresetToggleEnabled = (iniFile.GetInt(sec, L"HotkeyPresetToggleEnabled", cfg.hotkeyPresetToggleEnabled ? 1 : 0) != 0);
        cfg.hotkeyPresetToggleShowOsd = (iniFile.GetInt(sec, L"HotkeyPresetToggleShowOsd", cfg.hotkeyPresetToggleShowOsd ? 1 : 0) != 0);
        cfg.hotkeyPresetToggleMods = iniFile.GetInt(sec, L"HotkeyPresetToggleMods", cfg.hotkeyPresetToggleMods);
        cfg.hotkeyPresetToggleKey = iniFile.GetInt(sec, L"HotkeyPresetToggleKey", cfg.hotkeyPresetToggleKey);
        cfg.hotkeyPresetToggle1Brightness = (std::clamp)(iniFile.GetInt(sec, L"HotkeyPresetToggle1Brightness", cfg.hotkeyPresetToggle1Brightness), 0, 100);
        cfg.hotkeyPresetToggle2Brightness = (std::clamp)(iniFile.GetInt(sec, L"HotkeyPresetToggle2Brightness", cfg.hotkeyPresetToggle2Brightness), 0, 100);
        cfg.hotkeyStep = (std::clamp)(iniFile.GetInt(sec, L"HotkeyStep", cfg.hotkeyStep), 1, 50);

        // OSD
        cfg.overlayUseWindowsAccent = (iniFile.GetInt(sec, L"OsdUseWindowsAccent", cfg.overlayUseWindowsAccent ? 1 : 0) != 0);
        cfg.overlayAccentColor = ParseColor(iniFile.GetString(sec, L"OsdAccentColor", FormatColor(cfg.overlayAccentColor).c_str()), cfg.overlayAccentColor);
        cfg.overlayUseDualColors = (iniFile.GetInt(sec, L"OsdUseDualColors", cfg.overlayUseDualColors ? 1 : 0) != 0);
        cfg.overlayAccentColorDark = ParseColor(iniFile.GetString(sec, L"OsdAccentColorDark", FormatColor(cfg.overlayAccentColorDark).c_str()), cfg.overlayAccentColorDark);

        // Tray
        bool isMainMon = (!mainMonitorId.empty() && _wcsicmp(monId.c_str(), mainMonitorId.c_str()) == 0);
        if (isMainMon) {
            cfg.enabled = true;
            cfg.controlMode = 0;
        }
        int defaultPctMode = (!isMainMon && cfg.controlMode == 0) ? 3 : cfg.trayPercentageMode;
        cfg.trayPercentageMode = (std::clamp)(iniFile.GetInt(sec, L"TrayPercentageMode", defaultPctMode), 0, 4);
        if (isMainMon && cfg.trayPercentageMode == 3) {
            cfg.trayPercentageMode = 0;
        }
        cfg.trayIconColor = ParseColor(iniFile.GetString(sec, L"TrayIconColor", FormatColor(cfg.trayIconColor).c_str()), cfg.trayIconColor);
        cfg.trayIconUseDualColors = (iniFile.GetInt(sec, L"TrayUseDualColors", cfg.trayIconUseDualColors ? 1 : 0) != 0);
        cfg.trayIconColorDark = ParseColor(iniFile.GetString(sec, L"TrayIconColorDark", FormatColor(cfg.trayIconColorDark).c_str()), cfg.trayIconColorDark);
        cfg.trayIconUseDualCustomIcons = (iniFile.GetInt(sec, L"TrayUseDualCustomIcons", cfg.trayIconUseDualCustomIcons ? 1 : 0) != 0);
        std::wstring mIcon = iniFile.GetString(sec, L"TrayCustomIcon", cfg.trayCustomIcon.c_str());
        if (!mIcon.empty()) cfg.trayCustomIcon = mIcon;
        std::wstring mIconDark = iniFile.GetString(sec, L"TrayCustomIconDark", cfg.trayCustomIconDark.c_str());
        if (!mIconDark.empty()) cfg.trayCustomIconDark = mIconDark;
        std::wstring mFont = iniFile.GetString(sec, L"TrayFont", cfg.trayFont.c_str());
        if (!mFont.empty()) cfg.trayFont = mFont;
        cfg.trayFontWeight = (std::clamp)(iniFile.GetInt(sec, L"TrayFontWeight", cfg.trayFontWeight), 100, 900);
        cfg.trayFontSize = (std::clamp)(iniFile.GetInt(sec, L"TrayFontSize", cfg.trayFontSize), 40, 150);
        cfg.trayFontSize100 = (std::clamp)(iniFile.GetInt(sec, L"TrayFontSize100", cfg.trayFontSize100), 30, 130);
        bool uc = false;
        cfg.trayYOffset = ParseYOffset(iniFile.GetString(sec, L"TrayYOffset", FormatYOffset(cfg.trayYOffset, cfg.trayYOffsetUseComma).c_str()), &uc);
        cfg.trayYOffsetUseComma = uc;
        cfg.trayWheelEnabled = (iniFile.GetInt(sec, L"TrayWheelEnabled", cfg.trayWheelEnabled ? 1 : 0) != 0);
        cfg.trayWheelStep = (std::clamp)(iniFile.GetInt(sec, L"TrayWheelStep", cfg.trayWheelStep), 1, 50);
        cfg.trayWheelShowOsd = (iniFile.GetInt(sec, L"TrayWheelShowOsd", cfg.trayWheelShowOsd ? 1 : 0) != 0);
        cfg.trayMiddleClickEnabled = (iniFile.GetInt(sec, L"TrayMiddleClickEnabled", cfg.trayMiddleClickEnabled ? 1 : 0) != 0);
        cfg.trayMiddleClickShowOsd = (iniFile.GetInt(sec, L"TrayMiddleClickShowOsd", cfg.trayMiddleClickShowOsd ? 1 : 0) != 0);
        cfg.trayMiddleClickBrightness = (std::clamp)(iniFile.GetInt(sec, L"TrayMiddleClickBrightness", cfg.trayMiddleClickBrightness), 0, 100);

        // Flyout
        cfg.flyoutUseWindowsAccent = (iniFile.GetInt(sec, L"FlyoutUseWindowsAccent", cfg.flyoutUseWindowsAccent ? 1 : 0) != 0);
        cfg.flyoutAccentColor = ParseColor(iniFile.GetString(sec, L"FlyoutAccentColor", FormatColor(cfg.flyoutAccentColor).c_str()), cfg.flyoutAccentColor);
        cfg.flyoutUseDualColors = (iniFile.GetInt(sec, L"FlyoutUseDualColors", cfg.flyoutUseDualColors ? 1 : 0) != 0);
        cfg.flyoutAccentColorDark = ParseColor(iniFile.GetString(sec, L"FlyoutAccentColorDark", FormatColor(cfg.flyoutAccentColorDark).c_str()), cfg.flyoutAccentColorDark);
        cfg.flyoutScrollEnabled = (iniFile.GetInt(sec, L"FlyoutScrollEnabled", cfg.flyoutScrollEnabled ? 1 : 0) != 0);
        cfg.flyoutScrollStep = (std::clamp)(iniFile.GetInt(sec, L"FlyoutScrollStep", cfg.flyoutScrollStep), 1, 50);

        cfg.scheduleMode = (std::clamp)(iniFile.GetInt(sec, L"ScheduleMode", 0), 0, 2);
        cfg.scheduleSunSource = (std::clamp)(iniFile.GetInt(sec, L"ScheduleSunSource", 0), 0, 1);
        cfg.scheduleDayHour = (std::clamp)(iniFile.GetInt(sec, L"ScheduleDayHour", 7), 0, 23);
        cfg.scheduleDayMinute = (std::clamp)(iniFile.GetInt(sec, L"ScheduleDayMinute", 0), 0, 59);
        cfg.scheduleNightHour = (std::clamp)(iniFile.GetInt(sec, L"ScheduleNightHour", 20), 0, 23);
        cfg.scheduleNightMinute = (std::clamp)(iniFile.GetInt(sec, L"ScheduleNightMinute", 0), 0, 59);
        cfg.scheduleDayBrightness = (std::clamp)(iniFile.GetInt(sec, L"ScheduleDayBrightness", 80), 0, 100);
        cfg.scheduleNightBrightness = (std::clamp)(iniFile.GetInt(sec, L"ScheduleNightBrightness", 30), 0, 100);
        try { cfg.scheduleLatitude = std::stod(iniFile.GetString(sec, L"ScheduleLatitude", L"47.4979")); } catch (...) { cfg.scheduleLatitude = 47.4979; }
        try { cfg.scheduleLongitude = std::stod(iniFile.GetString(sec, L"ScheduleLongitude", L"19.0402")); } catch (...) { cfg.scheduleLongitude = 19.0402; }
        cfg.scheduleSunriseOffset = (std::clamp)(iniFile.GetInt(sec, L"ScheduleSunriseOffset", 0), -180, 180);
        cfg.scheduleSunsetOffset = (std::clamp)(iniFile.GetInt(sec, L"ScheduleSunsetOffset", 0), -180, 180);
        cfg.scheduleShowOsd = (iniFile.GetInt(sec, L"ScheduleShowOsd", 1) != 0);
        cfg.scheduleOsdOnlyOnChange = (iniFile.GetInt(sec, L"ScheduleOsdOnlyOnChange", 1) != 0);

        cfg.cachedBrightness = (std::clamp)(iniFile.GetInt(sec, L"CachedBrightness", cachedBrightness), 0, 100);
        monitorConfigs[monId] = cfg;
    }

    bool regStartWithWin = IsStartWithWindowsEnabled();
    int iniStartWithWin = iniFile.GetInt(L"General", L"StartWithWindows", -1);
    if (!IsInstalledInProgramFiles() || g_Settings.IsSimulatedMode()) {
        if (regStartWithWin) {
            startWithWindows = true;
        } else if (iniStartWithWin != -1) {
            startWithWindows = (iniStartWithWin == 1);
        } else {
            startWithWindows = false;
        }
    } else {
        if (iniStartWithWin == 1 && !regStartWithWin) {
            SetStartWithWindows(true);
            startWithWindows = true;
        } else if (iniStartWithWin == 0 && regStartWithWin) {
            SetStartWithWindows(false);
            startWithWindows = false;
        } else {
            startWithWindows = regStartWithWin;
        }
    }

    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Save();
    }
}

void AppSettings::Save() const {
    std::wstring ini = GetIniFilePath();

    std::wstring out;
    out.reserve(8192);
    out.push_back(L'\xFEFF'); // UTF-16LE BOM for GetPrivateProfile*W

    auto addSec = [&](const wchar_t* secName) {
        if (out.size() > 1) out += L"\r\n";
        out += L"[";
        out += secName;
        out += L"]\r\n";
    };
    auto addLine = [&](const wchar_t* key, const std::wstring& val) {
        out += key;
        out += L"=";
        out += val;
        out += L"\r\n";
    };

    addSec(L"General");
    addLine(L"StartWithWindows", startWithWindows ? L"1" : L"0");
    addLine(L"AppTheme", std::to_wstring(appTheme));
    addLine(L"TransparencyEnabled", transparencyEnabled ? L"1" : L"0");
    addLine(L"Language", language);

    std::wstring cfgIds;
    for (const auto& kv : monitorConfigs) {
        if (!cfgIds.empty()) cfgIds += L";";
        cfgIds += kv.first;
    }

    addSec(L"Monitors");
    addLine(L"MainId", mainMonitorId);
    addLine(L"SyncLaptopFnKeys", syncExternalWithLaptopFnKeys ? L"1" : L"0");
    addLine(L"ShowSeparateInMultiFlyout", showSeparateInMultiFlyout ? L"1" : L"0");
    addLine(L"DisableTurnedOffControl", disableTurnedOffControl ? L"1" : L"0");
    addLine(L"ConfiguredIds", cfgIds);

    addSec(L"Hotkeys");
    addLine(L"AdjustEnabled", hotkeyAdjustEnabled ? L"1" : L"0");
    addLine(L"AdjustShowOsd", hotkeyAdjustShowOsd ? L"1" : L"0");
    addLine(L"KeyUp", std::to_wstring(hotkeyUpKey));
    addLine(L"ModsUp", std::to_wstring(hotkeyUpMods));
    addLine(L"KeyDown", std::to_wstring(hotkeyDownKey));
    addLine(L"ModsDown", std::to_wstring(hotkeyDownMods));
    addLine(L"PresetsEnabled", hotkeyPresetsEnabled ? L"1" : L"0");
    addLine(L"PresetsShowOsd", hotkeyPresetsShowOsd ? L"1" : L"0");
    addLine(L"KeyPreset1", std::to_wstring(hotkeyPreset1Key));
    addLine(L"ModsPreset1", std::to_wstring(hotkeyPreset1Mods));
    addLine(L"Preset1Brightness", std::to_wstring(hotkeyPreset1Brightness));
    addLine(L"KeyPreset2", std::to_wstring(hotkeyPreset2Key));
    addLine(L"ModsPreset2", std::to_wstring(hotkeyPreset2Mods));
    addLine(L"Preset2Brightness", std::to_wstring(hotkeyPreset2Brightness));
    addLine(L"PresetToggleEnabled", hotkeyPresetToggleEnabled ? L"1" : L"0");
    addLine(L"PresetToggleShowOsd", hotkeyPresetToggleShowOsd ? L"1" : L"0");
    addLine(L"KeyPresetToggle", std::to_wstring(hotkeyPresetToggleKey));
    addLine(L"ModsPresetToggle", std::to_wstring(hotkeyPresetToggleMods));
    addLine(L"PresetToggle1Brightness", std::to_wstring(hotkeyPresetToggle1Brightness));
    addLine(L"PresetToggle2Brightness", std::to_wstring(hotkeyPresetToggle2Brightness));
    addLine(L"Step", std::to_wstring(hotkeyStep));
    addLine(L"RepeatIntervalMs", std::to_wstring(hotkeyRepeatIntervalMs));

    addSec(L"Delay");
    addLine(L"Enabled", hotkeyDelayEnabled ? L"1" : L"0");
    addLine(L"WaitTimeMs", std::to_wstring(hotkeyDelayMs));

    addSec(L"Overlay");
    addLine(L"Theme", std::to_wstring(overlayTheme));
    addLine(L"TimeoutMs", std::to_wstring(overlayTimeoutMs));
    addLine(L"SuppressOnDnd", osdSuppressOnDnd ? L"1" : L"0");
    addLine(L"UseWindowsAccent", overlayUseWindowsAccent ? L"1" : L"0");
    addLine(L"AccentColor", FormatColor(overlayAccentColor));
    addLine(L"UseDualColors", overlayUseDualColors ? L"1" : L"0");
    addLine(L"AccentColorDark", FormatColor(overlayAccentColorDark));

    addSec(L"Tray");
    addLine(L"PercentageMode", std::to_wstring(trayPercentageMode));
    addLine(L"IconColor", FormatColor(trayIconColor));
    addLine(L"UseDualColors", trayIconUseDualColors ? L"1" : L"0");
    addLine(L"IconColorDark", FormatColor(trayIconColorDark));
    addLine(L"UseDualCustomIcons", trayIconUseDualCustomIcons ? L"1" : L"0");
    addLine(L"CustomIcon", trayCustomIcon);
    addLine(L"CustomIconDark", trayCustomIconDark);
    addLine(L"Font", trayFont);
    addLine(L"FontWeight", std::to_wstring(trayFontWeight));
    addLine(L"FontSize", std::to_wstring(trayFontSize));
    addLine(L"FontSize100", std::to_wstring(trayFontSize100));
    addLine(L"YOffset", FormatYOffset(trayYOffset, trayYOffsetUseComma));
    addLine(L"WheelEnabled", trayWheelEnabled ? L"1" : L"0");
    addLine(L"WheelStep", std::to_wstring(trayWheelStep));
    addLine(L"WheelShowOsd", trayWheelShowOsd ? L"1" : L"0");
    addLine(L"WheelDelayEnabled", trayWheelDelayEnabled ? L"1" : L"0");
    addLine(L"WheelWaitTimeMs", std::to_wstring(trayWheelDelayMs));
    addLine(L"MiddleClickEnabled", trayMiddleClickEnabled ? L"1" : L"0");
    addLine(L"MiddleClickShowOsd", trayMiddleClickShowOsd ? L"1" : L"0");
    addLine(L"MiddleClickBrightness", std::to_wstring(trayMiddleClickBrightness));

    addSec(L"Flyout");
    addLine(L"UseWindowsAccent", flyoutUseWindowsAccent ? L"1" : L"0");
    addLine(L"AccentColor", FormatColor(flyoutAccentColor));
    addLine(L"UseDualColors", flyoutUseDualColors ? L"1" : L"0");
    addLine(L"AccentColorDark", FormatColor(flyoutAccentColorDark));
    addLine(L"ScrollEnabled", flyoutScrollEnabled ? L"1" : L"0");
    addLine(L"ScrollStep", std::to_wstring(flyoutScrollStep));
    addLine(L"KeyboardEnabled", flyoutKeyboardEnabled ? L"1" : L"0");
    addLine(L"ScrollDelayEnabled", flyoutScrollDelayEnabled ? L"1" : L"0");
    addLine(L"ScrollWaitTimeMs", std::to_wstring(flyoutScrollDelayMs));
    addLine(L"ButtonDelayEnabled", flyoutButtonDelayEnabled ? L"1" : L"0");
    addLine(L"ButtonWaitTimeMs", std::to_wstring(flyoutButtonDelayMs));

    addSec(L"State");
    addLine(L"Brightness", std::to_wstring(cachedBrightness));

    for (const auto& kv : monitorConfigs) {
        const std::wstring sec = L"Monitor_" + kv.first;
        const MonitorControlConfig& cfg = kv.second;
        addSec(sec.c_str());
        addLine(L"Enabled", cfg.enabled ? L"1" : L"0");
        addLine(L"CustomName", cfg.customName);
        addLine(L"ControlMode", std::to_wstring(cfg.controlMode));
        addLine(L"Offset", std::to_wstring(cfg.offset));
        addLine(L"SyncOverscrollMax", cfg.syncOverscrollMax ? L"1" : L"0");
        addLine(L"SyncOverscrollMin", cfg.syncOverscrollMin ? L"1" : L"0");
        addLine(L"HotkeyAdjustEnabled", cfg.hotkeyAdjustEnabled ? L"1" : L"0");
        addLine(L"HotkeyAdjustShowOsd", cfg.hotkeyAdjustShowOsd ? L"1" : L"0");
        addLine(L"HotkeyUpMods", std::to_wstring(cfg.hotkeyUpMods));
        addLine(L"HotkeyUpKey", std::to_wstring(cfg.hotkeyUpKey));
        addLine(L"HotkeyDownMods", std::to_wstring(cfg.hotkeyDownMods));
        addLine(L"HotkeyDownKey", std::to_wstring(cfg.hotkeyDownKey));
        addLine(L"HotkeyPresetsEnabled", cfg.hotkeyPresetsEnabled ? L"1" : L"0");
        addLine(L"HotkeyPresetsShowOsd", cfg.hotkeyPresetsShowOsd ? L"1" : L"0");
        addLine(L"HotkeyPreset1Mods", std::to_wstring(cfg.hotkeyPreset1Mods));
        addLine(L"HotkeyPreset1Key", std::to_wstring(cfg.hotkeyPreset1Key));
        addLine(L"HotkeyPreset1Brightness", std::to_wstring(cfg.hotkeyPreset1Brightness));
        addLine(L"HotkeyPreset2Mods", std::to_wstring(cfg.hotkeyPreset2Mods));
        addLine(L"HotkeyPreset2Key", std::to_wstring(cfg.hotkeyPreset2Key));
        addLine(L"HotkeyPreset2Brightness", std::to_wstring(cfg.hotkeyPreset2Brightness));
        addLine(L"HotkeyPresetToggleEnabled", cfg.hotkeyPresetToggleEnabled ? L"1" : L"0");
        addLine(L"HotkeyPresetToggleShowOsd", cfg.hotkeyPresetToggleShowOsd ? L"1" : L"0");
        addLine(L"HotkeyPresetToggleMods", std::to_wstring(cfg.hotkeyPresetToggleMods));
        addLine(L"HotkeyPresetToggleKey", std::to_wstring(cfg.hotkeyPresetToggleKey));
        addLine(L"HotkeyPresetToggle1Brightness", std::to_wstring(cfg.hotkeyPresetToggle1Brightness));
        addLine(L"HotkeyPresetToggle2Brightness", std::to_wstring(cfg.hotkeyPresetToggle2Brightness));
        addLine(L"HotkeyStep", std::to_wstring(cfg.hotkeyStep));
        addLine(L"OsdUseWindowsAccent", cfg.overlayUseWindowsAccent ? L"1" : L"0");
        addLine(L"OsdAccentColor", FormatColor(cfg.overlayAccentColor));
        addLine(L"OsdUseDualColors", cfg.overlayUseDualColors ? L"1" : L"0");
        addLine(L"OsdAccentColorDark", FormatColor(cfg.overlayAccentColorDark));
        addLine(L"TrayPercentageMode", std::to_wstring(cfg.trayPercentageMode));
        addLine(L"TrayIconColor", FormatColor(cfg.trayIconColor));
        addLine(L"TrayUseDualColors", cfg.trayIconUseDualColors ? L"1" : L"0");
        addLine(L"TrayIconColorDark", FormatColor(cfg.trayIconColorDark));
        addLine(L"TrayUseDualCustomIcons", cfg.trayIconUseDualCustomIcons ? L"1" : L"0");
        addLine(L"TrayCustomIcon", cfg.trayCustomIcon);
        addLine(L"TrayCustomIconDark", cfg.trayCustomIconDark);
        addLine(L"TrayFont", cfg.trayFont);
        addLine(L"TrayFontWeight", std::to_wstring(cfg.trayFontWeight));
        addLine(L"TrayFontSize", std::to_wstring(cfg.trayFontSize));
        addLine(L"TrayFontSize100", std::to_wstring(cfg.trayFontSize100));
        addLine(L"TrayYOffset", FormatYOffset(cfg.trayYOffset, cfg.trayYOffsetUseComma));
        addLine(L"TrayWheelEnabled", cfg.trayWheelEnabled ? L"1" : L"0");
        addLine(L"TrayWheelStep", std::to_wstring(cfg.trayWheelStep));
        addLine(L"TrayWheelShowOsd", cfg.trayWheelShowOsd ? L"1" : L"0");
        addLine(L"TrayMiddleClickEnabled", cfg.trayMiddleClickEnabled ? L"1" : L"0");
        addLine(L"TrayMiddleClickShowOsd", cfg.trayMiddleClickShowOsd ? L"1" : L"0");
        addLine(L"TrayMiddleClickBrightness", std::to_wstring(cfg.trayMiddleClickBrightness));
        addLine(L"FlyoutUseWindowsAccent", cfg.flyoutUseWindowsAccent ? L"1" : L"0");
        addLine(L"FlyoutAccentColor", FormatColor(cfg.flyoutAccentColor));
        addLine(L"FlyoutUseDualColors", cfg.flyoutUseDualColors ? L"1" : L"0");
        addLine(L"FlyoutAccentColorDark", FormatColor(cfg.flyoutAccentColorDark));
        addLine(L"FlyoutScrollEnabled", cfg.flyoutScrollEnabled ? L"1" : L"0");
        addLine(L"FlyoutScrollStep", std::to_wstring(cfg.flyoutScrollStep));
        addLine(L"ScheduleMode", std::to_wstring(cfg.scheduleMode));
        addLine(L"ScheduleSunSource", std::to_wstring(cfg.scheduleSunSource));
        addLine(L"ScheduleDayHour", std::to_wstring(cfg.scheduleDayHour));
        addLine(L"ScheduleDayMinute", std::to_wstring(cfg.scheduleDayMinute));
        addLine(L"ScheduleNightHour", std::to_wstring(cfg.scheduleNightHour));
        addLine(L"ScheduleNightMinute", std::to_wstring(cfg.scheduleNightMinute));
        addLine(L"ScheduleDayBrightness", std::to_wstring(cfg.scheduleDayBrightness));
        addLine(L"ScheduleNightBrightness", std::to_wstring(cfg.scheduleNightBrightness));
        addLine(L"ScheduleLatitude", std::to_wstring(cfg.scheduleLatitude));
        addLine(L"ScheduleLongitude", std::to_wstring(cfg.scheduleLongitude));
        addLine(L"ScheduleSunriseOffset", std::to_wstring(cfg.scheduleSunriseOffset));
        addLine(L"ScheduleSunsetOffset", std::to_wstring(cfg.scheduleSunsetOffset));
        addLine(L"ScheduleShowOsd", cfg.scheduleShowOsd ? L"1" : L"0");
        addLine(L"ScheduleOsdOnlyOnChange", cfg.scheduleOsdOnlyOnChange ? L"1" : L"0");
        addLine(L"CachedBrightness", std::to_wstring(cfg.cachedBrightness));
    }

    HANDLE hFile = CreateFileW(ini.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD bytesToWrite = static_cast<DWORD>(out.size() * sizeof(wchar_t));
        DWORD written = 0;
        WriteFile(hFile, out.data(), bytesToWrite, &written, NULL);
        CloseHandle(hFile);
    }
}

bool AppSettings::IsStartWithWindowsEnabled() {
    HKEY hKey = NULL;
    LONG res = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0,
        KEY_READ,
        &hKey
    );
    if (res != ERROR_SUCCESS) return false;

    DWORD type = 0;
    DWORD dataSize = 0;
    res = RegQueryValueExW(hKey, L"ClickBrightness", NULL, &type, NULL, &dataSize);
    RegCloseKey(hKey);

    return (res == ERROR_SUCCESS);
}

bool AppSettings::IsInstalledInProgramFiles() {
    wchar_t exePath[MAX_PATH] = { 0 };
    DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    auto checkFolder = [&](REFKNOWNFOLDERID rfid) -> bool {
        PWSTR pPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(rfid, 0, NULL, &pPath)) && pPath) {
            std::wstring folder = pPath;
            CoTaskMemFree(pPath);
            if (!folder.empty() && folder.back() != L'\\') folder += L'\\';
            if (folder.length() <= wcslen(exePath) && _wcsnicmp(exePath, folder.c_str(), folder.length()) == 0) {
                return true;
            }
        }
        return false;
    };

    if (checkFolder(FOLDERID_ProgramFiles)) return true;
    if (checkFolder(FOLDERID_ProgramFilesX86)) return true;
    if (checkFolder(FOLDERID_ProgramFilesX64)) return true;

    // Fallback: check environment variables ProgramFiles, ProgramFiles(x86), ProgramW6432
    wchar_t envBuf[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableW(L"ProgramFiles", envBuf, MAX_PATH) > 0) {
        std::wstring pf = envBuf;
        if (!pf.empty() && pf.back() != L'\\') pf += L'\\';
        if (pf.length() <= wcslen(exePath) && _wcsnicmp(exePath, pf.c_str(), pf.length()) == 0) return true;
    }
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", envBuf, MAX_PATH) > 0) {
        std::wstring pf = envBuf;
        if (!pf.empty() && pf.back() != L'\\') pf += L'\\';
        if (pf.length() <= wcslen(exePath) && _wcsnicmp(exePath, pf.c_str(), pf.length()) == 0) return true;
    }
    if (GetEnvironmentVariableW(L"ProgramW6432", envBuf, MAX_PATH) > 0) {
        std::wstring pf = envBuf;
        if (!pf.empty() && pf.back() != L'\\') pf += L'\\';
        if (pf.length() <= wcslen(exePath) && _wcsnicmp(exePath, pf.c_str(), pf.length()) == 0) return true;
    }

    return false;
}

bool AppSettings::SetStartWithWindows(bool enable) {
    if (!IsInstalledInProgramFiles() || g_Settings.IsSimulatedMode()) {
        // Block changes when not installed in Program Files without touching existing registry entries
        return false;
    }

    HKEY hKey = NULL;
    LONG res = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0,
        KEY_SET_VALUE,
        &hKey
    );
    if (res != ERROR_SUCCESS) return false;

    if (enable) {
        wchar_t exePath[MAX_PATH];
        DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring quoted = L"\"" + std::wstring(exePath) + L"\"";
            res = RegSetValueExW(
                hKey,
                L"ClickBrightness",
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(quoted.c_str()),
                static_cast<DWORD>((quoted.length() + 1) * sizeof(wchar_t))
            );
        } else {
            res = ERROR_BAD_PATHNAME;
        }
    } else {
        res = RegDeleteValueW(hKey, L"ClickBrightness");
        if (res == ERROR_FILE_NOT_FOUND) {
            res = ERROR_SUCCESS;
        }
    }

    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS);
}

std::wstring AppSettings::FormatYOffset(float val, bool useComma) {
    wchar_t buf[32];
    if (std::abs(val - std::round(val)) < 0.005f) {
        swprintf_s(buf, L"%d", (int)std::round(val));
    } else {
        swprintf_s(buf, L"%.2f", val);
        size_t len = wcslen(buf);
        while (len > 0 && buf[len - 1] == L'0') {
            buf[--len] = L'\0';
        }
        if (len > 0 && buf[len - 1] == L'.') {
            buf[--len] = L'\0';
        }
    }
    std::wstring res = buf;
    if (useComma) {
        for (wchar_t& ch : res) {
            if (ch == L'.') ch = L',';
        }
    }
    return res;
}

float AppSettings::ParseYOffset(const std::wstring& str, bool* outUseComma) {
    if (str.empty()) return 0.0f;
    bool hasComma = (str.find(L',') != std::wstring::npos);
    if (outUseComma) {
        if (hasComma) {
            *outUseComma = true;
        } else if (str.find(L'.') != std::wstring::npos) {
            *outUseComma = false;
        }
    }
    std::wstring normalized = str;
    for (wchar_t& ch : normalized) {
        if (ch == L',') ch = L'.';
    }
    wchar_t* endPtr = nullptr;
    float val = wcstof(normalized.c_str(), &endPtr);
    if (endPtr == normalized.c_str()) return 0.0f;
    if (val < -10.0f) val = -10.0f;
    if (val > 10.0f) val = 10.0f;
    return val;
}

COLORREF AppSettings::ParseColor(const std::wstring& str, COLORREF defaultColor) {
    if (str.empty()) return defaultColor;

    size_t first = str.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return defaultColor;
    size_t last = str.find_last_not_of(L" \t\r\n");
    std::wstring s = str.substr(first, last - first + 1);

    if (s.find(L',') != std::wstring::npos) {
        int r = 0, g = 0, b = 0;
        if (swscanf_s(s.c_str(), L"%d , %d , %d", &r, &g, &b) == 3) {
            r = (std::max)(0, (std::min)(255, r));
            g = (std::max)(0, (std::min)(255, g));
            b = (std::max)(0, (std::min)(255, b));
            return RGB(r, g, b);
        }
    }

    if (s.rfind(L"#", 0) == 0) {
        s = s.substr(1);
    } else if (s.rfind(L"0x", 0) == 0 || s.rfind(L"0X", 0) == 0) {
        s = s.substr(2);
    }

    if (s.length() == 6) {
        wchar_t* endPtr = nullptr;
        unsigned long val = wcstoul(s.c_str(), &endPtr, 16);
        if (endPtr != s.c_str()) {
            int r = (val >> 16) & 0xFF;
            int g = (val >> 8) & 0xFF;
            int b = val & 0xFF;
            return RGB(r, g, b);
        }
    } else if (s.length() == 3) {
        wchar_t* endPtr = nullptr;
        unsigned long val = wcstoul(s.c_str(), &endPtr, 16);
        if (endPtr != s.c_str()) {
            int r = ((val >> 8) & 0xF) * 17;
            int g = ((val >> 4) & 0xF) * 17;
            int b = (val & 0xF) * 17;
            return RGB(r, g, b);
        }
    }

    return defaultColor;
}

std::wstring AppSettings::FormatColor(COLORREF c) {
    wchar_t buf[16];
    swprintf_s(buf, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

bool AppSettings::IsMonitorSelected(const std::wstring& monitorId) const {
    auto it = monitorConfigs.find(monitorId);
    if (it != monitorConfigs.end()) {
        return it->second.enabled;
    }
    return true;
}

std::wstring AppSettings::SanitizeMonitorName(const std::wstring& name, size_t maxLen) {
    std::wstring s = name;
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    if (maxLen > 0 && s.length() > maxLen) {
        s.resize(maxLen);
    }
    return s;
}

MonitorControlConfig& AppSettings::GetOrCreateMonitorConfig(const std::wstring& monitorId, const std::wstring& defaultName) {
    auto it = monitorConfigs.find(monitorId);
    if (it != monitorConfigs.end()) {
        if (it->second.customName.empty() && !defaultName.empty()) {
            it->second.customName = SanitizeMonitorName(defaultName);
        }
        return it->second;
    }
    MonitorControlConfig cfg;
    CopyCustomizationToMonitor(cfg);
    if (!mainMonitorId.empty() && _wcsicmp(monitorId.c_str(), mainMonitorId.c_str()) != 0) {
        ClearHotkeys(cfg);
        cfg.trayPercentageMode = 3; // Non-main Sync monitors default to Hide tray icon
    }
    cfg.enabled = true;
    cfg.customName = SanitizeMonitorName(defaultName);
    cfg.controlMode = 0;
    cfg.offset = 0;
    monitorConfigs[monitorId] = cfg;
    return monitorConfigs[monitorId];
}

const MonitorControlConfig* AppSettings::FindMonitorConfig(const std::wstring& monitorId) const {
    auto it = monitorConfigs.find(monitorId);
    return (it != monitorConfigs.end()) ? &it->second : nullptr;
}

int AppSettings::ComputeSyncedBrightness(int mainBrightness, const MonitorControlConfig& cfg) {
    return (std::clamp)(mainBrightness + cfg.offset, 0, 100);
}

void AppSettings::ClearHotkeys(MonitorControlConfig& cfg) {
    cfg.hotkeyUpKey = 0;
    cfg.hotkeyUpMods = 0;
    cfg.hotkeyDownKey = 0;
    cfg.hotkeyDownMods = 0;
    cfg.hotkeyPreset1Key = 0;
    cfg.hotkeyPreset1Mods = 0;
    cfg.hotkeyPreset2Key = 0;
    cfg.hotkeyPreset2Mods = 0;
    cfg.hotkeyPresetToggleKey = 0;
    cfg.hotkeyPresetToggleMods = 0;
}

void AppSettings::ClearMatchingHotkey(MonitorControlConfig& cfg, UINT vk, UINT mods) {
    if (vk == 0) return;
    if (cfg.hotkeyUpKey == vk && cfg.hotkeyUpMods == mods) { cfg.hotkeyUpKey = 0; cfg.hotkeyUpMods = 0; }
    if (cfg.hotkeyDownKey == vk && cfg.hotkeyDownMods == mods) { cfg.hotkeyDownKey = 0; cfg.hotkeyDownMods = 0; }
    if (cfg.hotkeyPreset1Key == vk && cfg.hotkeyPreset1Mods == mods) { cfg.hotkeyPreset1Key = 0; cfg.hotkeyPreset1Mods = 0; }
    if (cfg.hotkeyPreset2Key == vk && cfg.hotkeyPreset2Mods == mods) { cfg.hotkeyPreset2Key = 0; cfg.hotkeyPreset2Mods = 0; }
    if (cfg.hotkeyPresetToggleKey == vk && cfg.hotkeyPresetToggleMods == mods) { cfg.hotkeyPresetToggleKey = 0; cfg.hotkeyPresetToggleMods = 0; }
}

MonitorControlConfig AppSettings::GetEffectiveMonitorConfig(const std::wstring& monitorId) const {
    auto it = monitorConfigs.find(monitorId);
    if (it != monitorConfigs.end() && it->second.controlMode == 1) {
        return it->second;
    }
    MonitorControlConfig result;
    if (!mainMonitorId.empty()) {
        auto mainIt = monitorConfigs.find(mainMonitorId);
        if (mainIt != monitorConfigs.end()) {
            result = mainIt->second;
        }
    } else if (it != monitorConfigs.end()) {
        result = it->second;
    }

    bool isMain = (mainMonitorId.empty() || _wcsicmp(monitorId.c_str(), mainMonitorId.c_str()) == 0);
    if (it != monitorConfigs.end() && !isMain) {
        const auto& own = it->second;
        result.enabled = own.enabled;
        result.customName = own.customName;
        result.controlMode = own.controlMode;
        result.offset = own.offset;
        result.syncOverscrollMax = own.syncOverscrollMax;
        result.syncOverscrollMin = own.syncOverscrollMin;
        result.overlayUseWindowsAccent = own.overlayUseWindowsAccent;
        result.overlayAccentColor = own.overlayAccentColor;
        result.overlayUseDualColors = own.overlayUseDualColors;
        result.overlayAccentColorDark = own.overlayAccentColorDark;
        result.flyoutUseWindowsAccent = own.flyoutUseWindowsAccent;
        result.flyoutAccentColor = own.flyoutAccentColor;
        result.flyoutUseDualColors = own.flyoutUseDualColors;
        result.flyoutAccentColorDark = own.flyoutAccentColorDark;
        result.trayPercentageMode = own.trayPercentageMode;
        result.trayIconColor = own.trayIconColor;
        result.trayIconUseDualColors = own.trayIconUseDualColors;
        result.trayIconColorDark = own.trayIconColorDark;
        result.trayIconUseDualCustomIcons = own.trayIconUseDualCustomIcons;
        result.trayCustomIcon = own.trayCustomIcon;
        result.trayCustomIconDark = own.trayCustomIconDark;
        result.trayFont = own.trayFont;
        result.trayFontWeight = own.trayFontWeight;
        result.trayFontSize = own.trayFontSize;
        result.trayFontSize100 = own.trayFontSize100;
        result.trayYOffset = own.trayYOffset;
        result.trayYOffsetUseComma = own.trayYOffsetUseComma;
        result.scheduleMode = own.scheduleMode;
        result.scheduleSunSource = own.scheduleSunSource;
        result.scheduleDayHour = own.scheduleDayHour;
        result.scheduleDayMinute = own.scheduleDayMinute;
        result.scheduleNightHour = own.scheduleNightHour;
        result.scheduleNightMinute = own.scheduleNightMinute;
        result.scheduleDayBrightness = own.scheduleDayBrightness;
        result.scheduleNightBrightness = own.scheduleNightBrightness;
        result.scheduleLatitude = own.scheduleLatitude;
        result.scheduleLongitude = own.scheduleLongitude;
        result.scheduleSunriseOffset = own.scheduleSunriseOffset;
        result.scheduleSunsetOffset = own.scheduleSunsetOffset;
        result.scheduleShowOsd = own.scheduleShowOsd;
        result.scheduleOsdOnlyOnChange = own.scheduleOsdOnlyOnChange;
        result.cachedBrightness = own.cachedBrightness;
    }
    if (isMain && result.trayPercentageMode == 3) {
        result.trayPercentageMode = 0;
    }
    return result;
}

void AppSettings::CopyCustomizationToMonitor(MonitorControlConfig& dst) const {
    dst.hotkeyAdjustEnabled = hotkeyAdjustEnabled;
    dst.hotkeyAdjustShowOsd = hotkeyAdjustShowOsd;
    dst.hotkeyUpMods = hotkeyUpMods;
    dst.hotkeyUpKey = hotkeyUpKey;
    dst.hotkeyDownMods = hotkeyDownMods;
    dst.hotkeyDownKey = hotkeyDownKey;
    dst.hotkeyPresetsEnabled = hotkeyPresetsEnabled;
    dst.hotkeyPresetsShowOsd = hotkeyPresetsShowOsd;
    dst.hotkeyPreset1Mods = hotkeyPreset1Mods;
    dst.hotkeyPreset1Key = hotkeyPreset1Key;
    dst.hotkeyPreset1Brightness = hotkeyPreset1Brightness;
    dst.hotkeyPreset2Mods = hotkeyPreset2Mods;
    dst.hotkeyPreset2Key = hotkeyPreset2Key;
    dst.hotkeyPreset2Brightness = hotkeyPreset2Brightness;
    dst.hotkeyPresetToggleEnabled = hotkeyPresetToggleEnabled;
    dst.hotkeyPresetToggleShowOsd = hotkeyPresetToggleShowOsd;
    dst.hotkeyPresetToggleMods = hotkeyPresetToggleMods;
    dst.hotkeyPresetToggleKey = hotkeyPresetToggleKey;
    dst.hotkeyPresetToggle1Brightness = hotkeyPresetToggle1Brightness;
    dst.hotkeyPresetToggle2Brightness = hotkeyPresetToggle2Brightness;
    dst.hotkeyStep = hotkeyStep;

    dst.overlayUseWindowsAccent = overlayUseWindowsAccent;
    dst.overlayAccentColor = overlayAccentColor;
    dst.overlayUseDualColors = overlayUseDualColors;
    dst.overlayAccentColorDark = overlayAccentColorDark;

    dst.trayPercentageMode = trayPercentageMode;
    dst.trayIconColor = trayIconColor;
    dst.trayIconUseDualColors = trayIconUseDualColors;
    dst.trayIconColorDark = trayIconColorDark;
    dst.trayIconUseDualCustomIcons = trayIconUseDualCustomIcons;
    dst.trayCustomIcon = trayCustomIcon;
    dst.trayCustomIconDark = trayCustomIconDark;
    dst.trayFont = trayFont;
    dst.trayFontWeight = trayFontWeight;
    dst.trayFontSize = trayFontSize;
    dst.trayFontSize100 = trayFontSize100;
    dst.trayYOffset = trayYOffset;
    dst.trayYOffsetUseComma = trayYOffsetUseComma;
    dst.trayWheelEnabled = trayWheelEnabled;
    dst.trayWheelStep = trayWheelStep;
    dst.trayWheelShowOsd = trayWheelShowOsd;
    dst.trayMiddleClickEnabled = trayMiddleClickEnabled;
    dst.trayMiddleClickShowOsd = trayMiddleClickShowOsd;
    dst.trayMiddleClickBrightness = trayMiddleClickBrightness;

    dst.flyoutUseWindowsAccent = flyoutUseWindowsAccent;
    dst.flyoutAccentColor = flyoutAccentColor;
    dst.flyoutUseDualColors = flyoutUseDualColors;
    dst.flyoutAccentColorDark = flyoutAccentColorDark;
    dst.flyoutScrollEnabled = flyoutScrollEnabled;
    dst.flyoutScrollStep = flyoutScrollStep;
    dst.scheduleMode = scheduleMode;
    dst.scheduleSunSource = scheduleSunSource;
    dst.scheduleDayHour = scheduleDayHour;
    dst.scheduleDayMinute = scheduleDayMinute;
    dst.scheduleNightHour = scheduleNightHour;
    dst.scheduleNightMinute = scheduleNightMinute;
    dst.scheduleDayBrightness = scheduleDayBrightness;
    dst.scheduleNightBrightness = scheduleNightBrightness;
    dst.scheduleLatitude = scheduleLatitude;
    dst.scheduleLongitude = scheduleLongitude;
    dst.scheduleSunriseOffset = scheduleSunriseOffset;
    dst.scheduleSunsetOffset = scheduleSunsetOffset;
    dst.scheduleShowOsd = scheduleShowOsd;
    dst.scheduleOsdOnlyOnChange = scheduleOsdOnlyOnChange;
    dst.cachedBrightness = cachedBrightness;
}

void AppSettings::ApplyMonitorCustomizationToLegacy(const MonitorControlConfig& src) {
    hotkeyAdjustEnabled = src.hotkeyAdjustEnabled;
    hotkeyAdjustShowOsd = src.hotkeyAdjustShowOsd;
    hotkeyUpMods = src.hotkeyUpMods;
    hotkeyUpKey = src.hotkeyUpKey;
    hotkeyDownMods = src.hotkeyDownMods;
    hotkeyDownKey = src.hotkeyDownKey;
    hotkeyPresetsEnabled = src.hotkeyPresetsEnabled;
    hotkeyPresetsShowOsd = src.hotkeyPresetsShowOsd;
    hotkeyPreset1Mods = src.hotkeyPreset1Mods;
    hotkeyPreset1Key = src.hotkeyPreset1Key;
    hotkeyPreset1Brightness = src.hotkeyPreset1Brightness;
    hotkeyPreset2Mods = src.hotkeyPreset2Mods;
    hotkeyPreset2Key = src.hotkeyPreset2Key;
    hotkeyPreset2Brightness = src.hotkeyPreset2Brightness;
    hotkeyPresetToggleEnabled = src.hotkeyPresetToggleEnabled;
    hotkeyPresetToggleShowOsd = src.hotkeyPresetToggleShowOsd;
    hotkeyPresetToggleMods = src.hotkeyPresetToggleMods;
    hotkeyPresetToggleKey = src.hotkeyPresetToggleKey;
    hotkeyPresetToggle1Brightness = src.hotkeyPresetToggle1Brightness;
    hotkeyPresetToggle2Brightness = src.hotkeyPresetToggle2Brightness;
    hotkeyStep = src.hotkeyStep;

    overlayUseWindowsAccent = src.overlayUseWindowsAccent;
    overlayAccentColor = src.overlayAccentColor;
    overlayUseDualColors = src.overlayUseDualColors;
    overlayAccentColorDark = src.overlayAccentColorDark;

    trayPercentageMode = src.trayPercentageMode;
    trayIconColor = src.trayIconColor;
    trayIconUseDualColors = src.trayIconUseDualColors;
    trayIconColorDark = src.trayIconColorDark;
    trayIconUseDualCustomIcons = src.trayIconUseDualCustomIcons;
    trayCustomIcon = src.trayCustomIcon;
    trayCustomIconDark = src.trayCustomIconDark;
    trayFont = src.trayFont;
    trayFontWeight = src.trayFontWeight;
    trayFontSize = src.trayFontSize;
    trayFontSize100 = src.trayFontSize100;
    trayYOffset = src.trayYOffset;
    trayYOffsetUseComma = src.trayYOffsetUseComma;
    trayWheelEnabled = src.trayWheelEnabled;
    trayWheelStep = src.trayWheelStep;
    trayWheelShowOsd = src.trayWheelShowOsd;
    trayMiddleClickEnabled = src.trayMiddleClickEnabled;
    trayMiddleClickShowOsd = src.trayMiddleClickShowOsd;
    trayMiddleClickBrightness = src.trayMiddleClickBrightness;

    flyoutUseWindowsAccent = src.flyoutUseWindowsAccent;
    flyoutAccentColor = src.flyoutAccentColor;
    flyoutUseDualColors = src.flyoutUseDualColors;
    flyoutAccentColorDark = src.flyoutAccentColorDark;
    flyoutScrollEnabled = src.flyoutScrollEnabled;
    flyoutScrollStep = src.flyoutScrollStep;
    scheduleMode = src.scheduleMode;
    scheduleSunSource = src.scheduleSunSource;
    scheduleDayHour = src.scheduleDayHour;
    scheduleDayMinute = src.scheduleDayMinute;
    scheduleNightHour = src.scheduleNightHour;
    scheduleNightMinute = src.scheduleNightMinute;
    scheduleDayBrightness = src.scheduleDayBrightness;
    scheduleNightBrightness = src.scheduleNightBrightness;
    scheduleLatitude = src.scheduleLatitude;
    scheduleLongitude = src.scheduleLongitude;
    scheduleSunriseOffset = src.scheduleSunriseOffset;
    scheduleSunsetOffset = src.scheduleSunsetOffset;
    scheduleShowOsd = src.scheduleShowOsd;
    scheduleOsdOnlyOnChange = src.scheduleOsdOnlyOnChange;
    cachedBrightness = src.cachedBrightness;
}


