#pragma once

#include <windows.h>
#include <string>

struct AppSettings {
    // Hotkey Stepping (Up/Down) Enabled
    bool hotkeyAdjustEnabled = false;  // Default: OFF
    bool hotkeyAdjustShowOsd = true;   // Default: ON (Show OSD banner on increase/decrease)

    // Hotkey Up
    UINT hotkeyUpMods = 0;             // 0 = none, MOD_CONTROL, MOD_ALT, etc.
    UINT hotkeyUpKey = VK_F14;         // Default F14
    
    // Hotkey Down
    UINT hotkeyDownMods = 0;           // 0 = none
    UINT hotkeyDownKey = VK_F13;       // Default F13

    // Hotkey Presets Enabled
    bool hotkeyPresetsEnabled = false; // Default: OFF
    bool hotkeyPresetsShowOsd = true;  // Default: ON (Show OSD banner on preset shortcuts)

    // Hotkey Presets
    UINT hotkeyPreset1Mods = 0;        // 0 = none
    UINT hotkeyPreset1Key = VK_F15;    // Default F15
    int hotkeyPreset1Brightness = 30;  // Default: 30%

    UINT hotkeyPreset2Mods = 0;        // 0 = none
    UINT hotkeyPreset2Key = VK_F16;    // Default F16
    int hotkeyPreset2Brightness = 80;  // Default: 80%

    // Hotkey Preset Toggle Enabled
    bool hotkeyPresetToggleEnabled = false; // Default: OFF
    bool hotkeyPresetToggleShowOsd = true;  // Default: ON (Show OSD banner on preset toggle shortcut)

    // Hotkey Preset Toggle
    UINT hotkeyPresetToggleMods = 0;        // 0 = none
    UINT hotkeyPresetToggleKey = VK_F17;    // Default F17
    int hotkeyPresetToggle1Brightness = 30; // Default: 30%
    int hotkeyPresetToggle2Brightness = 80; // Default: 80%

    // Hotkey Step
    int hotkeyStep = 5;                // Default 5%

    // Hotkey Debounce / Delay
    bool hotkeyDelayEnabled = true;    // Default: ON
    int hotkeyDelayMs = 1000;          // Default: 1000 ms (1 second)

    // Hotkey Continuous Repeat / Hold Interval (throttle)
    int hotkeyRepeatIntervalMs = 90;   // Default: 90 ms (~11 steps/sec, ~1.8s for 0-100% with step=5)

    // Tray Appearance
    bool trayShowPercentage = true;                    // Default: ON (Show live brightness percentage)
    std::wstring trayFont = L"Segoe UI Variable Text"; // Default font: Segoe UI Variable Text
    int trayFontSize = 102;                            // Default: 102% (1-2 digits)
    int trayFontSize100 = 63;                          // Default: 63% (3 digits / 100)
    float trayYOffset = -0.5f;                         // Default: -0.5 (px vertical offset, auto-scales with DPI)
    bool trayYOffsetUseComma = false;                  // True if decimal comma format preferred (e.g. 1,5)

    // Tray Mouse Wheel Scrolling
    bool trayWheelEnabled = false;                     // Default: OFF
    int trayWheelStep = 5;                             // Default: 5%
    bool trayWheelShowOsd = true;                      // Default: ON (Show OSD banner when scrolling)
    bool trayWheelDelayEnabled = true;                 // Default: ON
    int trayWheelDelayMs = 1000;                       // Default: 1000 ms (1 second)

    // Tray Middle-Click Quick Action
    bool trayMiddleClickEnabled = false;               // Default: OFF
    bool trayMiddleClickShowOsd = true;                // Default: ON (Show OSD banner on middle-click)
    int trayMiddleClickBrightness = 50;                // Default: 50%

    // App Theme (0 = Follow Windows (app), 1 = Dark, 2 = Light)
    int appTheme = 0;

    // Transparency Effects (Mica & Acrylic Backdrops)
    bool transparencyEnabled = true;   // Default: ON

    // Display Language ("default" = Windows display language, or "en-US", etc.)
    std::wstring language = L"default";

    // Target Monitors to Control ("all", or semicolon-separated monitor IDs)
    std::wstring selectedMonitors = L"all";

    // OSD & Tray Menu Theme (0 = Follow Windows (system), 1 = Dark, 2 = Light)
    int overlayTheme = 0;
    int overlayTimeoutMs = 2000;             // Default: 2000 ms (2 seconds)
    bool overlayUseWindowsAccent = true;     // Default: Use Windows accent color
    COLORREF overlayAccentColor = RGB(0, 120, 212); // Default: #0078D4

    // Flyout Settings - Accent Color & Debounce
    bool flyoutUseWindowsAccent = true;      // Default: Use Windows accent color
    COLORREF flyoutAccentColor = RGB(0, 120, 212);  // Default: #0078D4
    bool flyoutScrollEnabled = true;         // Default: ON
    int flyoutScrollStep = 5;                // Default: 5%
    bool flyoutScrollDelayEnabled = true;    // Default: ON
    int flyoutScrollDelayMs = 1000;          // Default: 1000 ms (1 second)
    bool flyoutButtonDelayEnabled = false;   // Default: OFF
    int flyoutButtonDelayMs = 500;           // Default: 500 ms (0.5 seconds)

    // Last known brightness (fallback cache)
    int cachedBrightness = 45;

    // Start with Windows (Startup Registry)
    bool startWithWindows = false;

    void Load();
    void Save() const;
    void SaveBrightnessOnly() const;
    static const std::wstring& GetIniFilePath();

    static bool IsStartWithWindowsEnabled();
    static bool SetStartWithWindows(bool enable);
    static bool IsInstalledInProgramFiles();

    static std::wstring FormatYOffset(float val, bool useComma);
    static float ParseYOffset(const std::wstring& str, bool* outUseComma = nullptr);

    static COLORREF ParseColor(const std::wstring& str, COLORREF defaultColor);
    static std::wstring FormatColor(COLORREF c);

    bool IsMonitorSelected(const std::wstring& monitorId) const;
    void SetMonitorSelected(const std::wstring& monitorId, bool selected, const std::vector<std::wstring>& allIds);
};

extern AppSettings g_Settings;
