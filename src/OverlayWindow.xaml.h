#pragma once

#include "OverlayWindow.g.h"
#include "OverlayWindow.xaml.g.h"
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>

namespace winrt::BrightnessApp::implementation
{
    struct OverlayWindow : OverlayWindowT<OverlayWindow>
    {
        OverlayWindow();
        ~OverlayWindow();

        void TriggerBrightnessDelta(int delta, bool showOsd = true);
        void TriggerTrayWheel(int delta);
        void TriggerBrightnessSet(int target, bool showOsd = true);
        void ApplyPendingBrightness();
        void UpdateContent(int brightness);
        void UpdateTheme();
        void ResetGraphics();
        void PositionWindow();
        void HideWindow();
        void DestroyWindow();

    private:
        void SetupWindow();
        void SetupBackdrop();
        void DetachBackdrop();
        static LRESULT CALLBACK SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

        HWND m_hwnd{ nullptr };
        Microsoft::UI::Windowing::AppWindow m_appWindow{ nullptr };

        Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController m_acrylicController{ nullptr };
        Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration m_backdropConfig{ nullptr };

        int m_displayedBrightness{ 45 };
        int m_pendingBrightness{ 45 };
        bool m_hasPendingChange{ false };
        bool m_isVisible{ false };

        int m_lastTheme{ -1 };
        int m_lastTransparency{ -1 };
        COLORREF m_lastAccentCol{ 0 };
        int m_lastX{ -999999 };
        int m_lastY{ -999999 };
        int m_lastW{ 0 };
        int m_lastH{ 0 };
        ULONGLONG m_hideAfterTick{ 0 };
    };
}

namespace winrt::BrightnessApp::factory_implementation
{
    struct OverlayWindow : OverlayWindowT<OverlayWindow, implementation::OverlayWindow>
    {
    };
}
