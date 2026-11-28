#pragma once

#include "OverlayWindow.g.h"
#include "OverlayWindow.xaml.g.h"
#include "MonitorManager.h"
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>

namespace winrt::ClickBrightness::implementation
{
    struct OverlayWindow : OverlayWindowT<OverlayWindow>
    {
        OverlayWindow();
        ~OverlayWindow();

        void TriggerBrightnessDelta(int delta, bool showOsd = true);
        void TriggerTrayWheel(int delta);
        void TriggerBrightnessSet(int target, bool showOsd = true);
        void TriggerBrightnessDeltaForTarget(const std::wstring& targetMonitorId, int delta, bool showOsd = true);
        void TriggerTrayWheelForTarget(const std::wstring& targetMonitorId, int delta);
        void TriggerBrightnessSetForTarget(const std::wstring& targetMonitorId, int target, bool showOsd = true);
        void ApplyPendingBrightness();
        void UpdateContent(int brightness);
        void UpdateMultiContent(const std::wstring& targetMonitorId, int brightness);
        void UpdateTheme();
        void ResetGraphics();
        void RefreshOnDisplayChange();
        void PositionWindow();
        void HideWindow();
        void DestroyWindow();

    private:
        enum class AnimState { None, Showing, Hiding };

        void SetupWindow();
        void SetupBackdrop();
        void DetachBackdrop();
        void StartShowAnimation();
        void StartHideAnimation();
        void StopAnimation();
        void UpdateAnimation();
        void FinishHide();
        static LRESULT CALLBACK SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

        HWND m_hwnd{ nullptr };
        Microsoft::UI::Windowing::AppWindow m_appWindow{ nullptr };

        Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController m_acrylicController{ nullptr };
        Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration m_backdropConfig{ nullptr };

        int m_displayedBrightness{ 45 };
        int m_pendingBrightness{ 45 };
        std::wstring m_pendingTargetMonitorId;
        bool m_hasPendingChange{ false };
        bool m_isVisible{ false };
        int m_osdWidthDip{ 194 };
        int m_osdHeightDip{ 47 };

        int m_lastTheme{ -1 };
        int m_lastTransparency{ -1 };
        COLORREF m_lastAccentCol{ 0 };
        int m_lastX{ -999999 };
        int m_lastY{ -999999 };
        int m_lastW{ 0 };
        int m_lastH{ 0 };
        ULONGLONG m_hideAfterTick{ 0 };

        AnimState m_animState{ AnimState::None };
        LARGE_INTEGER m_animStartQpc{};
        double m_animDurationMs{ 180.0 };
        int m_animStartY{ 0 };
        int m_animEndY{ 0 };
        int m_currentY{ 0 };
        int m_slideOffset{ 63 };
        winrt::event_token m_renderingToken{};
    };
}

namespace winrt::ClickBrightness::factory_implementation
{
    struct OverlayWindow : OverlayWindowT<OverlayWindow, implementation::OverlayWindow>
    {
    };
}
