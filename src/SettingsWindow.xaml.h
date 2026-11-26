#pragma once

#include "SettingsWindow.g.h"
#include "SettingsWindow.xaml.g.h"
#include <winrt/Microsoft.UI.Windowing.h>

namespace winrt::BrightnessApp::implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow>
    {
        SettingsWindow();
        ~SettingsWindow();

        void ShowWindow();
        void HideWindow();
        void LoadSettingsToUI();
        void ApplyAndSave();
        void ScheduleSave(bool reRegisterHotkeys = false, bool syncTray = false);
        void FlushPendingSave();
        void ApplyTheme();
        void OnDisplayChanged();

        void StartCapture(int target); // 1 = Up, 2 = Down, 3 = Preset1, 4 = Preset2
        void StopCapture(bool saveKey);
        void UpdateCaptureHint(UINT mods);
        void HandleCapturedKey(UINT vkCode, UINT mods);
        void SetExiting(bool exiting) { m_isAppExiting = exiting; }
        bool IsClosing() const { return m_isClosing; }
        void SetClosing(bool closing) { m_isClosing = closing; }

    private:
        void InitializeControls();
        void SetupWindow();
        void SetupBackdrop();
        void UpdateTitleBarButtons(bool isDark);
        void PopulateMonitorsInfo();
        void UpdateLocalizedStrings();
        void PopulateLanguageList();
        void PopulateMonitorsSelectionList();
        void UpdateMonitorsSelectionUI();
        void LoadMonitorConfigToUI();
        void SaveCurrentMonitorConfigFromUI();
        void OpenMonitorsSubpage(const std::wstring& subpageTag);
        void OpenTimingsSubpage(const std::wstring& subpageTag);
        void UpdateTitleBarDragRegions();
        void SetBackButtonUsable(bool usable);

        void EnsureTrayTabLoaded();
        void EnsureAboutTabLoaded();

        static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
        static LRESULT CALLBACK SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

        HWND m_hwnd{ nullptr };
        Microsoft::UI::Windowing::AppWindow m_appWindow{ nullptr };
        HICON m_hIconBig{ nullptr };
        HICON m_hIconSmall{ nullptr };

        int m_captureTarget{ 0 }; // 0 = None, 1 = Up, 2 = Down, 3 = Preset1, 4 = Preset2, 5 = PresetToggle
        UINT m_curUpKey{ 0x7D }; // VK_F14
        UINT m_curUpMods{ 0 };
        UINT m_curDownKey{ 0x7C }; // VK_F13
        UINT m_curDownMods{ 0 };
        UINT m_curPreset1Key{ 0x7E }; // VK_F15
        UINT m_curPreset1Mods{ 0 };
        UINT m_curPreset2Key{ 0x7F }; // VK_F16
        UINT m_curPreset2Mods{ 0 };
        UINT m_curPresetToggleKey{ 0x80 }; // VK_F17
        UINT m_curPresetToggleMods{ 0 };
        bool m_isUpdatingUI{ false };
        bool m_pendingSave{ false };
        bool m_pendingSaveHotkeys{ false };
        bool m_pendingSaveTray{ false };
        bool m_isAppExiting{ false };
        bool m_isClosing{ false };
        bool m_trayTabLoaded{ false };
        bool m_aboutTabLoaded{ false };
        std::vector<std::wstring> m_cachedMonitorIds;
        std::wstring m_selectedConfigMonitorId;
        std::wstring m_currentMonitorsSubpage;
        std::wstring m_currentTimingsSubpage;

        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_generalPanel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_monitorsPanel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_monitorsMainView{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_monitorsSubpageView{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_timingsPanel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_timingsMainView{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_timingsSubpageView{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_aboutPanel{ nullptr };
    };
}

namespace winrt::BrightnessApp::factory_implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow>
    {
    };
}

void InitSettingsWindow(bool show);
void ShowSettingsWindow();
void CloseSettingsWindow();
bool IsSettingsWindowOpen();
void OnSettingsThemeChanged();
void OnSettingsDisplayChanged();
void TrimProcessWorkingSetAsync();
