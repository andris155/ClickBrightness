#pragma once

#include "FlyoutWindow.g.h"
#include "FlyoutWindow.xaml.g.h"
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>

namespace winrt::BrightnessApp::implementation
{
    struct FlyoutWindow : FlyoutWindowT<FlyoutWindow>
    {
        FlyoutWindow();
        ~FlyoutWindow();

        void ShowWindow(const POINT* pTrayPos);
        void HideWindow();
        void DestroyWindow();
        bool IsWindowVisible() const;
        ULONGLONG GetHideTick() const { return m_hideTick; }

        void SetBrightness(int brightness, bool notifyHardware);
        void CommitBrightness(bool immediate = false);
        void ApplyPendingBrightness();
        void UpdateTheme();
        void ResetGraphics();

    private:
        void SetupWindow();
        void SetupBackdrop();
        void DetachBackdrop();
        void InitializeButtons();
        void SetBrightnessVisual(int val);
        void CommitHardwareBrightness(int val);
        void ScheduleDebounce(int val, int delayMs);
        void OnScrollAdjust(int val);
        void OnButtonClicked(int val);

        static LRESULT CALLBACK SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

        HWND m_hwnd{ nullptr };
        Microsoft::UI::Windowing::AppWindow m_appWindow{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController m_acrylicController{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration m_backdropConfig{ nullptr };

        int m_brightness{ 45 };
        int m_pendingBrightness{ 45 };
        bool m_hasPendingBrightness{ false };
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
    };
}

namespace winrt::BrightnessApp::factory_implementation
{
    struct FlyoutWindow : FlyoutWindowT<FlyoutWindow, implementation::FlyoutWindow>
    {
    };
}
