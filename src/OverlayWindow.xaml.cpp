#include "pch.h"
#include "OverlayWindow.xaml.h"
#if __has_include("OverlayWindow.g.cpp")
#include "OverlayWindow.g.cpp"
#endif

#include "MonitorManager.h"
#include "TrayIcon.h"
#include "FlyoutWindow.h"
#include "Settings.h"
#include "LocalizationManager.h"
#include "DpiHelper.h"
#include "ThemeHelper.h"
#include "resource.h"

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>

namespace winrt::ClickBrightness::implementation
{
    OverlayWindow::OverlayWindow()
    {
        InitializeComponent();
        bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
        RootGrid().RequestedTheme(isDark ? winrt::Microsoft::UI::Xaml::ElementTheme::Dark : winrt::Microsoft::UI::Xaml::ElementTheme::Light);
        SetupWindow();
        SetupBackdrop();

        // Pre-warm layout and DirectX composition offscreen at startup
        // Pre-scaled to primary monitor DPI to prevent first-hit WM_DPICHANGED / swapchain resize
        if (m_hwnd)
        {
            POINT ptCursor = { 0, 0 };
            GetCursorPos(&ptCursor);
            HMONITOR hMon = MonitorFromPoint(ptCursor, MONITOR_DEFAULTTOPRIMARY);
            UINT dpi = DpiHelper::GetDpiForMonitor(hMon);
            if (dpi == 0) dpi = GetDpiForSystem();
            if (dpi == 0) dpi = 96;

            int width = DpiHelper::Scale(194, dpi);
            int height = DpiHelper::Scale(47, dpi);
            m_lastW = width;
            m_lastH = height;

            BOOL cloak = TRUE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
            HWND hPrevForeground = GetForegroundWindow();
            SetWindowPos(m_hwnd, NULL, -32000, -32000, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            this->Activate();
            RootGrid().UpdateLayout();
            ShowWindow(m_hwnd, SW_HIDE);
            if (hPrevForeground && hPrevForeground != m_hwnd)
            {
                SetForegroundWindow(hPrevForeground);
            }
        }
    }

    OverlayWindow::~OverlayWindow()
    {
        DestroyWindow();
    }

    void OverlayWindow::SetupWindow()
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

            // 1. Strip standard window frame styles (caption, thickframe, borders) to remove white border lines
            LONG_PTR style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
            style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
            style |= WS_POPUP;
            SetWindowLongPtrW(m_hwnd, GWL_STYLE, style);

            // 2. Window styles: toolwindow, topmost, noactivate, no edges
            LONG_PTR exStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
            exStyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME | WS_EX_LAYERED);
            exStyle |= WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
            SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exStyle);

            // 3. Window corners: round window frame
            DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
            DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

            // 4. Immersive dark mode attribute
            bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
            BOOL darkMode = isDark ? TRUE : FALSE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

            COLORREF captionColor = isDark ? RGB(32, 32, 32) : RGB(243, 243, 243);
            DwmSetWindowAttribute(m_hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

            // 5. Border: native FlyoutWindow DWM border (DWMWA_COLOR_DEFAULT)
            COLORREF borderColor = DWMWA_COLOR_DEFAULT;
            DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));

            // 6. Native DWM Transient Acrylic (Windows 11 Volume / Shell OSD Backdrop)
            int backdropType = g_Settings.transparencyEnabled ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));

            // 7. Instant snappy display (no DWM open/close animation lag)
            BOOL forceDisableTransitions = TRUE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &forceDisableTransitions, sizeof(forceDisableTransitions));

            // 8. Keep shadow effect under overlay
            int ncPolicy = 0; // DWMNCRP_USEWINDOWSTYLE
            DwmSetWindowAttribute(m_hwnd, DWMWA_NCRENDERING_POLICY, &ncPolicy, sizeof(ncPolicy));

            SetWindowPos(m_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);

            // Subclass window for click-through and timers
            SetWindowSubclass(m_hwnd, SubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
        }

        // Keep indicator size in sync with track size
        TrackGrid().SizeChanged([this](auto&&, auto&&)
        {
            UpdateContent(m_displayedBrightness);
        });

        m_displayedBrightness = g_Settings.cachedBrightness;
        m_pendingBrightness = m_displayedBrightness;

        UpdateContent(m_displayedBrightness);
    }

    void OverlayWindow::SetupBackdrop()
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

            // CRITICAL: Force IsInputActive(true) permanently so the OS compositor NEVER reverts to opaque black!
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

    void OverlayWindow::DetachBackdrop()
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

    void OverlayWindow::UpdateTheme()
    {
        bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
        int curTrans = g_Settings.transparencyEnabled ? 1 : 0;
        COLORREF accCol = g_Settings.overlayUseWindowsAccent
            ? ThemeHelper::GetAccentColors().GetForTheme(!isDark)
            : ((g_Settings.overlayUseDualColors && isDark) ? g_Settings.overlayAccentColorDark : g_Settings.overlayAccentColor);

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

            // Tell DWM whether to render dark or light transient acrylic (matches Windows volume flyout)
            if (m_hwnd)
            {
                BOOL darkMode = isDark ? TRUE : FALSE;
                DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

                int backdropType = g_Settings.transparencyEnabled ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE;
                DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));

                // Native FlyoutWindow DWM border
                COLORREF borderColor = DWMWA_COLOR_DEFAULT;
                DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));

                // Keep shadow effect under overlay
                int ncPolicy = 0; // DWMNCRP_USEWINDOWSTYLE
                DwmSetWindowAttribute(m_hwnd, DWMWA_NCRENDERING_POLICY, &ncPolicy, sizeof(ncPolicy));

                SetWindowPos(m_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
            }

            // Layout matching FlyoutWindow (no inner XAML border, native DWM border handles frame)
            PillBorder().Margin(winrt::Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 0 });
            PillBorder().Padding(winrt::Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 0 });
            PillBorder().BorderThickness(winrt::Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 0 });
            PillBorder().BorderBrush(nullptr);
            PillBorder().CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{ 8, 8, 8, 8 });

            if (m_backdropConfig)
            {
                m_backdropConfig.IsInputActive(true);
                m_backdropConfig.Theme(isDark ?
                    winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropTheme::Dark :
                    winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropTheme::Light);
            }

            winrt::Windows::UI::Color fgColor = isDark
                ? winrt::Microsoft::UI::Colors::White()
                : winrt::Microsoft::UI::ColorHelper::FromArgb(255, 28, 28, 28);

            SunIcon().Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(fgColor));
            PercentText().Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(fgColor));

            winrt::Windows::UI::Color trackBg = isDark
                ? winrt::Microsoft::UI::ColorHelper::FromArgb(135, 255, 255, 255)
                : winrt::Microsoft::UI::ColorHelper::FromArgb(120, 0, 0, 0);

            TrackGrid().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(trackBg));
        }

        winrt::Windows::UI::Color accColor = winrt::Microsoft::UI::ColorHelper::FromArgb(
            255, GetRValue(accCol), GetGValue(accCol), GetBValue(accCol)
        );
        IndicatorBorder().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(accColor));
    }

    void OverlayWindow::ResetGraphics()
    {
        DetachBackdrop();
        SetupBackdrop();
        UpdateTheme();
    }

    void OverlayWindow::UpdateContent(int brightness)
    {
        PercentText().Text(std::to_wstring(brightness));

        double trackW = TrackGrid().ActualWidth();
        if (trackW <= 0) trackW = 110.0; // standard width in DIPs matching Windows 11 volume changer

        if (brightness <= 0)
        {
            IndicatorBorder().Width(0.0);
        }
        else
        {
            double fillW = (brightness / 100.0) * trackW;
            if (fillW < 4.0) fillW = 4.0; // min width matching 2px corner radius
            if (fillW > trackW) fillW = trackW;
            IndicatorBorder().Width(fillW);
        }
    }

    void OverlayWindow::UpdateMultiContent(const std::wstring& targetMonitorId, int brightness)
    {
        auto entries = g_MonitorManager.GetChangedMonitorsForTarget(targetMonitorId, brightness);
        if (entries.empty())
        {
            SingleRowGrid().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            MultiRowsPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            m_osdWidthDip = 194;
            m_osdHeightDip = 47;
            UpdateContent(brightness);
            return;
        }

        // Count enabled monitors total
        int enabledTotal = 0;
        for (const auto& mon : g_MonitorManager.GetMonitors())
        {
            if (g_Settings.IsMonitorSelected(mon.id)) enabledTotal++;
        }

        bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
        if (entries.size() == 1 && enabledTotal <= 1)
        {
            SingleRowGrid().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            MultiRowsPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            m_osdWidthDip = 194;
            m_osdHeightDip = 47;

            COLORREF accCol = entries[0].useWindowsAccent
                ? ThemeHelper::GetAccentColors().GetForTheme(!isDark)
                : entries[0].accentColor;
            IndicatorBorder().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
                winrt::Microsoft::UI::ColorHelper::FromArgb(255, GetRValue(accCol), GetGValue(accCol), GetBValue(accCol))));
            PercentText().Opacity(entries[0].isPoweredOff ? 0.55 : 1.0);
            UpdateContent(entries[0].brightness);
            return;
        }

        SingleRowGrid().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        MultiRowsPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        MultiRowsPanel().Children().Clear();

        winrt::Windows::UI::Color fgColor = isDark
            ? winrt::Microsoft::UI::Colors::White()
            : winrt::Microsoft::UI::ColorHelper::FromArgb(255, 28, 28, 28);
        winrt::Windows::UI::Color secColor = isDark
            ? winrt::Microsoft::UI::ColorHelper::FromArgb(200, 220, 220, 220)
            : winrt::Microsoft::UI::ColorHelper::FromArgb(200, 60, 60, 60);
        winrt::Windows::UI::Color trackBg = isDark
            ? winrt::Microsoft::UI::ColorHelper::FromArgb(135, 255, 255, 255)
            : winrt::Microsoft::UI::ColorHelper::FromArgb(120, 0, 0, 0);

        auto fgBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(fgColor);
        auto secBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(secColor);
        auto trackBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(trackBg);

        for (const auto& entry : entries)
        {
            winrt::Microsoft::UI::Xaml::Controls::StackPanel rowPanel;
            rowPanel.Spacing(4);

            // Top row: Monitor Name + Brightness %
            winrt::Microsoft::UI::Xaml::Controls::Grid topGrid;
            winrt::Microsoft::UI::Xaml::Controls::TextBlock nameText;
            nameText.Text(entry.displayName);
            nameText.FontSize(11.5);
            nameText.Foreground(secBrush);
            nameText.TextTrimming(winrt::Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
            nameText.MaxWidth(entry.isPoweredOff ? 125 : 155);
            nameText.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Left);
            nameText.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);

            winrt::Microsoft::UI::Xaml::Controls::TextBlock pctText;
            if (entry.isPoweredOff)
            {
                pctText.Text(std::to_wstring(entry.brightness) + L"% (" + g_Loc.Get(L"Common_Off", L"Off") + L")");
                pctText.Opacity(0.55);
            }
            else
            {
                pctText.Text(std::to_wstring(entry.brightness) + L"%");
                pctText.Opacity(1.0);
            }
            pctText.FontSize(12);
            pctText.Foreground(fgBrush);
            pctText.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right);
            pctText.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);

            topGrid.Children().Append(nameText);
            topGrid.Children().Append(pctText);
            rowPanel.Children().Append(topGrid);

            // Bottom row: SunIcon + Bar
            winrt::Microsoft::UI::Xaml::Controls::Grid barGrid;
            winrt::Microsoft::UI::Xaml::Controls::FontIcon icon;
            icon.Glyph(L"\uE706");
            icon.FontSize(13);
            icon.Width(14);
            icon.Height(14);
            icon.Foreground(fgBrush);
            icon.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Left);
            icon.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);

            constexpr double trackW = 172.0;
            winrt::Microsoft::UI::Xaml::Controls::Grid track;
            track.Width(trackW);
            track.Height(4);
            track.CornerRadius({ 2, 2, 2, 2 });
            track.Background(trackBrush);
            track.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right);
            track.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);

            winrt::Microsoft::UI::Xaml::Controls::Border indicator;
            indicator.Height(4);
            indicator.CornerRadius({ 2, 2, 2, 2 });
            indicator.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Left);
            COLORREF accCol = entry.useWindowsAccent
                ? ThemeHelper::GetAccentColors().GetForTheme(!isDark)
                : entry.accentColor;
            indicator.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
                winrt::Microsoft::UI::ColorHelper::FromArgb(255, GetRValue(accCol), GetGValue(accCol), GetBValue(accCol))));
            if (entry.brightness <= 0)
            {
                indicator.Width(0.0);
            }
            else
            {
                double fillW = (entry.brightness / 100.0) * trackW;
                if (fillW < 4.0) fillW = 4.0;
                if (fillW > trackW) fillW = trackW;
                indicator.Width(fillW);
            }
            track.Children().Append(indicator);

            barGrid.Children().Append(icon);
            barGrid.Children().Append(track);
            rowPanel.Children().Append(barGrid);

            MultiRowsPanel().Children().Append(rowPanel);
        }

        int n = static_cast<int>(entries.size());
        m_osdWidthDip = 220;
        m_osdHeightDip = n * 35 + (n - 1) * 6 + 16;
    }

    void OverlayWindow::PositionWindow()
    {
        if (!m_hwnd || !m_appWindow) return;

        POINT ptCursor;
        GetCursorPos(&ptCursor);
        HMONITOR hMon = MonitorFromPoint(ptCursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(hMon, &mi);

        UINT dpi = DpiHelper::GetDpiForMonitor(hMon);
        if (dpi == 0) dpi = GetDpiForWindow(m_hwnd);
        if (dpi == 0) dpi = 96;
        int width = DpiHelper::Scale(m_osdWidthDip, dpi);
        int height = DpiHelper::Scale(m_osdHeightDip, dpi);
        int marginBottom = DpiHelper::Scale(16, dpi);
        m_slideOffset = height + marginBottom;

        int workW = mi.rcWork.right - mi.rcWork.left;
        int targetX = mi.rcWork.left + (workW - width) / 2;
        int targetY = mi.rcWork.bottom - height - marginBottom;

        // Place right behind the monitor's taskbar in topmost Z-order so it emerges from under it
        HWND hInsertAfter = HWND_TOPMOST;
        HWND hTray = FindWindowW(L"Shell_TrayWnd", NULL);
        if (hTray && MonitorFromWindow(hTray, MONITOR_DEFAULTTONEAREST) == hMon &&
            (GetWindowLongPtrW(hTray, GWL_EXSTYLE) & WS_EX_TOPMOST))
        {
            hInsertAfter = hTray;
        }
        else
        {
            HWND hSecTray = NULL;
            while ((hSecTray = FindWindowExW(NULL, hSecTray, L"Shell_SecondaryTrayWnd", NULL)) != NULL)
            {
                if (MonitorFromWindow(hSecTray, MONITOR_DEFAULTTONEAREST) == hMon &&
                    (GetWindowLongPtrW(hSecTray, GWL_EXSTYLE) & WS_EX_TOPMOST))
                {
                    hInsertAfter = hSecTray;
                    break;
                }
            }
        }

        bool sizeChanged = (m_lastW != width || m_lastH != height);
        bool posChanged = (m_lastX != targetX || m_lastY != targetY || sizeChanged);
        m_lastX = targetX;
        m_lastY = targetY;
        m_lastW = width;
        m_lastH = height;

        UINT swpFlags = SWP_NOACTIVATE;
        if (!sizeChanged)
        {
            swpFlags |= SWP_NOSIZE;
        }

        if (!m_isVisible)
        {
            SetupBackdrop();
            BOOL cloak = TRUE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
            m_currentY = targetY + m_slideOffset;
            SetWindowPos(m_hwnd, hInsertAfter, targetX, m_currentY, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            RootGrid().UpdateLayout();
            SetTimer(m_hwnd, IDT_OVERLAY_UNCLOAK, 16, NULL);
            m_isVisible = true;
            m_animState = AnimState::Showing;
        }
        else if (m_animState == AnimState::Hiding)
        {
            if (posChanged)
            {
                SetWindowPos(m_hwnd, hInsertAfter, targetX, m_currentY, width, height, swpFlags);
            }
            StartShowAnimation();
        }
        else if (m_animState == AnimState::Showing)
        {
            if (posChanged)
            {
                m_animEndY = targetY;
                SetWindowPos(m_hwnd, hInsertAfter, targetX, m_currentY, width, height, swpFlags);
            }
        }
        else if (posChanged)
        {
            m_currentY = targetY;
            SetWindowPos(m_hwnd, hInsertAfter, targetX, targetY, width, height, swpFlags);
        }
    }

    void OverlayWindow::TriggerBrightnessDelta(int delta, bool showOsd)
    {
        TriggerBrightnessDeltaForTarget(L"", delta, showOsd);
    }

    void OverlayWindow::TriggerBrightnessDeltaForTarget(const std::wstring& targetMonitorId, int delta, bool showOsd)
    {
        g_FlyoutWindow.ApplyPendingBrightness();
        if (m_hasPendingChange && _wcsicmp(m_pendingTargetMonitorId.c_str(), targetMonitorId.c_str()) != 0)
        {
            ApplyPendingBrightness();
        }
        m_pendingTargetMonitorId = targetMonitorId;
        int minB = 0, maxB = 100;
        g_MonitorManager.GetTargetBounds(targetMonitorId, minB, maxB);
        int cur = m_hasPendingChange ? m_pendingBrightness : g_MonitorManager.GetTargetControlBrightness(targetMonitorId);
        int target = (std::clamp)(cur + delta, minB, maxB);
        m_displayedBrightness = target;
        m_pendingBrightness = target;
        m_hasPendingChange = true;

        bool dndSuppressed = (g_Settings.osdSuppressOnDnd && ThemeHelper::IsDoNotDisturbActive());
        if (showOsd && !dndSuppressed)
        {
            UpdateMultiContent(targetMonitorId, target);
            PositionWindow();

            if (m_backdropConfig)
            {
                m_backdropConfig.IsInputActive(true);
            }

            // Reset hide timer with deadline timestamp tracking to prevent timer race conditions
            int timeout = g_Settings.overlayTimeoutMs;
            if (timeout < 200) timeout = 500;

            ULONGLONG now = GetTickCount64();
            m_hideAfterTick = now + timeout;

            if (m_hwnd)
            {
                KillTimer(m_hwnd, IDT_OVERLAY_HIDE);
                SetTimer(m_hwnd, IDT_OVERLAY_HIDE, timeout, NULL);
            }
        }

        // Hardware delay logic
        if (!g_Settings.hotkeyDelayEnabled)
        {
            ApplyPendingBrightness();
        }
        else
        {
            int delay = g_Settings.hotkeyDelayMs;
            if (delay < 50) delay = 1000;
            if (m_hwnd)
            {
                KillTimer(m_hwnd, IDT_HOTKEY_DEBOUNCE);
                SetTimer(m_hwnd, IDT_HOTKEY_DEBOUNCE, delay, NULL);
            }
        }

        g_TrayIcon.UpdateBrightnessForTarget(targetMonitorId, target);
        g_FlyoutWindow.SetBrightnessForTarget(targetMonitorId, target, false);
    }

    void OverlayWindow::TriggerTrayWheel(int delta)
    {
        TriggerTrayWheelForTarget(L"", delta);
    }

    void OverlayWindow::TriggerTrayWheelForTarget(const std::wstring& targetMonitorId, int delta)
    {
        g_FlyoutWindow.ApplyPendingBrightness();
        if (m_hasPendingChange && _wcsicmp(m_pendingTargetMonitorId.c_str(), targetMonitorId.c_str()) != 0)
        {
            ApplyPendingBrightness();
        }
        m_pendingTargetMonitorId = targetMonitorId;
        int minB = 0, maxB = 100;
        g_MonitorManager.GetTargetBounds(targetMonitorId, minB, maxB);
        int cur = m_hasPendingChange ? m_pendingBrightness : g_MonitorManager.GetTargetControlBrightness(targetMonitorId);
        int target = (std::clamp)(cur + delta, minB, maxB);
        m_displayedBrightness = target;
        m_pendingBrightness = target;
        m_hasPendingChange = true;

        const auto& effCfg = targetMonitorId.empty()
            ? g_Settings.GetEffectiveMonitorConfig(g_Settings.mainMonitorId)
            : g_Settings.GetEffectiveMonitorConfig(targetMonitorId);

        bool dndSuppressed = (g_Settings.osdSuppressOnDnd && ThemeHelper::IsDoNotDisturbActive());
        if (effCfg.trayWheelShowOsd && !dndSuppressed)
        {
            UpdateMultiContent(targetMonitorId, target);
            PositionWindow();

            if (m_backdropConfig)
            {
                m_backdropConfig.IsInputActive(true);
            }

            int timeout = g_Settings.overlayTimeoutMs;
            if (timeout < 200) timeout = 500;

            ULONGLONG now = GetTickCount64();
            m_hideAfterTick = now + timeout;

            if (m_hwnd)
            {
                KillTimer(m_hwnd, IDT_OVERLAY_HIDE);
                SetTimer(m_hwnd, IDT_OVERLAY_HIDE, timeout, NULL);
            }
        }

        // Hardware delay logic using Tray Wheel settings
        if (!g_Settings.trayWheelDelayEnabled)
        {
            ApplyPendingBrightness();
        }
        else
        {
            int delay = g_Settings.trayWheelDelayMs;
            if (delay < 50) delay = 500;
            if (m_hwnd)
            {
                KillTimer(m_hwnd, IDT_HOTKEY_DEBOUNCE);
                SetTimer(m_hwnd, IDT_HOTKEY_DEBOUNCE, delay, NULL);
            }
        }

        g_TrayIcon.UpdateBrightnessForTarget(targetMonitorId, target);
        g_FlyoutWindow.SetBrightnessForTarget(targetMonitorId, target, false);
    }

    void OverlayWindow::TriggerBrightnessSet(int target, bool showOsd)
    {
        TriggerBrightnessSetForTarget(L"", target, showOsd);
    }

    void OverlayWindow::TriggerBrightnessSetForTarget(const std::wstring& targetMonitorId, int target, bool showOsd)
    {
        g_FlyoutWindow.ApplyPendingBrightness();
        if (m_hasPendingChange && _wcsicmp(m_pendingTargetMonitorId.c_str(), targetMonitorId.c_str()) != 0)
        {
            ApplyPendingBrightness();
        }
        m_pendingTargetMonitorId = targetMonitorId;
        int minB = 0, maxB = 100;
        g_MonitorManager.GetTargetBounds(targetMonitorId, minB, maxB);
        target = (std::clamp)(target, minB, maxB);
        int cur = g_MonitorManager.GetTargetControlBrightness(targetMonitorId);
        bool changed = (cur != target);

        m_displayedBrightness = target;
        m_pendingBrightness = target;
        m_hasPendingChange = changed;

        bool dndSuppressed = (g_Settings.osdSuppressOnDnd && ThemeHelper::IsDoNotDisturbActive());
        if (showOsd && !dndSuppressed)
        {
            UpdateMultiContent(targetMonitorId, target);
            PositionWindow();

            if (m_backdropConfig)
            {
                m_backdropConfig.IsInputActive(true);
            }

            int timeout = g_Settings.overlayTimeoutMs;
            if (timeout < 200) timeout = 500;

            ULONGLONG now = GetTickCount64();
            m_hideAfterTick = now + timeout;

            if (m_hwnd)
            {
                KillTimer(m_hwnd, IDT_OVERLAY_HIDE);
                SetTimer(m_hwnd, IDT_OVERLAY_HIDE, timeout, NULL);
            }
        }

        // Only send DDC/CI hardware commands if brightness actually changes
        if (changed)
        {
            ApplyPendingBrightness();
        }

        g_TrayIcon.UpdateBrightnessForTarget(targetMonitorId, target);
        g_FlyoutWindow.SetBrightnessForTarget(targetMonitorId, target, false);
    }

    void OverlayWindow::ApplyPendingBrightness()
    {
        if (m_hasPendingChange)
        {
            m_hasPendingChange = false;
            if (m_hwnd)
            {
                KillTimer(m_hwnd, IDT_HOTKEY_DEBOUNCE);
            }
            g_MonitorManager.SetMonitorBrightnessTarget(m_pendingTargetMonitorId, m_pendingBrightness);
            g_FlyoutWindow.SetBrightnessForTarget(m_pendingTargetMonitorId, m_pendingBrightness, false);
            if (m_pendingTargetMonitorId.empty() ||
                _wcsicmp(m_pendingTargetMonitorId.c_str(), L"__SYNC__") == 0 ||
                _wcsicmp(m_pendingTargetMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0)
            {
                g_Settings.cachedBrightness = m_pendingBrightness;
            }
            g_Settings.Save();
        }
    }

    void OverlayWindow::StopAnimation()
    {
        if (m_renderingToken.value != 0)
        {
            try
            {
                winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(m_renderingToken);
            }
            catch (...) {}
            m_renderingToken = {};
        }
    }

    void OverlayWindow::StartShowAnimation()
    {
        if (!m_hwnd) return;
        m_animState = AnimState::Showing;
        m_animStartY = m_currentY;
        m_animEndY = m_lastY;
        m_animDurationMs = 180.0;
        QueryPerformanceCounter(&m_animStartQpc);
        if (m_renderingToken.value == 0)
        {
            m_renderingToken = winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(
                [this](auto&&, auto&&) { UpdateAnimation(); });
        }
    }

    void OverlayWindow::StartHideAnimation()
    {
        if (!m_hwnd || !m_isVisible) return;
        if (m_animState == AnimState::Hiding) return;
        m_animState = AnimState::Hiding;
        m_animStartY = m_currentY;
        m_animEndY = m_lastY + m_slideOffset;
        m_animDurationMs = 150.0;
        QueryPerformanceCounter(&m_animStartQpc);
        if (m_renderingToken.value == 0)
        {
            m_renderingToken = winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(
                [this](auto&&, auto&&) { UpdateAnimation(); });
        }
    }

    void OverlayWindow::UpdateAnimation()
    {
        if (!m_hwnd || m_animState == AnimState::None)
        {
            StopAnimation();
            return;
        }

        static LARGE_INTEGER s_freq = []() {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return f;
        }();

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsedMs = (double)(now.QuadPart - m_animStartQpc.QuadPart) * 1000.0 / (double)s_freq.QuadPart;
        double t = (m_animDurationMs > 0.0) ? (elapsedMs / m_animDurationMs) : 1.0;
        if (t < 0.0) t = 0.0;
        if (t >= 1.0) t = 1.0;

        double eased = (m_animState == AnimState::Showing)
            ? (1.0 - std::pow(1.0 - t, 3.0))
            : (t * t);

        int newY = m_animStartY + static_cast<int>(std::round((m_animEndY - m_animStartY) * eased));
        if (newY != m_currentY)
        {
            m_currentY = newY;
            SetWindowPos(m_hwnd, NULL, m_lastX, m_currentY, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_NOCOPYBITS | SWP_DEFERERASE);
        }

        if (t >= 1.0)
        {
            StopAnimation();
            if (m_animState == AnimState::Hiding)
            {
                FinishHide();
            }
            else
            {
                m_animState = AnimState::None;
            }
        }
    }

    void OverlayWindow::FinishHide()
    {
        StopAnimation();
        if (m_hwnd)
        {
            KillTimer(m_hwnd, IDT_OVERLAY_UNCLOAK);
        }
        m_animState = AnimState::None;
        m_isVisible = false;
        m_hideAfterTick = 0;
        m_lastX = -999999;
        m_lastY = -999999;
        if (m_hwnd)
        {
            BOOL cloak = TRUE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
            ::ShowWindow(m_hwnd, SW_HIDE);
        }
    }

    void OverlayWindow::HideWindow()
    {
        if (m_hwnd)
        {
            KillTimer(m_hwnd, IDT_OVERLAY_HIDE);
            KillTimer(m_hwnd, IDT_OVERLAY_UNCLOAK);
        }
        ApplyPendingBrightness();

        if (m_isVisible)
        {
            StartHideAnimation();
        }
    }

    void OverlayWindow::DestroyWindow()
    {
        StopAnimation();
        ApplyPendingBrightness();
        HWND h = m_hwnd;
        m_hwnd = NULL;
        if (h)
        {
            KillTimer(h, IDT_OVERLAY_HIDE);
            KillTimer(h, IDT_OVERLAY_UNCLOAK);
            KillTimer(h, IDT_HOTKEY_DEBOUNCE);
            RemoveWindowSubclass(h, SubclassProc, 2);
            ::ShowWindow(h, SW_HIDE);
        }
        DetachBackdrop();
        m_backdropConfig = nullptr;
        m_appWindow = nullptr;
        m_isVisible = false;
        m_hideAfterTick = 0;
        try
        {
            this->Close();
        }
        catch (...) {}
    }

    LRESULT CALLBACK OverlayWindow::SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        auto pThis = reinterpret_cast<OverlayWindow*>(dwRefData);
        if (!pThis) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        if (uMsg == WM_ERASEBKGND)
        {
            return 1; // Prevent GDI white background flash before DWM/DirectComposition
        }

        if (uMsg == WM_NCHITTEST)
        {
            return HTTRANSPARENT; // Click-through overlay
        }

        if (uMsg == WM_TIMER)
        {
            if (wParam == IDT_OVERLAY_HIDE)
            {
                ULONGLONG now = GetTickCount64();
                if (now < pThis->m_hideAfterTick)
                {
                    // A subsequent hotkey press occurred or timer fired early; reset for remaining duration
                    UINT remaining = (UINT)(pThis->m_hideAfterTick - now);
                    if (remaining < 30) remaining = 30;
                    SetTimer(hWnd, IDT_OVERLAY_HIDE, remaining, NULL);
                    return 0;
                }
                KillTimer(hWnd, IDT_OVERLAY_HIDE);
                pThis->HideWindow();
                return 0;
            }
            else if (wParam == IDT_OVERLAY_UNCLOAK)
            {
                KillTimer(hWnd, IDT_OVERLAY_UNCLOAK);
                if (pThis->m_isVisible)
                {
                    BOOL cloak = FALSE;
                    DwmSetWindowAttribute(hWnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
                    pThis->StartShowAnimation();
                }
                return 0;
            }
            else if (wParam == IDT_HOTKEY_DEBOUNCE)
            {
                KillTimer(hWnd, IDT_HOTKEY_DEBOUNCE);
                pThis->ApplyPendingBrightness();
                return 0;
            }
        }

        if (uMsg == WM_NCDESTROY)
        {
            RemoveWindowSubclass(hWnd, SubclassProc, uIdSubclass);
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}
