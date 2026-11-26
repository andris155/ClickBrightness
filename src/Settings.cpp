#include "Settings.h"
#include <cmath>
#include <algorithm>
#include <shlobj.h>
#include <filesystem>

#pragma comment(lib, "shell32.lib")

AppSettings g_Settings;

const std::wstring& AppSettings::GetIniFilePath() {
    static std::wstring s_iniPath;
    if (s_iniPath.empty()) {
        // If running in portable mode (settings.ini or portable.txt exists next to exe), keep settings app-local
        wchar_t exePath[MAX_PATH] = { 0 };
        DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
            std::filesystem::path localIni = exeDir / L"settings.ini";
            std::filesystem::path portableMarker = exeDir / L"portable.txt";
            if (std::filesystem::exists(localIni) || std::filesystem::exists(portableMarker)) {
                s_iniPath = localIni.wstring();
                return s_iniPath;
            }
        }

        PWSTR localAppData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &localAppData))) {
            std::filesystem::path dir = std::filesystem::path(localAppData) / L"ClickBrightness";
            std::filesystem::path oldDir = std::filesystem::path(localAppData) / L"BrightnessApp";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::filesystem::path iniFile = dir / L"settings.ini";
            std::filesystem::path oldIniFile = oldDir / L"settings.ini";
            if (!std::filesystem::exists(iniFile, ec) && std::filesystem::exists(oldIniFile, ec)) {
                std::filesystem::copy_file(oldIniFile, iniFile, std::filesystem::copy_options::overwrite_existing, ec);
            }
            s_iniPath = iniFile.wstring();
            CoTaskMemFree(localAppData);
        }
        if (s_iniPath.empty()) {
            wchar_t exePath[MAX_PATH];
            DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                exePath[len] = L'\0';
                wchar_t* lastSlash = wcsrchr(exePath, L'\\');
                if (lastSlash) {
                    *(lastSlash + 1) = L'\0';
                    s_iniPath = exePath;
                    s_iniPath += L"settings.ini";
                }
            }
            if (s_iniPath.empty()) {
                s_iniPath = L"settings.ini";
            }
        }
    }
    return s_iniPath;
}

