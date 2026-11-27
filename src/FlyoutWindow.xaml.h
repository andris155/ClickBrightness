#pragma once

#include "FlyoutWindow.g.h"
#include "FlyoutWindow.xaml.g.h"
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <functional>

namespace winrt::ClickBrightness::implementation
{
    struct FlyoutWindow : FlyoutWindowT<FlyoutWindow>
    {
        FlyoutWindow();
        ~FlyoutWindow();

        void SetTargetMonitor(const std::wstring& targetMonitorId, UINT trayIconId = 0) { m_targetMonitorId = targetMonitorId; m_targetTrayId = trayIconId; }
        const std::wstring& GetTargetMonitor() const { return m_targetMonitorId; }
        void Prewarm();
        void RefreshLayoutInPlace();
        void ShowWindow(const POINT* pTrayPos);
        void HideWindow();
        void DestroyWindow();
        bool IsWindowVisible() const;
        ULONGLONG GetHideTick() const { return m_hideTick; }

        void SetBrightness(int brightness, bool notifyHardware);
        void SetBrightnessForTarget(const std::wstring& targetMonitorId, int brightness, bool notifyHardware);
        void CommitBrightness(bool immediate = false);
        void ApplyPendingBrightness();
        void UpdateTheme();
        void ResetGraphics();

    private:
        void SetupWindow();
        void SetupBackdrop();
        void DetachBackdrop();
        void InitializeButtons();
        void PopulatePresetButtons(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& panel, std::function<void(int)> onClick);
        bool RebuildSyncMonitorsPanel();
        void SetBrightnessVisual(int val);
        void ApplySliderBrushes();
        void ApplySliderBrushesForControl(winrt::Microsoft::UI::Xaml::Controls::Slider const& slider, const std::wstring& monitorId);
        void CommitHardwareBrightness(int val);
        void ScheduleDebounce(int val, int delayMs);
        void CancelPendingShow();
        void OnScrollAdjust(int val);
        void OnButtonClicked(int val);

        static LRESULT CALLBACK SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

        HWND m_hwnd{ nullptr };
        Microsoft::UI::Windowing::AppWindow m_appWindow{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController m_acrylicController{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration m_backdropConfig{ nullptr };

        int m_brightness{ 45 };
        int m_syncBaseBrightness{ 45 };
        int m_pendingBrightness{ 45 };
        std::wstring m_targetMonitorId;
        UINT m_targetTrayId{ 0 };
        std::wstring m_lastSyncSignature;
        bool m_hasPendingBrightness{ false };
        bool m_hasSeparatePendingSave{ false };
        bool m_isUpdatingSlider{ false };
        bool m_isDraggingSlider{ false };
        bool m_isVisible{ false };
        ULONGLONG m_showTick{ 0 };
        ULONGLONG m_hideTick{ 0 };

        int m_lastTheme{ -1 };
        int m_lastTransparency{ -1 };
        COLORREF m_lastAccentCol{ 0 };
        int m_lastW{ 0 };
        int m_lastH{ 0 };
        winrt::event_token m_showRenderToken{};
        int m_pendingShowFrames{ 0 };
        int m_pendingShowX{ 0 };
        int m_pendingShowY{ 0 };

        struct SyncSliderEntry
        {
            std::wstring monitorId;
            std::wstring displayName;
            bool isSeparate{ false };
            winrt::Microsoft::UI::Xaml::Controls::TextBlock header{ nullptr };
            winrt::Microsoft::UI::Xaml::Controls::Slider slider{ nullptr };
            winrt::Microsoft::UI::Xaml::Controls::StackPanel sepButtons{ nullptr };
        };
        std::vector<SyncSliderEntry> m_syncEntries;
    };
}

namespace winrt::ClickBrightness::factory_implementation
{
    struct FlyoutWindow : FlyoutWindowT<FlyoutWindow, implementation::FlyoutWindow>
    {
    };
}
