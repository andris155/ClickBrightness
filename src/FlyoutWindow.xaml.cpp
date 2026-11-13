#include "pch.h"
#include "FlyoutWindow.xaml.h"
#if __has_include("FlyoutWindow.g.cpp")
#include "FlyoutWindow.g.cpp"
#endif

#include "MonitorManager.h"
#include "TrayIcon.h"
#include "OverlayWindow.h"
#include "Settings.h"
#include "LocalizationManager.h"
#include "DpiHelper.h"
#include "ThemeHelper.h"

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Input.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>

#define IDT_FLYOUT_DEBOUNCE 101

namespace winrt::BrightnessApp::implementation
{
    FlyoutWindow::FlyoutWindow()
    {
        InitializeComponent();
        bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
        RootGrid().RequestedTheme(isDark ? winrt::Microsoft::UI::Xaml::ElementTheme::Dark : winrt::Microsoft::UI::Xaml::ElementTheme::Light);
        SetupWindow();
        SetupBackdrop();
        InitializeButtons();

        // Mouse wheel brightness adjustment anywhere over the flyout
        auto onWheel = [this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
        {
            if (!g_Settings.flyoutScrollEnabled)
            {
                e.Handled(true);
                return;
            }
            auto ptrPt = e.GetCurrentPoint(RootGrid());
            int delta = ptrPt.Properties().MouseWheelDelta();
            if (delta != 0)
            {
                int step = (delta > 0) ? g_Settings.flyoutScrollStep : -g_Settings.flyoutScrollStep;
                OnScrollAdjust(m_brightness + step);
                e.Handled(true);
            }
        };
        BrightnessSlider().PointerWheelChanged(onWheel);
        RootGrid().PointerWheelChanged(onWheel);

        // Native WinUI 3 Slider ValueChanged
        BrightnessSlider().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
        {
            if (m_isUpdatingSlider) return;
            int val = static_cast<int>(std::round(e.NewValue()));
            val = (std::clamp)(val, 0, 100);
            if (val != m_brightness)
            {
                // Only commit hardware brightness immediately if not dragging (e.g. keyboard navigation, mouse wheel directly over slider)
                if (!m_isDraggingSlider)
                {
                    OnScrollAdjust(val);
                }
                else
                {
                    SetBrightnessVisual(val);
                }
            }
        });

        // Track drag state so hardware brightness only changes upon release
        BrightnessSlider().AddHandler(
            winrt::Microsoft::UI::Xaml::UIElement::PointerPressedEvent(),
            winrt::box_value(winrt::Microsoft::UI::Xaml::Input::PointerEventHandler([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
            {
                m_isDraggingSlider = true;
                if (m_hwnd)
                {
                    KillTimer(m_hwnd, IDT_FLYOUT_DEBOUNCE);
                }
                m_hasPendingBrightness = false;
            })),
            true
        );

        BrightnessSlider().AddHandler(
            winrt::Microsoft::UI::Xaml::UIElement::PointerReleasedEvent(),
            winrt::box_value(winrt::Microsoft::UI::Xaml::Input::PointerEventHandler([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
            {
                if (m_isDraggingSlider)
                {
                    m_isDraggingSlider = false;
                    CommitHardwareBrightness(m_brightness);
                }
            })),
            true
        );

        BrightnessSlider().AddHandler(
            winrt::Microsoft::UI::Xaml::UIElement::PointerCaptureLostEvent(),
            winrt::box_value(winrt::Microsoft::UI::Xaml::Input::PointerEventHandler([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
            {
                if (m_isDraggingSlider)
                {
                    m_isDraggingSlider = false;
                    CommitHardwareBrightness(m_brightness);
                }
            })),
            true
        );

        // Auto-hide when deactivating
        this->Activated([this](auto&&, winrt::Microsoft::UI::Xaml::WindowActivatedEventArgs const& args)
        {
            if (args.WindowActivationState() == winrt::Microsoft::UI::Xaml::WindowActivationState::Deactivated)
            {
                if (m_isVisible && GetTickCount64() - m_showTick >= 150)
                {
                    HideWindow();
                }
            }
        });

        m_brightness = g_Settings.cachedBrightness;
        m_isUpdatingSlider = true;
        BrightnessSlider().Value(m_brightness);
        m_isUpdatingSlider = false;
        TitleText().Text(L"Brightness " + std::to_wstring(m_brightness) + L"%");

        // Pre-warm layout and DirectX composition offscreen at startup
        // Pre-scaled to primary monitor DPI to eliminate first-click resize and re-layout
        if (m_hwnd)
        {
            POINT ptCursor = { 0, 0 };
            GetCursorPos(&ptCursor);
            HMONITOR hMon = MonitorFromPoint(ptCursor, MONITOR_DEFAULTTOPRIMARY);
            UINT dpi = DpiHelper::GetDpiForMonitor(hMon);
            if (dpi == 0) dpi = GetDpiForSystem();
            if (dpi == 0) dpi = 96;

            int width = DpiHelper::Scale(131, dpi);
            int height = DpiHelper::Scale(566, dpi);
            m_lastW = width;
            m_lastH = height;

            HWND hPrevForeground = GetForegroundWindow();
            SetWindowPos(m_hwnd, NULL, -32000, -32000, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            this->Activate();
            RootGrid().UpdateLayout();
            if (hPrevForeground && hPrevForeground != m_hwnd)
            {
                SetForegroundWindow(hPrevForeground);
            }
        }
    }

    FlyoutWindow::~FlyoutWindow()
    {
        DestroyWindow();
    }

    void FlyoutWindow::SetupWindow()
    {
        auto windowNative = this->try_as<::IWindowNative>();
        if (windowNative)
        {
            windowNative->get_WindowHandle(&m_hwnd);
        }

        if (m_hwnd)
        {
            auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(m_hwnd);
            m_appWindow = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);

            if (m_appWindow)
            {
                auto presenter = m_appWindow.Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>();
                if (presenter)
                {
                    presenter.SetBorderAndTitleBar(false, false);
                    presenter.IsAlwaysOnTop(true);
                    presenter.IsResizable(false);
                    presenter.IsMinimizable(false);
                    presenter.IsMaximizable(false);
                }
            }

            // Strip standard window frame styles
            LONG_PTR style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
            style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
            style |= WS_POPUP;
            SetWindowLongPtrW(m_hwnd, GWL_STYLE, style);

            LONG_PTR exStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
            exStyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME);
            exStyle |= WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
            SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exStyle);

            SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);

            COLORREF borderColor = DWMWA_COLOR_DEFAULT;
            DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));

            bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
            BOOL darkMode = isDark ? TRUE : FALSE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

            COLORREF captionColor = isDark ? RGB(32, 32, 32) : RGB(243, 243, 243);
            DwmSetWindowAttribute(m_hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

            DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
            DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

            int backdropType = g_Settings.transparencyEnabled ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));

            BOOL forceDisableTransitions = TRUE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &forceDisableTransitions, sizeof(forceDisableTransitions));

            SetWindowSubclass(m_hwnd, SubclassProc, 3, reinterpret_cast<DWORD_PTR>(this));
        }
    }

    void FlyoutWindow::SetupBackdrop()
    {
        if (!g_Settings.transparencyEnabled)
        {
            DetachBackdrop();
            UpdateTheme();
            return;
        }

        if (m_acrylicController)
        {
            return;
        }

        if (winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController::IsSupported())
        {
            m_acrylicController = winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController();
            if (!m_backdropConfig)
            {
                m_backdropConfig = winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration();
            }

            m_backdropConfig.IsInputActive(true);

            bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
            m_backdropConfig.Theme(isDark ?
                winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropTheme::Dark :
                winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropTheme::Light);

            auto target = this->try_as<winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>();
            if (target)
            {
                m_acrylicController.AddSystemBackdropTarget(target);
                m_acrylicController.SetSystemBackdropConfiguration(m_backdropConfig);
            }
        }

        UpdateTheme();
    }

    void FlyoutWindow::DetachBackdrop()
    {
        if (m_acrylicController)
        {
            try
            {
                m_acrylicController.RemoveAllSystemBackdropTargets();
                m_acrylicController.Close();
            }
            catch (...)
            {
            }
            m_acrylicController = nullptr;
        }
    }

    void FlyoutWindow::InitializeButtons()
    {
        ButtonStack().Children().Clear();
        for (int val = 100; val >= 0; val -= 5)
        {
            winrt::Microsoft::UI::Xaml::Controls::Button btn;
            btn.Content(winrt::box_value(std::to_wstring(val)));
            btn.Width(34);
            btn.Height(22);
            btn.Padding({ 0, 0, 0, 0 });
            btn.FontSize(13);
            btn.CornerRadius({ 3, 3, 3, 3 });
            btn.HorizontalContentAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
            btn.VerticalContentAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
            btn.Click([this, val](auto&&, auto&&)
            {
                OnButtonClicked(val);
            });
            ButtonStack().Children().Append(btn);
        }
    }

    void FlyoutWindow::UpdateTheme()
    {
        bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
        int curTrans = g_Settings.transparencyEnabled ? 1 : 0;
        COLORREF accCol = g_Settings.flyoutUseWindowsAccent
            ? ThemeHelper::GetAccentColors().GetForTheme(!isDark)
            : g_Settings.flyoutAccentColor;

        if (m_lastTheme == (isDark ? 1 : 0) && m_lastTransparency == curTrans && m_lastAccentCol == accCol)
        {
            return;
        }
        bool themeChanged = (m_lastTheme != (isDark ? 1 : 0));
        bool transChanged = (m_lastTransparency != curTrans);
        m_lastTheme = (isDark ? 1 : 0);
        m_lastTransparency = curTrans;
        m_lastAccentCol = accCol;

        if (themeChanged || transChanged)
        {
            RootGrid().RequestedTheme(isDark ? winrt::Microsoft::UI::Xaml::ElementTheme::Dark : winrt::Microsoft::UI::Xaml::ElementTheme::Light);

            if (g_Settings.transparencyEnabled)
            {
                if (!m_acrylicController)
                {
                    SetupBackdrop();
                }
                RootGrid().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Transparent()));
            }
            else
            {
                DetachBackdrop();
                winrt::Windows::UI::Color solidBg = isDark
                    ? winrt::Microsoft::UI::ColorHelper::FromArgb(255, 44, 44, 44)
                    : winrt::Microsoft::UI::ColorHelper::FromArgb(255, 243, 243, 243);
                RootGrid().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(solidBg));
            }

            if (m_backdropConfig)
            {
                m_backdropConfig.Theme(isDark ?
                    winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropTheme::Dark :
                    winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropTheme::Light);
            }

            if (m_hwnd)
            {
                BOOL darkMode = isDark ? TRUE : FALSE;
                DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

                int backdropType = g_Settings.transparencyEnabled ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE;
                DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
            }
        }

        auto res = BrightnessSlider().Resources();
        res.Clear();

        if (g_Settings.flyoutUseWindowsAccent)
        {
            BrightnessSlider().ClearValue(winrt::Microsoft::UI::Xaml::Controls::Control::ForegroundProperty());
        }
        else
        {
            winrt::Windows::UI::Color accColor = winrt::Microsoft::UI::ColorHelper::FromArgb(
                255, GetRValue(accCol), GetGValue(accCol), GetBValue(accCol)
            );

            auto makeVariation = [](COLORREF base, float factor) -> winrt::Windows::UI::Color {
                int r = (std::clamp)(static_cast<int>(GetRValue(base) * factor), 0, 255);
                int g = (std::clamp)(static_cast<int>(GetGValue(base) * factor), 0, 255);
                int b = (std::clamp)(static_cast<int>(GetBValue(base) * factor), 0, 255);
                return winrt::Microsoft::UI::ColorHelper::FromArgb(255, static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
            };

            winrt::Windows::UI::Color hoverColor = isDark ? makeVariation(accCol, 1.15f) : makeVariation(accCol, 0.9f);
            winrt::Windows::UI::Color pressedColor = isDark ? makeVariation(accCol, 0.85f) : makeVariation(accCol, 0.75f);

            auto defaultBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(accColor);
            auto hoverBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(hoverColor);
            auto pressedBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(pressedColor);

            res.Insert(winrt::box_value(L"SliderTrackValueFill"), defaultBrush);
            res.Insert(winrt::box_value(L"SliderTrackValueFillPointerOver"), hoverBrush);
            res.Insert(winrt::box_value(L"SliderTrackValueFillPressed"), pressedBrush);

            res.Insert(winrt::box_value(L"SliderThumbBackground"), defaultBrush);
            res.Insert(winrt::box_value(L"SliderThumbBackgroundPointerOver"), hoverBrush);
            res.Insert(winrt::box_value(L"SliderThumbBackgroundPressed"), pressedBrush);

            BrightnessSlider().Foreground(defaultBrush);
        }
    }

    void FlyoutWindow::ResetGraphics()
    {
        DetachBackdrop();
        SetupBackdrop();
        UpdateTheme();
        InitializeButtons();
        SetBrightnessVisual(m_brightness);
    }

    void FlyoutWindow::SetBrightnessVisual(int val)
    {
        m_brightness = (std::clamp)(val, 0, 100);
        std::wstring fmt = g_Loc.Get(L"Flyout_Brightness", L"Brightness %d%%");
        wchar_t bbuf[64];
        swprintf_s(bbuf, fmt.c_str(), m_brightness);
        TitleText().Text(bbuf);

        m_isUpdatingSlider = true;
        BrightnessSlider().Value(m_brightness);
        m_isUpdatingSlider = false;

        g_TrayIcon.UpdateBrightness(m_brightness);
        g_Settings.cachedBrightness = m_brightness;
    }

    void FlyoutWindow::CommitHardwareBrightness(int val)
    {
        if (m_hwnd)
        {
            KillTimer(m_hwnd, IDT_FLYOUT_DEBOUNCE);
        }
        m_hasPendingBrightness = false;
        g_MonitorManager.SetBrightness(val);
        g_Settings.cachedBrightness = val;
        g_Settings.SaveBrightnessOnly();
    }

    void FlyoutWindow::ScheduleDebounce(int val, int delayMs)
    {
        if (delayMs < 50) delayMs = 500;
        m_pendingBrightness = val;
        m_hasPendingBrightness = true;
        if (m_hwnd)
        {
            KillTimer(m_hwnd, IDT_FLYOUT_DEBOUNCE);
            SetTimer(m_hwnd, IDT_FLYOUT_DEBOUNCE, delayMs, NULL);
        }
    }

    void FlyoutWindow::OnScrollAdjust(int val)
    {
        SetBrightnessVisual(val);
        if (!g_Settings.flyoutScrollDelayEnabled)
        {
            CommitHardwareBrightness(m_brightness);
        }
        else
        {
            ScheduleDebounce(m_brightness, g_Settings.flyoutScrollDelayMs);
        }
    }

    void FlyoutWindow::OnButtonClicked(int val)
    {
        SetBrightnessVisual(val);
        if (!g_Settings.flyoutButtonDelayEnabled)
        {
            CommitHardwareBrightness(m_brightness);
        }
        else
        {
            ScheduleDebounce(m_brightness, g_Settings.flyoutButtonDelayMs);
        }
    }

    void FlyoutWindow::CommitBrightness(bool immediate)
    {
        g_TrayIcon.UpdateBrightness(m_brightness);
        g_Settings.cachedBrightness = m_brightness;

        if (immediate || !g_Settings.flyoutScrollDelayEnabled)
        {
            CommitHardwareBrightness(m_brightness);
        }
        else
        {
            ScheduleDebounce(m_brightness, g_Settings.flyoutScrollDelayMs);
        }
    }

    void FlyoutWindow::ApplyPendingBrightness()
    {
        if (m_hwnd)
        {
            KillTimer(m_hwnd, IDT_FLYOUT_DEBOUNCE);
        }
        if (m_hasPendingBrightness)
        {
            m_hasPendingBrightness = false;
            g_MonitorManager.SetBrightness(m_pendingBrightness);
            g_Settings.cachedBrightness = m_pendingBrightness;
            g_Settings.SaveBrightnessOnly();
        }
    }

    void FlyoutWindow::SetBrightness(int brightness, bool notifyHardware)
    {
        SetBrightnessVisual(brightness);

        if (notifyHardware)
        {
            CommitBrightness(false);
        }
        else
        {
            if (m_hwnd)
            {
                KillTimer(m_hwnd, IDT_FLYOUT_DEBOUNCE);
            }
            m_hasPendingBrightness = false;
        }
    }

    void FlyoutWindow::ShowWindow(const POINT* pTrayPos)
    {
        g_OverlayWindow.ApplyPendingBrightness();
        ApplyPendingBrightness();
        m_brightness = g_MonitorManager.GetCachedBrightness();
        m_pendingBrightness = m_brightness;
        m_hasPendingBrightness = false;
        UpdateTheme();
        m_isUpdatingSlider = true;
        BrightnessSlider().Value(m_brightness);
        m_isUpdatingSlider = false;
        std::wstring fmt = g_Loc.Get(L"Flyout_Brightness", L"Brightness %d%%");
        wchar_t bbuf[64];
        swprintf_s(bbuf, fmt.c_str(), m_brightness);
        TitleText().Text(bbuf);

        POINT pt;
        if (pTrayPos)
        {
            pt = *pTrayPos;
        }
        else
        {
            GetCursorPos(&pt);
        }

        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(hMon, &mi);

        UINT dpi = DpiHelper::GetDpiForMonitor(hMon);
        if (dpi == 0) dpi = GetDpiForWindow(m_hwnd);
        if (dpi == 0) dpi = 96;

        int width = DpiHelper::Scale(131, dpi);
        int height = DpiHelper::Scale(566, dpi);

        // Check if opened from Windows hidden icons overflow window
        HWND hOverflow = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", NULL);
        if (!hOverflow)
        {
            hOverflow = FindWindowW(L"NotifyIconOverflowWindow", NULL);
        }
        RECT rcOverflow = { 0 };
        bool isFromOverflow = false;
        if (hOverflow && ::IsWindowVisible(hOverflow) && GetWindowRect(hOverflow, &rcOverflow))
        {
            if (pt.x >= rcOverflow.left && pt.x <= rcOverflow.right &&
                pt.y >= rcOverflow.top && pt.y <= rcOverflow.bottom)
            {
                isFromOverflow = true;
            }
        }

        // Also query tray icon rectangle
        RECT rcIcon = { 0 };
        NOTIFYICONIDENTIFIER nii = { sizeof(NOTIFYICONIDENTIFIER) };
        nii.hWnd = g_TrayIcon.GetOwnerWnd();
        nii.uID = g_TrayIcon.GetId();
        if (SUCCEEDED(Shell_NotifyIconGetRect(&nii, &rcIcon)))
        {
            if (rcIcon.right > rcIcon.left && rcIcon.bottom > rcIcon.top)
            {
                int iconCenterX = rcIcon.left + (rcIcon.right - rcIcon.left) / 2;
                pt.x = iconCenterX;

                if (!isFromOverflow && hOverflow && ::IsWindowVisible(hOverflow))
                {
                    if (rcIcon.left >= rcOverflow.left && rcIcon.right <= rcOverflow.right &&
                        rcIcon.top >= rcOverflow.top && rcIcon.bottom <= rcOverflow.bottom)
                    {
                        isFromOverflow = true;
                    }
                }
            }
        }

        int x = pt.x - width / 2;
        int y;
        if (isFromOverflow && (rcOverflow.bottom > rcOverflow.top))
        {
            // Position above the hidden icons section like the right-click menu
            if (pt.y < (mi.rcWork.top + mi.rcWork.bottom) / 2)
            {
                // Top taskbar: position below the overflow window
                y = rcOverflow.bottom + DpiHelper::Scale(8, dpi);
            }
            else
            {
                // Bottom taskbar: position directly above the top of the overflow window
                y = rcOverflow.top - height - DpiHelper::Scale(8, dpi);
                if (y < mi.rcWork.top)
                {
                    // If it does not fit vertically above the overflow window, position to its left
                    if (rcOverflow.left - width - DpiHelper::Scale(8, dpi) >= mi.rcWork.left)
                    {
                        x = rcOverflow.left - width - DpiHelper::Scale(8, dpi);
                        y = mi.rcWork.bottom - height - DpiHelper::Scale(14, dpi);
                    }
                    else
                    {
                        y = mi.rcWork.top + DpiHelper::Scale(8, dpi);
                    }
                }
            }
        }
        else
        {
            if (pt.y < (mi.rcWork.top + mi.rcWork.bottom) / 2)
            {
                y = mi.rcWork.top + DpiHelper::Scale(8, dpi);
            }
            else
            {
                y = mi.rcWork.bottom - height - DpiHelper::Scale(14, dpi);
            }
        }

        if (x + width > mi.rcWork.right - DpiHelper::Scale(8, dpi))
        {
            x = mi.rcWork.right - width - DpiHelper::Scale(8, dpi);
        }
        if (x < mi.rcWork.left + DpiHelper::Scale(8, dpi))
        {
            x = mi.rcWork.left + DpiHelper::Scale(8, dpi);
        }
        if (y + height > mi.rcWork.bottom - DpiHelper::Scale(8, dpi))
        {
            y = mi.rcWork.bottom - height - DpiHelper::Scale(8, dpi);
        }
        if (y < mi.rcWork.top + DpiHelper::Scale(8, dpi))
        {
            y = mi.rcWork.top + DpiHelper::Scale(8, dpi);
        }

        m_showTick = GetTickCount64();
        m_isVisible = true;

        bool sizeChanged = (m_lastW != width || m_lastH != height);
        m_lastW = width;
        m_lastH = height;

        UINT swpFlags = SWP_SHOWWINDOW;
        if (!sizeChanged)
        {
            swpFlags |= SWP_NOSIZE;
        }

        if (m_hwnd)
        {
            SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, width, height, swpFlags);
            SetForegroundWindow(m_hwnd);
            SetFocus(m_hwnd);
        }
    }

    void FlyoutWindow::HideWindow()
    {
        if (m_isVisible)
        {
            m_isVisible = false;
            m_hideTick = GetTickCount64();
            if (m_isDraggingSlider)
            {
                m_isDraggingSlider = false;
                CommitHardwareBrightness(m_brightness);
            }
            ApplyPendingBrightness();
            if (m_hwnd)
            {
                SetWindowPos(m_hwnd, NULL, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }

    void FlyoutWindow::DestroyWindow()
    {
        ApplyPendingBrightness();
        m_isVisible = false;
        HWND h = m_hwnd;
        m_hwnd = NULL;
        if (h)
        {
            KillTimer(h, IDT_FLYOUT_DEBOUNCE);
            RemoveWindowSubclass(h, SubclassProc, 3);
            ::ShowWindow(h, SW_HIDE);
        }
        DetachBackdrop();
        m_backdropConfig = nullptr;
        m_appWindow = nullptr;
        try
        {
            this->Close();
        }
        catch (...) {}
    }

    bool FlyoutWindow::IsWindowVisible() const
    {
        return m_isVisible;
    }

    LRESULT CALLBACK FlyoutWindow::SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        auto pThis = reinterpret_cast<FlyoutWindow*>(dwRefData);
        if (!pThis) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        if (uMsg == WM_TIMER)
        {
            if (wParam == IDT_FLYOUT_DEBOUNCE)
            {
                pThis->ApplyPendingBrightness();
                return 0;
            }
        }

        if (uMsg == WM_ACTIVATE)
        {
            if (LOWORD(wParam) == WA_INACTIVE)
            {
                if (pThis->m_isVisible && GetTickCount64() - pThis->m_showTick >= 150)
                {
                    pThis->HideWindow();
                }
            }
        }

        if (uMsg == WM_NCDESTROY)
        {
            RemoveWindowSubclass(hWnd, SubclassProc, uIdSubclass);
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}