void AppSettings::Load() {
    std::wstring ini = GetIniFilePath();

    hotkeyAdjustEnabled = (GetPrivateProfileIntW(L"Hotkeys", L"AdjustEnabled", 0, ini.c_str()) != 0);
    hotkeyAdjustShowOsd = (GetPrivateProfileIntW(L"Hotkeys", L"AdjustShowOsd", 1, ini.c_str()) != 0);
    hotkeyUpKey = GetPrivateProfileIntW(L"Hotkeys", L"KeyUp", VK_F14, ini.c_str());
    hotkeyUpMods = GetPrivateProfileIntW(L"Hotkeys", L"ModsUp", 0, ini.c_str());

    hotkeyDownKey = GetPrivateProfileIntW(L"Hotkeys", L"KeyDown", VK_F13, ini.c_str());
    hotkeyDownMods = GetPrivateProfileIntW(L"Hotkeys", L"ModsDown", 0, ini.c_str());

    hotkeyPresetsEnabled = (GetPrivateProfileIntW(L"Hotkeys", L"PresetsEnabled", 0, ini.c_str()) != 0);
    hotkeyPresetsShowOsd = (GetPrivateProfileIntW(L"Hotkeys", L"PresetsShowOsd", 1, ini.c_str()) != 0);
    hotkeyPreset1Key = GetPrivateProfileIntW(L"Hotkeys", L"KeyPreset1", VK_F15, ini.c_str());
    hotkeyPreset1Mods = GetPrivateProfileIntW(L"Hotkeys", L"ModsPreset1", 0, ini.c_str());
    hotkeyPreset1Brightness = GetPrivateProfileIntW(L"Hotkeys", L"Preset1Brightness", 30, ini.c_str());
    if (hotkeyPreset1Brightness < 0 || hotkeyPreset1Brightness > 100) hotkeyPreset1Brightness = 30;

    hotkeyPreset2Key = GetPrivateProfileIntW(L"Hotkeys", L"KeyPreset2", VK_F16, ini.c_str());
    hotkeyPreset2Mods = GetPrivateProfileIntW(L"Hotkeys", L"ModsPreset2", 0, ini.c_str());
    hotkeyPreset2Brightness = GetPrivateProfileIntW(L"Hotkeys", L"Preset2Brightness", 80, ini.c_str());
    if (hotkeyPreset2Brightness < 0 || hotkeyPreset2Brightness > 100) hotkeyPreset2Brightness = 80;

    hotkeyPresetToggleEnabled = (GetPrivateProfileIntW(L"Hotkeys", L"PresetToggleEnabled", 0, ini.c_str()) != 0);
    hotkeyPresetToggleShowOsd = (GetPrivateProfileIntW(L"Hotkeys", L"PresetToggleShowOsd", 1, ini.c_str()) != 0);
    hotkeyPresetToggleKey = GetPrivateProfileIntW(L"Hotkeys", L"KeyPresetToggle", VK_F17, ini.c_str());
    hotkeyPresetToggleMods = GetPrivateProfileIntW(L"Hotkeys", L"ModsPresetToggle", 0, ini.c_str());
    hotkeyPresetToggle1Brightness = GetPrivateProfileIntW(L"Hotkeys", L"PresetToggle1Brightness", 30, ini.c_str());
    if (hotkeyPresetToggle1Brightness < 0 || hotkeyPresetToggle1Brightness > 100) hotkeyPresetToggle1Brightness = 30;
    hotkeyPresetToggle2Brightness = GetPrivateProfileIntW(L"Hotkeys", L"PresetToggle2Brightness", 80, ini.c_str());
    if (hotkeyPresetToggle2Brightness < 0 || hotkeyPresetToggle2Brightness > 100) hotkeyPresetToggle2Brightness = 80;

    hotkeyStep = GetPrivateProfileIntW(L"Hotkeys", L"Step", 5, ini.c_str());
    if (hotkeyStep < 1 || hotkeyStep > 50) hotkeyStep = 5;

    hotkeyRepeatIntervalMs = GetPrivateProfileIntW(L"Hotkeys", L"RepeatIntervalMs", 90, ini.c_str());
    if (hotkeyRepeatIntervalMs < 20 || hotkeyRepeatIntervalMs > 500) hotkeyRepeatIntervalMs = 90;

    hotkeyDelayEnabled = (GetPrivateProfileIntW(L"Delay", L"Enabled", 1, ini.c_str()) != 0);
    hotkeyDelayMs = GetPrivateProfileIntW(L"Delay", L"WaitTimeMs", 1000, ini.c_str());
    if (hotkeyDelayMs < 100 || hotkeyDelayMs > 3000) hotkeyDelayMs = 1000;

    int pctMode = GetPrivateProfileIntW(L"Tray", L"PercentageMode", -1, ini.c_str());
    if (pctMode == -1) {
        bool showPct = (GetPrivateProfileIntW(L"Tray", L"ShowPercentage", 1, ini.c_str()) != 0);
        trayPercentageMode = showPct ? 0 : 2;
    } else {
        trayPercentageMode = (pctMode >= 0 && pctMode <= 2) ? pctMode : 0;
    }

    wchar_t trayColBuf[64] = { 0 };
    GetPrivateProfileStringW(L"Tray", L"IconColor", L"#0078D4", trayColBuf, 64, ini.c_str());
    trayIconColor = ParseColor(trayColBuf, RGB(0, 120, 212));

    trayIconUseDualColors = (GetPrivateProfileIntW(L"Tray", L"UseDualColors", 0, ini.c_str()) != 0);
    GetPrivateProfileStringW(L"Tray", L"IconColorDark", L"#60CDFF", trayColBuf, 64, ini.c_str());
    trayIconColorDark = ParseColor(trayColBuf, RGB(96, 205, 255));

    wchar_t fontBuf[128] = { 0 };
    GetPrivateProfileStringW(L"Tray", L"Font", L"Segoe UI Variable Text", fontBuf, 128, ini.c_str());
    trayFont = fontBuf;
    if (trayFont.empty()) trayFont = L"Segoe UI Variable Text";

    int loadedWeight = GetPrivateProfileIntW(L"Tray", L"FontWeight", -1, ini.c_str());
    if (loadedWeight == -1) {
        // Migrate legacy weight suffix embedded in trayFont name
        trayFontWeight = 375;
        auto stripSuffix = [&](const wchar_t* suffix, int weight) {
            size_t pos = trayFont.rfind(suffix);
            if (pos != std::wstring::npos && pos + wcslen(suffix) == trayFont.length()) {
                trayFont.erase(pos);
                trayFontWeight = weight;
                return true;
            }
            return false;
        };
        if (!stripSuffix(L" Semilight", 350) && !stripSuffix(L" SemiLight", 350) &&
            !stripSuffix(L" Light", 300) && !stripSuffix(L" Semibold", 600) &&
            !stripSuffix(L" SemiBold", 600) && !stripSuffix(L" Medium", 500)) {
            stripSuffix(L" Bold", 700);
        }
    } else {
        trayFontWeight = (loadedWeight >= 100 && loadedWeight <= 900) ? loadedWeight : 375;
    }

    trayFontSize = GetPrivateProfileIntW(L"Tray", L"FontSize", 102, ini.c_str());
    if (trayFontSize < 40 || trayFontSize > 150) trayFontSize = 102;

    trayFontSize100 = GetPrivateProfileIntW(L"Tray", L"FontSize100", 63, ini.c_str());
    if (trayFontSize100 < 30 || trayFontSize100 > 130) trayFontSize100 = 63;

    wchar_t yoBuf[32] = { 0 };
    GetPrivateProfileStringW(L"Tray", L"YOffset", L"-0.5", yoBuf, 32, ini.c_str());
    trayYOffset = ParseYOffset(yoBuf, &trayYOffsetUseComma);

    trayWheelEnabled = (GetPrivateProfileIntW(L"Tray", L"WheelEnabled", 0, ini.c_str()) != 0);
    trayWheelStep = GetPrivateProfileIntW(L"Tray", L"WheelStep", 5, ini.c_str());
    if (trayWheelStep < 1 || trayWheelStep > 50) trayWheelStep = 5;
    trayWheelShowOsd = (GetPrivateProfileIntW(L"Tray", L"WheelShowOsd", 1, ini.c_str()) != 0);
    trayWheelDelayEnabled = (GetPrivateProfileIntW(L"Tray", L"WheelDelayEnabled", 1, ini.c_str()) != 0);
    trayWheelDelayMs = GetPrivateProfileIntW(L"Tray", L"WheelWaitTimeMs", 1000, ini.c_str());
    if (trayWheelDelayMs < 100 || trayWheelDelayMs > 3000) trayWheelDelayMs = 1000;

    trayMiddleClickEnabled = (GetPrivateProfileIntW(L"Tray", L"MiddleClickEnabled", 0, ini.c_str()) != 0);
    trayMiddleClickShowOsd = (GetPrivateProfileIntW(L"Tray", L"MiddleClickShowOsd", 1, ini.c_str()) != 0);
    trayMiddleClickBrightness = GetPrivateProfileIntW(L"Tray", L"MiddleClickBrightness", 50, ini.c_str());
    if (trayMiddleClickBrightness < 0 || trayMiddleClickBrightness > 100) trayMiddleClickBrightness = 50;

    appTheme = GetPrivateProfileIntW(L"General", L"AppTheme", 0, ini.c_str());
    if (appTheme < 0 || appTheme > 2) appTheme = 0;

    transparencyEnabled = (GetPrivateProfileIntW(L"General", L"TransparencyEnabled", 1, ini.c_str()) != 0);

    wchar_t langBuf[64] = { 0 };
    GetPrivateProfileStringW(L"General", L"Language", L"default", langBuf, 64, ini.c_str());
    language = langBuf;
    if (language.empty()) language = L"default";

    wchar_t monBuf[1024] = { 0 };
    GetPrivateProfileStringW(L"Monitors", L"SelectedMonitors", L"all", monBuf, 1024, ini.c_str());
    selectedMonitors = monBuf;
    if (selectedMonitors.empty()) selectedMonitors = L"all";

    overlayTheme = GetPrivateProfileIntW(L"Overlay", L"Theme", 0, ini.c_str());
    if (overlayTheme < 0 || overlayTheme > 2) overlayTheme = 0;

    overlayTimeoutMs = GetPrivateProfileIntW(L"Overlay", L"TimeoutMs", 2000, ini.c_str());
    if (overlayTimeoutMs < 500) overlayTimeoutMs = 500;
    if (overlayTimeoutMs > 10000) overlayTimeoutMs = 10000;

    overlayUseWindowsAccent = GetPrivateProfileIntW(L"Overlay", L"UseWindowsAccent", 1, ini.c_str()) != 0;

    wchar_t colBuf[64] = { 0 };
    GetPrivateProfileStringW(L"Overlay", L"AccentColor", L"#0078D4", colBuf, 64, ini.c_str());
    overlayAccentColor = ParseColor(colBuf, RGB(0, 120, 212));
    overlayUseDualColors = (GetPrivateProfileIntW(L"Overlay", L"UseDualColors", 0, ini.c_str()) != 0);
    GetPrivateProfileStringW(L"Overlay", L"AccentColorDark", L"#60CDFF", colBuf, 64, ini.c_str());
    overlayAccentColorDark = ParseColor(colBuf, RGB(96, 205, 255));

    flyoutUseWindowsAccent = GetPrivateProfileIntW(L"Flyout", L"UseWindowsAccent", 1, ini.c_str()) != 0;

    GetPrivateProfileStringW(L"Flyout", L"AccentColor", L"#0078D4", colBuf, 64, ini.c_str());
    flyoutAccentColor = ParseColor(colBuf, RGB(0, 120, 212));
    flyoutUseDualColors = (GetPrivateProfileIntW(L"Flyout", L"UseDualColors", 0, ini.c_str()) != 0);
    GetPrivateProfileStringW(L"Flyout", L"AccentColorDark", L"#60CDFF", colBuf, 64, ini.c_str());
    flyoutAccentColorDark = ParseColor(colBuf, RGB(96, 205, 255));

    flyoutScrollEnabled = (GetPrivateProfileIntW(L"Flyout", L"ScrollEnabled", 1, ini.c_str()) != 0);
    flyoutScrollStep = GetPrivateProfileIntW(L"Flyout", L"ScrollStep", 5, ini.c_str());
    if (flyoutScrollStep < 1 || flyoutScrollStep > 50) flyoutScrollStep = 5;

    flyoutKeyboardEnabled = (GetPrivateProfileIntW(L"Flyout", L"KeyboardEnabled", 0, ini.c_str()) != 0);

    flyoutScrollDelayEnabled = (GetPrivateProfileIntW(L"Flyout", L"ScrollDelayEnabled", GetPrivateProfileIntW(L"Flyout", L"DelayEnabled", 1, ini.c_str()), ini.c_str()) != 0);
    flyoutScrollDelayMs = GetPrivateProfileIntW(L"Flyout", L"ScrollWaitTimeMs", GetPrivateProfileIntW(L"Flyout", L"WaitTimeMs", 1000, ini.c_str()), ini.c_str());
    if (flyoutScrollDelayMs < 100 || flyoutScrollDelayMs > 3000) flyoutScrollDelayMs = 1000;

    flyoutButtonDelayEnabled = (GetPrivateProfileIntW(L"Flyout", L"ButtonDelayEnabled", 0, ini.c_str()) != 0);
    flyoutButtonDelayMs = GetPrivateProfileIntW(L"Flyout", L"ButtonWaitTimeMs", 500, ini.c_str());
    if (flyoutButtonDelayMs < 100 || flyoutButtonDelayMs > 3000) flyoutButtonDelayMs = 500;

    cachedBrightness = GetPrivateProfileIntW(L"State", L"Brightness", 45, ini.c_str());
    if (cachedBrightness < 0) cachedBrightness = 0;
    if (cachedBrightness > 100) cachedBrightness = 100;

    simulateMonitorsCount = (std::clamp)((int)GetPrivateProfileIntW(L"Debug", L"SimulateMonitors", 0, ini.c_str()), 0, 16);

    // Multi-Monitor Sync & Separate Configs
    wchar_t mainMonBuf[256] = { 0 };
    GetPrivateProfileStringW(L"Monitors", L"MainId", L"", mainMonBuf, 256, ini.c_str());
    mainMonitorId = mainMonBuf;
    syncExternalWithLaptopFnKeys = (GetPrivateProfileIntW(L"Monitors", L"SyncLaptopFnKeys", 0, ini.c_str()) != 0);
    showSeparateInMultiFlyout = (GetPrivateProfileIntW(L"Monitors", L"ShowSeparateInMultiFlyout", 1, ini.c_str()) != 0);

    monitorConfigs.clear();
    wchar_t cfgIdsBuf[2048] = { 0 };
    GetPrivateProfileStringW(L"Monitors", L"ConfiguredIds", L"", cfgIdsBuf, 2048, ini.c_str());
    std::wstring cfgIdsStr = cfgIdsBuf;
    size_t pos = 0;
    while (pos < cfgIdsStr.length()) {
        size_t end = cfgIdsStr.find(L';', pos);
        if (end == std::wstring::npos) end = cfgIdsStr.length();
        std::wstring monId = cfgIdsStr.substr(pos, end - pos);
        pos = end + 1;
        if (monId.empty()) continue;

        std::wstring sec = L"Monitor_" + monId;
        MonitorControlConfig cfg;
        CopyCustomizationToMonitor(cfg);
        if (!mainMonitorId.empty() && _wcsicmp(monId.c_str(), mainMonitorId.c_str()) != 0) {
            ClearHotkeys(cfg);
        }
        cfg.enabled = (GetPrivateProfileIntW(sec.c_str(), L"Enabled", 1, ini.c_str()) != 0);
        wchar_t nameBuf[128] = { 0 };
        GetPrivateProfileStringW(sec.c_str(), L"CustomName", L"", nameBuf, 128, ini.c_str());
        cfg.customName = SanitizeMonitorName(nameBuf);
        cfg.controlMode = GetPrivateProfileIntW(sec.c_str(), L"ControlMode", 0, ini.c_str());
        if (cfg.controlMode < 0 || cfg.controlMode > 1) cfg.controlMode = 0;
        cfg.offset = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"Offset", 0, ini.c_str()), -100, 100);
        cfg.syncOverscrollMax = (GetPrivateProfileIntW(sec.c_str(), L"SyncOverscrollMax", 1, ini.c_str()) != 0);
        cfg.syncOverscrollMin = (GetPrivateProfileIntW(sec.c_str(), L"SyncOverscrollMin", 1, ini.c_str()) != 0);
        bool legacySyncSeparateTray = (GetPrivateProfileIntW(sec.c_str(), L"SyncSeparateTrayIcon", 0, ini.c_str()) != 0);

        // Shortcuts
        cfg.hotkeyAdjustEnabled = (GetPrivateProfileIntW(sec.c_str(), L"HotkeyAdjustEnabled", cfg.hotkeyAdjustEnabled ? 1 : 0, ini.c_str()) != 0);
        cfg.hotkeyAdjustShowOsd = (GetPrivateProfileIntW(sec.c_str(), L"HotkeyAdjustShowOsd", cfg.hotkeyAdjustShowOsd ? 1 : 0, ini.c_str()) != 0);
        cfg.hotkeyUpMods = GetPrivateProfileIntW(sec.c_str(), L"HotkeyUpMods", cfg.hotkeyUpMods, ini.c_str());
        cfg.hotkeyUpKey = GetPrivateProfileIntW(sec.c_str(), L"HotkeyUpKey", cfg.hotkeyUpKey, ini.c_str());
        cfg.hotkeyDownMods = GetPrivateProfileIntW(sec.c_str(), L"HotkeyDownMods", cfg.hotkeyDownMods, ini.c_str());
        cfg.hotkeyDownKey = GetPrivateProfileIntW(sec.c_str(), L"HotkeyDownKey", cfg.hotkeyDownKey, ini.c_str());
        cfg.hotkeyPresetsEnabled = (GetPrivateProfileIntW(sec.c_str(), L"HotkeyPresetsEnabled", cfg.hotkeyPresetsEnabled ? 1 : 0, ini.c_str()) != 0);
        cfg.hotkeyPresetsShowOsd = (GetPrivateProfileIntW(sec.c_str(), L"HotkeyPresetsShowOsd", cfg.hotkeyPresetsShowOsd ? 1 : 0, ini.c_str()) != 0);
        cfg.hotkeyPreset1Mods = GetPrivateProfileIntW(sec.c_str(), L"HotkeyPreset1Mods", cfg.hotkeyPreset1Mods, ini.c_str());
        cfg.hotkeyPreset1Key = GetPrivateProfileIntW(sec.c_str(), L"HotkeyPreset1Key", cfg.hotkeyPreset1Key, ini.c_str());
        cfg.hotkeyPreset1Brightness = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"HotkeyPreset1Brightness", cfg.hotkeyPreset1Brightness, ini.c_str()), 0, 100);
        cfg.hotkeyPreset2Mods = GetPrivateProfileIntW(sec.c_str(), L"HotkeyPreset2Mods", cfg.hotkeyPreset2Mods, ini.c_str());
        cfg.hotkeyPreset2Key = GetPrivateProfileIntW(sec.c_str(), L"HotkeyPreset2Key", cfg.hotkeyPreset2Key, ini.c_str());
        cfg.hotkeyPreset2Brightness = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"HotkeyPreset2Brightness", cfg.hotkeyPreset2Brightness, ini.c_str()), 0, 100);
        cfg.hotkeyPresetToggleEnabled = (GetPrivateProfileIntW(sec.c_str(), L"HotkeyPresetToggleEnabled", cfg.hotkeyPresetToggleEnabled ? 1 : 0, ini.c_str()) != 0);
        cfg.hotkeyPresetToggleShowOsd = (GetPrivateProfileIntW(sec.c_str(), L"HotkeyPresetToggleShowOsd", cfg.hotkeyPresetToggleShowOsd ? 1 : 0, ini.c_str()) != 0);
        cfg.hotkeyPresetToggleMods = GetPrivateProfileIntW(sec.c_str(), L"HotkeyPresetToggleMods", cfg.hotkeyPresetToggleMods, ini.c_str());
        cfg.hotkeyPresetToggleKey = GetPrivateProfileIntW(sec.c_str(), L"HotkeyPresetToggleKey", cfg.hotkeyPresetToggleKey, ini.c_str());
        cfg.hotkeyPresetToggle1Brightness = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"HotkeyPresetToggle1Brightness", cfg.hotkeyPresetToggle1Brightness, ini.c_str()), 0, 100);
        cfg.hotkeyPresetToggle2Brightness = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"HotkeyPresetToggle2Brightness", cfg.hotkeyPresetToggle2Brightness, ini.c_str()), 0, 100);
        cfg.hotkeyStep = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"HotkeyStep", cfg.hotkeyStep, ini.c_str()), 1, 50);

        // OSD
        cfg.overlayUseWindowsAccent = (GetPrivateProfileIntW(sec.c_str(), L"OsdUseWindowsAccent", cfg.overlayUseWindowsAccent ? 1 : 0, ini.c_str()) != 0);
        GetPrivateProfileStringW(sec.c_str(), L"OsdAccentColor", FormatColor(cfg.overlayAccentColor).c_str(), colBuf, 64, ini.c_str());
        cfg.overlayAccentColor = ParseColor(colBuf, cfg.overlayAccentColor);
        cfg.overlayUseDualColors = (GetPrivateProfileIntW(sec.c_str(), L"OsdUseDualColors", cfg.overlayUseDualColors ? 1 : 0, ini.c_str()) != 0);
        GetPrivateProfileStringW(sec.c_str(), L"OsdAccentColorDark", FormatColor(cfg.overlayAccentColorDark).c_str(), colBuf, 64, ini.c_str());
        cfg.overlayAccentColorDark = ParseColor(colBuf, cfg.overlayAccentColorDark);

        // Tray
        bool isMainMon = (!mainMonitorId.empty() && _wcsicmp(monId.c_str(), mainMonitorId.c_str()) == 0);
        int defaultPctMode = (!isMainMon && cfg.controlMode == 0 && !legacySyncSeparateTray) ? 3 : cfg.trayPercentageMode;
        cfg.trayPercentageMode = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"TrayPercentageMode", defaultPctMode, ini.c_str()), 0, 3);
        if (isMainMon && cfg.trayPercentageMode == 3) {
            cfg.trayPercentageMode = 0;
        }
        GetPrivateProfileStringW(sec.c_str(), L"TrayIconColor", FormatColor(cfg.trayIconColor).c_str(), colBuf, 64, ini.c_str());
        cfg.trayIconColor = ParseColor(colBuf, cfg.trayIconColor);
        cfg.trayIconUseDualColors = (GetPrivateProfileIntW(sec.c_str(), L"TrayUseDualColors", cfg.trayIconUseDualColors ? 1 : 0, ini.c_str()) != 0);
        GetPrivateProfileStringW(sec.c_str(), L"TrayIconColorDark", FormatColor(cfg.trayIconColorDark).c_str(), colBuf, 64, ini.c_str());
        cfg.trayIconColorDark = ParseColor(colBuf, cfg.trayIconColorDark);
        wchar_t mFontBuf[128] = { 0 };
        GetPrivateProfileStringW(sec.c_str(), L"TrayFont", cfg.trayFont.c_str(), mFontBuf, 128, ini.c_str());
        if (mFontBuf[0] != L'\0') cfg.trayFont = mFontBuf;
        cfg.trayFontWeight = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"TrayFontWeight", cfg.trayFontWeight, ini.c_str()), 100, 900);
        cfg.trayFontSize = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"TrayFontSize", cfg.trayFontSize, ini.c_str()), 40, 150);
        cfg.trayFontSize100 = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"TrayFontSize100", cfg.trayFontSize100, ini.c_str()), 30, 130);
        wchar_t yBuf[64] = { 0 };
        GetPrivateProfileStringW(sec.c_str(), L"TrayYOffset", FormatYOffset(cfg.trayYOffset, cfg.trayYOffsetUseComma).c_str(), yBuf, 64, ini.c_str());
        bool uc = false;
        cfg.trayYOffset = ParseYOffset(yBuf, &uc);
        cfg.trayYOffsetUseComma = uc;
        cfg.trayWheelEnabled = (GetPrivateProfileIntW(sec.c_str(), L"TrayWheelEnabled", cfg.trayWheelEnabled ? 1 : 0, ini.c_str()) != 0);
        cfg.trayWheelStep = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"TrayWheelStep", cfg.trayWheelStep, ini.c_str()), 1, 50);
        cfg.trayWheelShowOsd = (GetPrivateProfileIntW(sec.c_str(), L"TrayWheelShowOsd", cfg.trayWheelShowOsd ? 1 : 0, ini.c_str()) != 0);
        cfg.trayMiddleClickEnabled = (GetPrivateProfileIntW(sec.c_str(), L"TrayMiddleClickEnabled", cfg.trayMiddleClickEnabled ? 1 : 0, ini.c_str()) != 0);
        cfg.trayMiddleClickShowOsd = (GetPrivateProfileIntW(sec.c_str(), L"TrayMiddleClickShowOsd", cfg.trayMiddleClickShowOsd ? 1 : 0, ini.c_str()) != 0);
        cfg.trayMiddleClickBrightness = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"TrayMiddleClickBrightness", cfg.trayMiddleClickBrightness, ini.c_str()), 0, 100);

        // Flyout
        cfg.flyoutUseWindowsAccent = (GetPrivateProfileIntW(sec.c_str(), L"FlyoutUseWindowsAccent", cfg.flyoutUseWindowsAccent ? 1 : 0, ini.c_str()) != 0);
        GetPrivateProfileStringW(sec.c_str(), L"FlyoutAccentColor", FormatColor(cfg.flyoutAccentColor).c_str(), colBuf, 64, ini.c_str());
        cfg.flyoutAccentColor = ParseColor(colBuf, cfg.flyoutAccentColor);
        cfg.flyoutUseDualColors = (GetPrivateProfileIntW(sec.c_str(), L"FlyoutUseDualColors", cfg.flyoutUseDualColors ? 1 : 0, ini.c_str()) != 0);
        GetPrivateProfileStringW(sec.c_str(), L"FlyoutAccentColorDark", FormatColor(cfg.flyoutAccentColorDark).c_str(), colBuf, 64, ini.c_str());
        cfg.flyoutAccentColorDark = ParseColor(colBuf, cfg.flyoutAccentColorDark);
        cfg.flyoutScrollEnabled = (GetPrivateProfileIntW(sec.c_str(), L"FlyoutScrollEnabled", cfg.flyoutScrollEnabled ? 1 : 0, ini.c_str()) != 0);
        cfg.flyoutScrollStep = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"FlyoutScrollStep", cfg.flyoutScrollStep, ini.c_str()), 1, 50);

        cfg.cachedBrightness = (std::clamp)((int)GetPrivateProfileIntW(sec.c_str(), L"CachedBrightness", cachedBrightness, ini.c_str()), 0, 100);
        monitorConfigs[monId] = cfg;
    }



    bool regStartWithWin = IsStartWithWindowsEnabled();
    if (!IsInstalledInProgramFiles()) {
        // Not running from Program Files (e.g. debug or portable build).
        // Block modifications to startup registry; keep existing startup state if already active.
        int iniStartWithWin = GetPrivateProfileIntW(L"General", L"StartWithWindows", -1, ini.c_str());
        if (regStartWithWin) {
            startWithWindows = true;
        } else if (iniStartWithWin != -1) {
            startWithWindows = (iniStartWithWin == 1);
        } else {
            startWithWindows = false;
        }
    } else {
        int iniStartWithWin = GetPrivateProfileIntW(L"General", L"StartWithWindows", -1, ini.c_str());
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
    addLine(L"SelectedMonitors", selectedMonitors);
    addLine(L"MainId", mainMonitorId);
    addLine(L"SyncLaptopFnKeys", syncExternalWithLaptopFnKeys ? L"1" : L"0");
    addLine(L"ShowSeparateInMultiFlyout", showSeparateInMultiFlyout ? L"1" : L"0");
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
    addLine(L"UseWindowsAccent", overlayUseWindowsAccent ? L"1" : L"0");
    addLine(L"AccentColor", FormatColor(overlayAccentColor));
    addLine(L"UseDualColors", overlayUseDualColors ? L"1" : L"0");
    addLine(L"AccentColorDark", FormatColor(overlayAccentColorDark));

    addSec(L"Tray");
    addLine(L"PercentageMode", std::to_wstring(trayPercentageMode));
    addLine(L"IconColor", FormatColor(trayIconColor));
    addLine(L"UseDualColors", trayIconUseDualColors ? L"1" : L"0");
    addLine(L"IconColorDark", FormatColor(trayIconColorDark));
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

void AppSettings::SaveBrightnessOnly() const {
    Save();
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
    if (res != ERROR_SUCCESS) {
        res = RegQueryValueExW(hKey, L"BrightnessApp", NULL, &type, NULL, &dataSize);
    }
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
    if (!IsInstalledInProgramFiles()) {
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

    // Clean up legacy BrightnessApp registry value if present
    RegDeleteValueW(hKey, L"BrightnessApp");

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
    if (selectedMonitors.empty() || _wcsicmp(selectedMonitors.c_str(), L"all") == 0) {
        return true;
    }
    size_t start = 0;
    while (start < selectedMonitors.length()) {
        size_t end = selectedMonitors.find(L';', start);
        if (end == std::wstring::npos) end = selectedMonitors.length();
        std::wstring token = selectedMonitors.substr(start, end - start);
        while (!token.empty() && iswspace(token.front())) token.erase(token.begin());
        while (!token.empty() && iswspace(token.back())) token.pop_back();
        if (_wcsicmp(token.c_str(), monitorId.c_str()) == 0) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

void AppSettings::SetMonitorSelected(const std::wstring& monitorId, bool selected, const std::vector<std::wstring>& allIds) {
    std::vector<std::wstring> curSelected;
    if (selectedMonitors.empty() || _wcsicmp(selectedMonitors.c_str(), L"all") == 0) {
        curSelected = allIds;
    } else {
        size_t start = 0;
        while (start < selectedMonitors.length()) {
            size_t end = selectedMonitors.find(L';', start);
            if (end == std::wstring::npos) end = selectedMonitors.length();
            std::wstring token = selectedMonitors.substr(start, end - start);
            while (!token.empty() && iswspace(token.front())) token.erase(token.begin());
            while (!token.empty() && iswspace(token.back())) token.pop_back();
            if (!token.empty()) {
                curSelected.push_back(token);
            }
            start = end + 1;
        }
    }

    if (selected) {
        bool exists = false;
        for (const auto& id : curSelected) {
            if (_wcsicmp(id.c_str(), monitorId.c_str()) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            curSelected.push_back(monitorId);
        }
    } else {
        curSelected.erase(std::remove_if(curSelected.begin(), curSelected.end(), [&](const std::wstring& id) {
            return _wcsicmp(id.c_str(), monitorId.c_str()) == 0;
        }), curSelected.end());
    }

    bool hasAll = (!allIds.empty());
    for (const auto& id : allIds) {
        bool found = false;
        for (const auto& sel : curSelected) {
            if (_wcsicmp(sel.c_str(), id.c_str()) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            hasAll = false;
            break;
        }
    }

    if (hasAll) {
        selectedMonitors = L"all";
    } else {
        std::wstring joined;
        for (size_t i = 0; i < curSelected.size(); ++i) {
            if (i > 0) joined += L";";
            joined += curSelected[i];
        }
        selectedMonitors = joined;
    }
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
    cfg.enabled = IsMonitorSelected(monitorId);
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
        result.trayFont = own.trayFont;
        result.trayFontWeight = own.trayFontWeight;
        result.trayFontSize = own.trayFontSize;
        result.trayFontSize100 = own.trayFontSize100;
        result.trayYOffset = own.trayYOffset;
        result.trayYOffsetUseComma = own.trayYOffsetUseComma;
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
    cachedBrightness = src.cachedBrightness;
}


