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

    trayShowPercentage = (GetPrivateProfileIntW(L"Tray", L"ShowPercentage", 1, ini.c_str()) != 0);

    wchar_t fontBuf[128] = { 0 };
    GetPrivateProfileStringW(L"Tray", L"Font", L"Segoe UI Variable Text", fontBuf, 128, ini.c_str());
    trayFont = fontBuf;
    if (trayFont.empty()) trayFont = L"Segoe UI Variable Text";

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

    flyoutUseWindowsAccent = GetPrivateProfileIntW(L"Flyout", L"UseWindowsAccent", 1, ini.c_str()) != 0;

    GetPrivateProfileStringW(L"Flyout", L"AccentColor", L"#0078D4", colBuf, 64, ini.c_str());
    flyoutAccentColor = ParseColor(colBuf, RGB(0, 120, 212));

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

    WritePrivateProfileStringW(L"General", L"StartWithWindows", startWithWindows ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"General", L"AppTheme", std::to_wstring(appTheme).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"General", L"TransparencyEnabled", transparencyEnabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"General", L"Language", language.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Monitors", L"SelectedMonitors", selectedMonitors.c_str(), ini.c_str());

    WritePrivateProfileStringW(L"Hotkeys", L"AdjustEnabled", hotkeyAdjustEnabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"AdjustShowOsd", hotkeyAdjustShowOsd ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"KeyUp", std::to_wstring(hotkeyUpKey).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"ModsUp", std::to_wstring(hotkeyUpMods).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"KeyDown", std::to_wstring(hotkeyDownKey).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"ModsDown", std::to_wstring(hotkeyDownMods).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"PresetsEnabled", hotkeyPresetsEnabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"PresetsShowOsd", hotkeyPresetsShowOsd ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"KeyPreset1", std::to_wstring(hotkeyPreset1Key).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"ModsPreset1", std::to_wstring(hotkeyPreset1Mods).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"Preset1Brightness", std::to_wstring(hotkeyPreset1Brightness).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"KeyPreset2", std::to_wstring(hotkeyPreset2Key).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"ModsPreset2", std::to_wstring(hotkeyPreset2Mods).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"Preset2Brightness", std::to_wstring(hotkeyPreset2Brightness).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"PresetToggleEnabled", hotkeyPresetToggleEnabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"PresetToggleShowOsd", hotkeyPresetToggleShowOsd ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"KeyPresetToggle", std::to_wstring(hotkeyPresetToggleKey).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"ModsPresetToggle", std::to_wstring(hotkeyPresetToggleMods).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"PresetToggle1Brightness", std::to_wstring(hotkeyPresetToggle1Brightness).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"PresetToggle2Brightness", std::to_wstring(hotkeyPresetToggle2Brightness).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"Step", std::to_wstring(hotkeyStep).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"RepeatIntervalMs", std::to_wstring(hotkeyRepeatIntervalMs).c_str(), ini.c_str());

    WritePrivateProfileStringW(L"Delay", L"Enabled", hotkeyDelayEnabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Delay", L"WaitTimeMs", std::to_wstring(hotkeyDelayMs).c_str(), ini.c_str());

    WritePrivateProfileStringW(L"Overlay", L"Theme", std::to_wstring(overlayTheme).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Overlay", L"TimeoutMs", std::to_wstring(overlayTimeoutMs).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Overlay", L"UseWindowsAccent", overlayUseWindowsAccent ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Overlay", L"AccentColor", FormatColor(overlayAccentColor).c_str(), ini.c_str());

    WritePrivateProfileStringW(L"Tray", L"ShowPercentage", trayShowPercentage ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Tray", L"Font", trayFont.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Tray", L"FontSize", std::to_wstring(trayFontSize).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Tray", L"FontSize100", std::to_wstring(trayFontSize100).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Tray", L"YOffset", FormatYOffset(trayYOffset, trayYOffsetUseComma).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Tray", L"WheelEnabled", trayWheelEnabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Tray", L"WheelStep", std::to_wstring(trayWheelStep).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Tray", L"WheelShowOsd", trayWheelShowOsd ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Tray", L"WheelDelayEnabled", trayWheelDelayEnabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Tray", L"WheelWaitTimeMs", std::to_wstring(trayWheelDelayMs).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Tray", L"MiddleClickEnabled", trayMiddleClickEnabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Tray", L"MiddleClickShowOsd", trayMiddleClickShowOsd ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Tray", L"MiddleClickBrightness", std::to_wstring(trayMiddleClickBrightness).c_str(), ini.c_str());

    // Flyout Accent Color & Debounce
    WritePrivateProfileStringW(L"Flyout", L"UseWindowsAccent", flyoutUseWindowsAccent ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Flyout", L"AccentColor", FormatColor(flyoutAccentColor).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Flyout", L"ScrollEnabled", flyoutScrollEnabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Flyout", L"ScrollStep", std::to_wstring(flyoutScrollStep).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Flyout", L"KeyboardEnabled", flyoutKeyboardEnabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Flyout", L"ScrollDelayEnabled", flyoutScrollDelayEnabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Flyout", L"ScrollWaitTimeMs", std::to_wstring(flyoutScrollDelayMs).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Flyout", L"ButtonDelayEnabled", flyoutButtonDelayEnabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Flyout", L"ButtonWaitTimeMs", std::to_wstring(flyoutButtonDelayMs).c_str(), ini.c_str());

    WritePrivateProfileStringW(L"State", L"Brightness", std::to_wstring(cachedBrightness).c_str(), ini.c_str());
}

void AppSettings::SaveBrightnessOnly() const {
    std::wstring ini = GetIniFilePath();
    WritePrivateProfileStringW(L"State", L"Brightness", std::to_wstring(cachedBrightness).c_str(), ini.c_str());
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


