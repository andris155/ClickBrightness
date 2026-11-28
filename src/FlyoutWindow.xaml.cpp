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
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Windows.System.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>

#define IDT_FLYOUT_DEBOUNCE 101

static bool IsHeaderNameTrimmed(const std::wstring& name, double fontSize, double maxWidth)
{
    if (name.empty()) return false;
    static std::wstring s_lastName;
    static double s_lastFontSize = 0.0;
    static double s_lastMaxWidth = 0.0;
    static bool s_lastResult = false;
    if (name == s_lastName && fontSize == s_lastFontSize && maxWidth == s_lastMaxWidth)
    {
        return s_lastResult;
    }
    winrt::Microsoft::UI::Xaml::Controls::TextBlock measureTb;
    measureTb.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Segoe UI Variable Text, Segoe UI"));
    measureTb.FontSize(fontSize);
    measureTb.Text(name);
    measureTb.Measure({ 10000.0f, 100.0f });
    s_lastName = name;
    s_lastFontSize = fontSize;
    s_lastMaxWidth = maxWidth;
    s_lastResult = measureTb.DesiredSize().Width > static_cast<float>(maxWidth);
    return s_lastResult;
}

namespace winrt::ClickBrightness::implementation
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
            const auto& effCfg = g_Settings.GetEffectiveMonitorConfig(m_targetMonitorId.empty() ? g_Settings.mainMonitorId : m_targetMonitorId);
            if (!effCfg.flyoutScrollEnabled)
            {
                e.Handled(true);
                return;
            }
            auto ptrPt = e.GetCurrentPoint(RootGrid());
            int delta = ptrPt.Properties().MouseWheelDelta();
            if (delta != 0)
            {
                int step = (delta > 0) ? effCfg.flyoutScrollStep : -effCfg.flyoutScrollStep;
                OnScrollAdjust(m_syncBaseBrightness + step);
                e.Handled(true);
            }
        };
        BrightnessSlider().PointerWheelChanged(onWheel);
        RootGrid().PointerWheelChanged(onWheel);

        BrightnessSlider().Loaded([this](auto&&, auto&&)
        {
            ApplySliderBrushes();
        });

        // Keyboard navigation anywhere over the flyout
        RootGrid().KeyDown([this](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e)
        {
            auto key = e.Key();
            if (key == winrt::Windows::System::VirtualKey::Escape)
            {
                HideWindow();
                e.Handled(true);
                return;
            }

            if (!g_Settings.flyoutKeyboardEnabled)
            {
                if (key == winrt::Windows::System::VirtualKey::Up ||
                    key == winrt::Windows::System::VirtualKey::Down ||
                    key == winrt::Windows::System::VirtualKey::Left ||
                    key == winrt::Windows::System::VirtualKey::Right ||
                    key == winrt::Windows::System::VirtualKey::PageUp ||
                    key == winrt::Windows::System::VirtualKey::PageDown ||
                    key == winrt::Windows::System::VirtualKey::Home ||
                    key == winrt::Windows::System::VirtualKey::End)
                {
                    e.Handled(true);
                }
                return;
            }

            int step = g_Settings.flyoutScrollStep > 0 ? g_Settings.flyoutScrollStep : 5;
            if (key == winrt::Windows::System::VirtualKey::Up || key == winrt::Windows::System::VirtualKey::Right)
            {
                OnScrollAdjust(m_syncBaseBrightness + step);
                e.Handled(true);
            }
            else if (key == winrt::Windows::System::VirtualKey::Down || key == winrt::Windows::System::VirtualKey::Left)
            {
                OnScrollAdjust(m_syncBaseBrightness - step);
                e.Handled(true);
            }
            else if (key == winrt::Windows::System::VirtualKey::PageUp)
            {
                OnScrollAdjust(m_syncBaseBrightness + (step * 2 > 10 ? step * 2 : 10));
                e.Handled(true);
            }
            else if (key == winrt::Windows::System::VirtualKey::PageDown)
            {
                OnScrollAdjust(m_syncBaseBrightness - (step * 2 > 10 ? step * 2 : 10));
                e.Handled(true);
            }
            else if (key == winrt::Windows::System::VirtualKey::Home)
            {
                OnButtonClicked(0);
                e.Handled(true);
            }
            else if (key == winrt::Windows::System::VirtualKey::End)
            {
                OnButtonClicked(100);
                e.Handled(true);
            }
        });

        // Intercept keys on the slider before it handles them internally
        BrightnessSlider().PreviewKeyDown([this](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e)
        {
            if (!g_Settings.flyoutKeyboardEnabled)
            {
                auto key = e.Key();
                if (key == winrt::Windows::System::VirtualKey::Up ||
                    key == winrt::Windows::System::VirtualKey::Down ||
                    key == winrt::Windows::System::VirtualKey::Left ||
                    key == winrt::Windows::System::VirtualKey::Right ||
                    key == winrt::Windows::System::VirtualKey::PageUp ||
                    key == winrt::Windows::System::VirtualKey::PageDown ||
                    key == winrt::Windows::System::VirtualKey::Home ||
                    key == winrt::Windows::System::VirtualKey::End)
                {
                    e.Handled(true);
                }
            }
        });

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
            BOOL cloak = TRUE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
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
                    presenter.IsAlwaysOnTop(false);
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
            exStyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME | WS_EX_TOPMOST);
            exStyle |= WS_EX_TOOLWINDOW;
            SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exStyle);

            SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);

            COLORREF borderColor = DWMWA_COLOR_DEFAULT;
            DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));

            bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
            BOOL darkMode = isDark ? TRUE : FALSE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

            DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
            DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

            int backdropType = DWMSBT_NONE;
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

    static void SetPanelButtonsEnabled(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& panel, bool enabled)
    {
        if (!panel) return;
        for (auto const& child : panel.Children())
        {
            if (auto ctrl = child.try_as<winrt::Microsoft::UI::Xaml::Controls::Control>())
            {
                ctrl.IsEnabled(enabled);
            }
        }
    }

    void FlyoutWindow::PopulatePresetButtons(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& panel, std::function<void(int)> onClick)
    {
        panel.Children().Clear();
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
            btn.Click([onClick, val](auto&&, auto&&)
            {
                onClick(val);
            });
            panel.Children().Append(btn);
        }
    }

    void FlyoutWindow::InitializeButtons()
    {
        PopulatePresetButtons(ButtonStack(), [this](int val)
        {
            OnButtonClicked(val);
        });
    }

    void FlyoutWindow::UpdateTheme()
    {
        std::wstring effId = m_targetMonitorId.empty() ? g_Settings.mainMonitorId : m_targetMonitorId;
        const auto& effCfg = g_Settings.GetEffectiveMonitorConfig(effId);
        bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
        int curTrans = g_Settings.transparencyEnabled ? 1 : 0;
        COLORREF accCol = effCfg.flyoutUseWindowsAccent
            ? ThemeHelper::GetAccentColors().GetForTheme(!isDark)
            : ((effCfg.flyoutUseDualColors && isDark) ? effCfg.flyoutAccentColorDark : effCfg.flyoutAccentColor);

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
            }

            RootGrid().UpdateLayout();
        }

        ApplySliderBrushes();
    }

    static void UpdateSliderVisualElements(
        winrt::Microsoft::UI::Xaml::DependencyObject const& root,
        winrt::Microsoft::UI::Xaml::Media::Brush const& trackBrush,
        winrt::Microsoft::UI::Xaml::Media::Brush const& thumbBrush)
    {
        if (!root) return;
        int count = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(root);
        for (int i = 0; i < count; ++i)
        {
            auto child = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChild(root, i);
            if (auto thumb = child.try_as<winrt::Microsoft::UI::Xaml::Controls::Primitives::Thumb>())
            {
                if (thumbBrush)
                {
                    thumb.Background(thumbBrush);
                }
            }
            else if (auto rect = child.try_as<winrt::Microsoft::UI::Xaml::Shapes::Rectangle>())
            {
                auto name = rect.Name();
                if (name == L"VerticalDecreaseRect" || name == L"HorizontalDecreaseRect")
                {
                    if (trackBrush)
                    {
                        rect.Fill(trackBrush);
                    }
                }
            }
            UpdateSliderVisualElements(child, trackBrush, thumbBrush);
        }
    }

    void FlyoutWindow::ApplySliderBrushesForControl(winrt::Microsoft::UI::Xaml::Controls::Slider const& slider, const std::wstring& monitorId)
    {
        if (!slider) return;
        std::wstring effId = monitorId.empty() ? g_Settings.mainMonitorId : monitorId;
        const auto& effCfg = g_Settings.GetEffectiveMonitorConfig(effId);
        bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
        COLORREF accCol = effCfg.flyoutUseWindowsAccent
            ? ThemeHelper::GetAccentColors().GetForTheme(!isDark)
            : ((effCfg.flyoutUseDualColors && isDark) ? effCfg.flyoutAccentColorDark : effCfg.flyoutAccentColor);

        uint64_t brushKey = (static_cast<uint64_t>(accCol) << 1) | (effCfg.flyoutUseWindowsAccent ? 1ULL : 0ULL);
        bool hasVisualChildren = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(slider) > 0;
        if (hasVisualChildren)
        {
            if (auto tag = slider.Tag())
            {
                if (auto prevKey = tag.try_as<uint64_t>(); prevKey && *prevKey == brushKey)
                {
                    return;
                }
            }
        }

        winrt::Windows::UI::Color accColor = winrt::Microsoft::UI::ColorHelper::FromArgb(
            255, GetRValue(accCol), GetGValue(accCol), GetBValue(accCol)
        );

        auto defaultBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(accColor);

        auto res = slider.Resources();
        res.Clear();
        res.Insert(winrt::box_value(L"SliderTrackValueFill"), defaultBrush);
        res.Insert(winrt::box_value(L"SliderTrackValueFillPointerOver"), defaultBrush);
        res.Insert(winrt::box_value(L"SliderTrackValueFillPressed"), defaultBrush);
        res.Insert(winrt::box_value(L"SliderThumbBackground"), defaultBrush);

        if (!effCfg.flyoutUseWindowsAccent)
        {
            auto hoverBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(accColor);
            hoverBrush.Opacity(0.9f);

            auto pressedBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(accColor);
            pressedBrush.Opacity(0.8f);

            res.Insert(winrt::box_value(L"SliderThumbBackgroundPointerOver"), hoverBrush);
            res.Insert(winrt::box_value(L"SliderThumbBackgroundPressed"), pressedBrush);
        }

        slider.Foreground(defaultBrush);
        UpdateSliderVisualElements(slider, defaultBrush, defaultBrush);
        winrt::Microsoft::UI::Xaml::VisualStateManager::GoToState(slider, L"PointerOver", false);
        winrt::Microsoft::UI::Xaml::VisualStateManager::GoToState(slider, L"Normal", false);
        if (hasVisualChildren)
        {
            slider.Tag(winrt::box_value(brushKey));
        }
    }

    void FlyoutWindow::ApplySliderBrushes()
    {
        ApplySliderBrushesForControl(BrightnessSlider(), m_targetMonitorId);
        for (const auto& entry : m_syncEntries)
        {
            ApplySliderBrushesForControl(entry.slider, entry.monitorId);
        }
    }

    bool FlyoutWindow::RebuildSyncMonitorsPanel()
    {
        bool isSyncGroup = m_targetMonitorId.empty() || _wcsicmp(m_targetMonitorId.c_str(), L"__SYNC__") == 0;
        std::wstring sig = isSyncGroup ? L"SYNC|" : L"SEP";
        if (isSyncGroup)
        {
            sig += g_Settings.mainMonitorId + L"|";
            sig += g_Settings.showSeparateInMultiFlyout ? L"1|" : L"0|";
            sig += g_Settings.disableTurnedOffControl ? L"1|" : L"0|";
            for (const auto& mon : g_MonitorManager.GetMonitors())
            {
                if (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0) continue;
                const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
                if (!cfg || !cfg->enabled) continue;
                if (cfg->controlMode == 1 && !g_Settings.showSeparateInMultiFlyout) continue;
                if (g_Settings.disableTurnedOffControl && mon.isPoweredOff) continue;
                std::wstring monName = !cfg->customName.empty() ? cfg->customName : AppSettings::SanitizeMonitorName(mon.friendlyName, 20);
                sig += mon.id + L":" + std::to_wstring(cfg->controlMode) + L":" + monName + L";";
            }
        }
        if (sig == m_lastSyncSignature)
        {
            return false;
        }
        m_lastSyncSignature = sig;

        m_syncEntries.clear();
        SyncHeadersPanel().Children().Clear();
        SyncMonitorsPanel().Children().Clear();

        if (!isSyncGroup)
        {
            TitleText().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            MainColumnHeader().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            SyncHeadersPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            SyncMonitorsPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            return true;
        }

        // Two passes: sync-mode monitors first, then separate-mode monitors (if enabled)
        for (int pass = 0; pass < 2; ++pass)
        {
            bool wantSeparate = (pass == 1);
            if (wantSeparate && !g_Settings.showSeparateInMultiFlyout) continue;
            for (const auto& mon : g_MonitorManager.GetMonitors())
            {
                if (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0) continue;
                const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
                if (!cfg || !cfg->enabled) continue;
                bool isSeparate = (cfg->controlMode == 1);
                if (isSeparate != wantSeparate) continue;
                if (g_Settings.disableTurnedOffControl && mon.isPoweredOff) continue;

                std::wstring monName = !cfg->customName.empty() ? cfg->customName : AppSettings::SanitizeMonitorName(mon.friendlyName, 20);
                if (monName.empty()) monName = mon.id;
                int effB = isSeparate
                    ? (std::clamp)(g_MonitorManager.GetCachedMonitorBrightnessById(mon.id), 0, 100)
                    : g_MonitorManager.GetEffectiveSyncedBrightness(mon.id, m_syncBaseBrightness);

                winrt::Microsoft::UI::Xaml::Controls::TextBlock hdr;
                hdr.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Segoe UI Variable Text, Segoe UI"));
                hdr.FontSize(12);
                hdr.TextAlignment(winrt::Microsoft::UI::Xaml::TextAlignment::Center);
                hdr.TextTrimming(winrt::Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
                hdr.Width(64);
                hdr.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Left);
                winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
                    hdr, IsHeaderNameTrimmed(monName, 12.0, 64.0) ? winrt::box_value(monName) : nullptr);
                if (isSeparate)
                {
                    hdr.Margin({ 25, 0, 0, 0 });
                }
                if (mon.isPoweredOff)
                {
                    if (g_Settings.disableTurnedOffControl)
                    {
                        hdr.Text(monName + L"\n(" + g_Loc.Get(L"Common_Off", L"Off") + L")");
                    }
                    else
                    {
                        hdr.Text(monName + L"\n" + std::to_wstring(effB) + L"% (" + g_Loc.Get(L"Common_Off", L"Off") + L")");
                    }
                    hdr.Opacity(0.55);
                }
                else
                {
                    hdr.Text(monName + L"\n" + std::to_wstring(effB) + L"%");
                    hdr.Opacity(1.0);
                }

                winrt::Microsoft::UI::Xaml::Controls::Slider sl;
                sl.Orientation(winrt::Microsoft::UI::Xaml::Controls::Orientation::Vertical);
                sl.Minimum(0);
                sl.Maximum(100);
                sl.StepFrequency(1);
                sl.SmallChange(1);
                sl.LargeChange(5);
                sl.Height(502);
                sl.IsThumbToolTipEnabled(true);
                sl.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Top);
                sl.Value(effB);

                std::wstring monIdCopy = mon.id;
                sl.ValueChanged([this, monIdCopy, hdr, monName](auto&&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
                {
                    if (m_isUpdatingSlider) return;
                    int v = (std::clamp)(static_cast<int>(std::round(e.NewValue())), 0, 100);
                    if (g_MonitorManager.IsMonitorPoweredOff(monIdCopy))
                    {
                        hdr.Text(monName + L"\n" + std::to_wstring(v) + L"% (" + g_Loc.Get(L"Common_Off", L"Off") + L")");
                        hdr.Opacity(0.55);
                    }
                    else
                    {
                        hdr.Text(monName + L"\n" + std::to_wstring(v) + L"%");
                        hdr.Opacity(1.0);
                    }
                    g_MonitorManager.SetMonitorBrightnessTarget(monIdCopy, v, true);
                    g_TrayIcon.UpdateBrightnessForTarget(monIdCopy, v);
                    m_hasSeparatePendingSave = true;
                });

                sl.Loaded([this, sl, monIdCopy](auto&&, auto&&)
                {
                    ApplySliderBrushesForControl(sl, monIdCopy);
                });

                winrt::Microsoft::UI::Xaml::Controls::Grid colHost;
                colHost.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Top);
                winrt::Microsoft::UI::Xaml::Controls::StackPanel sepButtons{ nullptr };
                if (isSeparate)
                {
                    colHost.Width(89);
                    colHost.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Transparent()));
                    winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition c0;
                    c0.Width(winrt::Microsoft::UI::Xaml::GridLengthHelper::Auto());
                    winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition c1;
                    c1.Width(winrt::Microsoft::UI::Xaml::GridLengthHelper::Auto());
                    colHost.ColumnDefinitions().Append(c0);
                    colHost.ColumnDefinitions().Append(c1);

                    auto applySeparateValue = [this, monIdCopy, hdr, monName, sl](int val, bool saveNow)
                    {
                        if (g_Settings.disableTurnedOffControl && g_MonitorManager.IsMonitorPoweredOff(monIdCopy)) return;
                        int clamped = (std::clamp)(val, 0, 100);
                        m_isUpdatingSlider = true;
                        sl.Value(clamped);
                        m_isUpdatingSlider = false;
                        if (g_MonitorManager.IsMonitorPoweredOff(monIdCopy))
                        {
                            hdr.Text(monName + L"\n" + std::to_wstring(clamped) + L"% (" + g_Loc.Get(L"Common_Off", L"Off") + L")");
                            hdr.Opacity(0.55);
                        }
                        else
                        {
                            hdr.Text(monName + L"\n" + std::to_wstring(clamped) + L"%");
                            hdr.Opacity(1.0);
                        }
                        g_MonitorManager.SetMonitorBrightnessTarget(monIdCopy, clamped, true);
                        g_TrayIcon.UpdateBrightnessForTarget(monIdCopy, clamped);
                        if (saveNow)
                        {
                            m_hasSeparatePendingSave = false;
                            g_Settings.Save();
                        }
                        else
                        {
                            m_hasSeparatePendingSave = true;
                        }
                    };

                    auto onSepWheel = [this, monIdCopy, sl, applySeparateValue](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
                    {
                        if (e.Handled()) return;
                        if (g_Settings.disableTurnedOffControl && g_MonitorManager.IsMonitorPoweredOff(monIdCopy))
                        {
                            e.Handled(true);
                            return;
                        }
                        const auto& sepCfg = g_Settings.GetEffectiveMonitorConfig(monIdCopy);
                        if (!sepCfg.flyoutScrollEnabled)
                        {
                            e.Handled(true);
                            return;
                        }
                        auto ptrPt = e.GetCurrentPoint(RootGrid());
                        int delta = ptrPt.Properties().MouseWheelDelta();
                        if (delta != 0)
                        {
                            int cur = static_cast<int>(std::round(sl.Value()));
                            int step = (delta > 0) ? sepCfg.flyoutScrollStep : -sepCfg.flyoutScrollStep;
                            applySeparateValue(cur + step, false);
                            e.Handled(true);
                        }
                    };
                    sl.PointerWheelChanged(onSepWheel);
                    colHost.PointerWheelChanged(onSepWheel);

                    sepButtons = winrt::Microsoft::UI::Xaml::Controls::StackPanel();
                    sepButtons.Spacing(2);
                    sepButtons.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Top);
                    PopulatePresetButtons(sepButtons, [applySeparateValue](int val)
                    {
                        applySeparateValue(val, true);
                    });
                    winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(sepButtons, 0);
                    colHost.Children().Append(sepButtons);

                    sl.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Left);
                    sl.Margin({ 7, 0, 0, 0 });
                    winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(sl, 1);
                    colHost.Children().Append(sl);
                }
                else
                {
                    colHost.Width(64);
                    sl.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
                    colHost.Children().Append(sl);
                }

                bool colEnabled = !(g_Settings.disableTurnedOffControl && mon.isPoweredOff);
                auto colVis = colEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
                sl.IsEnabled(colEnabled);
                sl.Visibility(colVis);
                SetPanelButtonsEnabled(sepButtons, colEnabled);
                if (sepButtons) sepButtons.Visibility(colVis);

                SyncHeadersPanel().Children().Append(hdr);
                SyncMonitorsPanel().Children().Append(colHost);
                m_syncEntries.push_back({ mon.id, monName, isSeparate, hdr, sl, sepButtons });
                ApplySliderBrushesForControl(sl, mon.id);
            }
        }

        if (!m_syncEntries.empty())
        {
            TitleText().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            MainColumnHeader().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            SyncHeadersPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            SyncMonitorsPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        }
        else
        {
            TitleText().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            MainColumnHeader().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            SyncHeadersPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            SyncMonitorsPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }
        return true;
    }

    void FlyoutWindow::ResetGraphics()
    {
        DetachBackdrop();
        m_lastTheme = -1;
        m_lastTransparency = -1;
        SetupBackdrop();
        UpdateTheme();
        InitializeButtons();
        SetBrightnessVisual(m_brightness);
    }

    void FlyoutWindow::SetBrightnessVisual(int val)
    {
        int minB = 0, maxB = 100;
        g_MonitorManager.GetTargetBounds(m_targetMonitorId, minB, maxB);
        m_syncBaseBrightness = (std::clamp)(val, minB, maxB);
        bool isMainOrSync = m_targetMonitorId.empty() ||
                            _wcsicmp(m_targetMonitorId.c_str(), L"__SYNC__") == 0 ||
                            _wcsicmp(m_targetMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0;
        m_brightness = isMainOrSync
            ? g_MonitorManager.GetEffectiveSyncedBrightness(g_Settings.mainMonitorId, m_syncBaseBrightness)
            : (std::clamp)(m_syncBaseBrightness, 0, 100);
        int enabledCount = 0;
        for (const auto& m : g_MonitorManager.GetMonitors())
        {
            if (g_Settings.IsMonitorSelected(m.id)) enabledCount++;
        }

        std::wstring targetId = m_targetMonitorId.empty() ? g_Settings.mainMonitorId : m_targetMonitorId;
        bool isTargetOff = g_MonitorManager.IsMonitorPoweredOff(targetId);
        const auto* cfg = g_Settings.FindMonitorConfig(targetId);
        if (enabledCount > 1 && cfg && !cfg->customName.empty())
        {
            TitleText().FontSize(12);
            TitleText().Width(96);
            TitleText().Margin(winrt::Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 10 });
            winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
                TitleText(), IsHeaderNameTrimmed(cfg->customName, 12.0, 96.0) ? winrt::box_value(cfg->customName) : nullptr);
            std::wstring titleText = (isTargetOff && g_Settings.disableTurnedOffControl)
                ? (cfg->customName + L"\n(" + g_Loc.Get(L"Common_Off", L"Off") + L")")
                : (cfg->customName + L"\n" + std::to_wstring(m_brightness) + L"%");
            if (isTargetOff)
            {
                if (!g_Settings.disableTurnedOffControl)
                {
                    titleText += L" (" + g_Loc.Get(L"Common_Off", L"Off") + L")";
                }
                TitleText().Opacity(0.55);
            }
            else
            {
                TitleText().Opacity(1.0);
            }
            TitleText().Text(titleText);
        }
        else
        {
            TitleText().FontSize(13.5);
            TitleText().Width(std::numeric_limits<double>::quiet_NaN());
            TitleText().Margin(winrt::Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 14 });
            winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(TitleText(), nullptr);
            std::wstring fmt = g_Loc.Get(L"Flyout_Brightness", L"Brightness %d%%");
            wchar_t bbuf[64];
            swprintf_s(bbuf, fmt.c_str(), m_brightness);
            std::wstring titleText = bbuf;
            if (isTargetOff)
            {
                if (g_Settings.disableTurnedOffControl)
                {
                    titleText = L"(" + g_Loc.Get(L"Common_Off", L"Off") + L")";
                }
                else
                {
                    titleText += L" (" + g_Loc.Get(L"Common_Off", L"Off") + L")";
                }
                TitleText().Opacity(0.55);
            }
            else
            {
                TitleText().Opacity(1.0);
            }
            TitleText().Text(titleText);
        }

        m_isUpdatingSlider = true;
        BrightnessSlider().Value(m_brightness);
        bool singleTargetEnabled = !(m_syncEntries.empty() && g_Settings.disableTurnedOffControl && isTargetOff);
        auto singleVis = singleTargetEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
        BrightnessSlider().IsEnabled(singleTargetEnabled);
        BrightnessSlider().Visibility(singleVis);
        SetPanelButtonsEnabled(ButtonStack(), singleTargetEnabled);
        ButtonStack().Visibility(singleVis);
        if (!m_syncEntries.empty())
        {
            std::wstring mainName = (cfg && !cfg->customName.empty()) ? cfg->customName : g_Loc.Get(L"MainDisplay", L"Main").c_str();
            winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
                MainColumnHeader(), IsHeaderNameTrimmed(mainName, 12.0, 64.0) ? winrt::box_value(mainName) : nullptr);
            if (g_MonitorManager.IsMonitorPoweredOff(g_Settings.mainMonitorId))
            {
                MainColumnHeader().Text(mainName + L"\n" + std::to_wstring(m_brightness) + L"% (" + g_Loc.Get(L"Common_Off", L"Off") + L")");
                MainColumnHeader().Opacity(0.55);
            }
            else
            {
                MainColumnHeader().Text(mainName + L"\n" + std::to_wstring(m_brightness) + L"%");
                MainColumnHeader().Opacity(1.0);
            }
            for (const auto& entry : m_syncEntries)
            {
                const auto* sCfg = g_Settings.FindMonitorConfig(entry.monitorId);
                int effB = entry.isSeparate
                    ? (std::clamp)(g_MonitorManager.GetCachedMonitorBrightnessById(entry.monitorId), 0, 100)
                    : g_MonitorManager.GetEffectiveSyncedBrightness(entry.monitorId, m_syncBaseBrightness);
                bool isEntryOff = g_MonitorManager.IsMonitorPoweredOff(entry.monitorId);
                if (isEntryOff)
                {
                    if (g_Settings.disableTurnedOffControl)
                    {
                        entry.header.Text(entry.displayName + L"\n(" + g_Loc.Get(L"Common_Off", L"Off") + L")");
                    }
                    else
                    {
                        entry.header.Text(entry.displayName + L"\n" + std::to_wstring(effB) + L"% (" + g_Loc.Get(L"Common_Off", L"Off") + L")");
                    }
                    entry.header.Opacity(0.55);
                }
                else
                {
                    entry.header.Text(entry.displayName + L"\n" + std::to_wstring(effB) + L"%");
                    entry.header.Opacity(1.0);
                }
                bool colEnabled = !(g_Settings.disableTurnedOffControl && isEntryOff);
                auto colVis = colEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
                entry.slider.IsEnabled(colEnabled);
                entry.slider.Visibility(colVis);
                SetPanelButtonsEnabled(entry.sepButtons, colEnabled);
                if (entry.sepButtons) entry.sepButtons.Visibility(colVis);
                entry.slider.Value(effB);
            }
        }
        m_isUpdatingSlider = false;

        g_TrayIcon.UpdateBrightnessForTarget(m_targetMonitorId, m_syncBaseBrightness);
        if (m_targetMonitorId.empty() || _wcsicmp(m_targetMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0)
        {
            g_Settings.cachedBrightness = m_brightness;
        }
    }

    void FlyoutWindow::CommitHardwareBrightness(int val)
    {
        if (m_hwnd)
        {
            KillTimer(m_hwnd, IDT_FLYOUT_DEBOUNCE);
        }
        m_hasPendingBrightness = false;
        g_MonitorManager.SetMonitorBrightnessTarget(m_targetMonitorId, val);
        if (m_targetMonitorId.empty() || _wcsicmp(m_targetMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0)
        {
            g_Settings.cachedBrightness = val;
        }
        g_Settings.Save();
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
            CommitHardwareBrightness(m_syncBaseBrightness);
        }
        else
        {
            ScheduleDebounce(m_syncBaseBrightness, g_Settings.flyoutScrollDelayMs);
        }
    }

    void FlyoutWindow::OnButtonClicked(int val)
    {
        SetBrightnessVisual(val);
        if (!g_Settings.flyoutButtonDelayEnabled)
        {
            CommitHardwareBrightness(m_syncBaseBrightness);
        }
        else
        {
            ScheduleDebounce(m_syncBaseBrightness, g_Settings.flyoutButtonDelayMs);
        }
    }

    void FlyoutWindow::CommitBrightness(bool immediate)
    {
        g_TrayIcon.UpdateBrightnessForTarget(m_targetMonitorId, m_syncBaseBrightness);
        if (m_targetMonitorId.empty() || _wcsicmp(m_targetMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0)
        {
            g_Settings.cachedBrightness = m_brightness;
        }

        if (immediate || !g_Settings.flyoutScrollDelayEnabled)
        {
            CommitHardwareBrightness(m_syncBaseBrightness);
        }
        else
        {
            ScheduleDebounce(m_syncBaseBrightness, g_Settings.flyoutScrollDelayMs);
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
            g_MonitorManager.SetMonitorBrightnessTarget(m_targetMonitorId, m_pendingBrightness);
            if (m_targetMonitorId.empty() || _wcsicmp(m_targetMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0)
            {
                g_Settings.cachedBrightness = m_pendingBrightness;
            }
            g_Settings.Save();
        }
    }

    void FlyoutWindow::SetBrightness(int brightness, bool notifyHardware)
    {
        SetBrightnessForTarget(L"", brightness, notifyHardware);
    }

    void FlyoutWindow::SetBrightnessForTarget(const std::wstring& targetMonitorId, int brightness, bool notifyHardware)
    {
        bool match = (targetMonitorId.empty() && m_targetMonitorId.empty()) ||
                     (!targetMonitorId.empty() && _wcsicmp(targetMonitorId.c_str(), m_targetMonitorId.c_str()) == 0) ||
                     (m_targetMonitorId.empty() && !targetMonitorId.empty() && _wcsicmp(targetMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
        if (!match)
        {
            if (m_isVisible)
            {
                for (const auto& entry : m_syncEntries)
                {
                    if (entry.isSeparate && !targetMonitorId.empty() && _wcsicmp(targetMonitorId.c_str(), entry.monitorId.c_str()) == 0)
                    {
                        int clamped = (std::clamp)(brightness, 0, 100);
                        m_isUpdatingSlider = true;
                        entry.slider.Value(clamped);
                        m_isUpdatingSlider = false;
                        entry.header.Text(entry.displayName + L"\n" + std::to_wstring(clamped) + L"%");
                        return;
                    }
                }
            }
            return;
        }

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

    void FlyoutWindow::Prewarm()
    {
        m_syncBaseBrightness = g_MonitorManager.GetTargetControlBrightness(m_targetMonitorId);
        m_brightness = (std::clamp)(m_syncBaseBrightness, 0, 100);
        RebuildSyncMonitorsPanel();
        UpdateTheme();

        POINT pt = { 0, 0 };
        GetCursorPos(&pt);
        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        UINT dpi = DpiHelper::GetDpiForMonitor(hMon);
        if (dpi == 0 && m_hwnd) dpi = GetDpiForWindow(m_hwnd);
        if (dpi == 0) dpi = 96;

        int extraWidth = 0;
        for (const auto& entry : m_syncEntries)
        {
            extraWidth += entry.isSeparate ? 95 : 70;
        }
        int enabledCount = 0;
        for (const auto& m : g_MonitorManager.GetMonitors())
        {
            if (g_Settings.IsMonitorSelected(m.id)) enabledCount++;
        }
        std::wstring targetIdForHeight = m_targetMonitorId.empty() ? g_Settings.mainMonitorId : m_targetMonitorId;
        const auto* cfgForHeight = g_Settings.FindMonitorConfig(targetIdForHeight);
        bool isTwoRowTitle = (enabledCount > 1 && cfgForHeight && !cfgForHeight->customName.empty());

        int width = DpiHelper::Scale(!m_syncEntries.empty() ? (137 + extraWidth) : 121, dpi);
        int height = DpiHelper::Scale(!m_syncEntries.empty() ? 574 : (isTwoRowTitle ? 576 : 566), dpi);
        m_lastW = width;
        m_lastH = height;

        if (m_hwnd)
        {
            SetWindowPos(m_hwnd, NULL, -32000, -32000, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        SetBrightnessVisual(m_syncBaseBrightness);
        RootGrid().UpdateLayout();
    }

    void FlyoutWindow::RefreshLayoutInPlace()
    {
        if (!m_isVisible)
        {
            Prewarm();
            return;
        }

        m_syncBaseBrightness = g_MonitorManager.GetTargetControlBrightness(m_targetMonitorId);
        m_brightness = (std::clamp)(m_syncBaseBrightness, 0, 100);
        bool rebuilt = RebuildSyncMonitorsPanel();
        UpdateTheme();

        RECT rc = {};
        if (m_hwnd) GetWindowRect(m_hwnd, &rc);
        POINT pt = { rc.left + (rc.right - rc.left) / 2, rc.top + (rc.bottom - rc.top) / 2 };
        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(hMon, &mi);
        UINT dpi = DpiHelper::GetDpiForMonitor(hMon);
        if (dpi == 0 && m_hwnd) dpi = GetDpiForWindow(m_hwnd);
        if (dpi == 0) dpi = 96;

        int extraWidth = 0;
        for (const auto& entry : m_syncEntries)
        {
            extraWidth += entry.isSeparate ? 95 : 70;
        }
        int enabledCount = 0;
        for (const auto& m : g_MonitorManager.GetMonitors())
        {
            if (g_Settings.IsMonitorSelected(m.id)) enabledCount++;
        }
        std::wstring targetIdForHeight = m_targetMonitorId.empty() ? g_Settings.mainMonitorId : m_targetMonitorId;
        const auto* cfgForHeight = g_Settings.FindMonitorConfig(targetIdForHeight);
        bool isTwoRowTitle = (enabledCount > 1 && cfgForHeight && !cfgForHeight->customName.empty());

        int width = DpiHelper::Scale(!m_syncEntries.empty() ? (137 + extraWidth) : 121, dpi);
        int height = DpiHelper::Scale(!m_syncEntries.empty() ? 574 : (isTwoRowTitle ? 576 : 566), dpi);
        bool sizeChanged = (m_lastW != width || m_lastH != height);
        m_lastW = width;
        m_lastH = height;

        SetBrightnessVisual(m_syncBaseBrightness);
        if (rebuilt || sizeChanged)
        {
            RootGrid().UpdateLayout();
        }

        if (m_hwnd && sizeChanged)
        {
            int x = rc.right - width;
            int y = rc.bottom - height;
            if (x + width > mi.rcWork.right) x = mi.rcWork.right - width - DpiHelper::Scale(12, dpi);
            if (x < mi.rcWork.left) x = mi.rcWork.left + DpiHelper::Scale(12, dpi);
            if (y + height > mi.rcWork.bottom) y = mi.rcWork.bottom - height - DpiHelper::Scale(12, dpi);
            if (y < mi.rcWork.top) y = mi.rcWork.top + DpiHelper::Scale(12, dpi);
            SetWindowPos(m_hwnd, HWND_TOP, x, y, width, height, SWP_NOACTIVATE);
        }
    }

    void FlyoutWindow::ShowWindow(const POINT* pTrayPos)
    {
        CancelPendingShow();
        g_MonitorManager.RefreshExternalPowerStatesAsync();
        g_OverlayWindow.ApplyPendingBrightness();
        ApplyPendingBrightness();
        m_syncBaseBrightness = g_MonitorManager.GetTargetControlBrightness(m_targetMonitorId);
        m_brightness = (std::clamp)(m_syncBaseBrightness, 0, 100);
        m_pendingBrightness = m_syncBaseBrightness;
        m_hasPendingBrightness = false;
        bool rebuilt = RebuildSyncMonitorsPanel();
        UpdateTheme();

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

        int extraWidth = 0;
        for (const auto& entry : m_syncEntries)
        {
            extraWidth += entry.isSeparate ? 95 : 70;
        }
        int enabledCount = 0;
        for (const auto& m : g_MonitorManager.GetMonitors())
        {
            if (g_Settings.IsMonitorSelected(m.id)) enabledCount++;
        }
        std::wstring targetIdForHeight = m_targetMonitorId.empty() ? g_Settings.mainMonitorId : m_targetMonitorId;
        const auto* cfgForHeight = g_Settings.FindMonitorConfig(targetIdForHeight);
        bool isTwoRowTitle = (enabledCount > 1 && cfgForHeight && !cfgForHeight->customName.empty());

        int width = DpiHelper::Scale(!m_syncEntries.empty() ? (137 + extraWidth) : 121, dpi);
        int height = DpiHelper::Scale(!m_syncEntries.empty() ? 574 : (isTwoRowTitle ? 576 : 566), dpi);

        bool sizeChanged = (m_lastW != width || m_lastH != height);
        m_lastW = width;
        m_lastH = height;

        // Resize while still offscreen before XAML layout so moving onscreen never shows an old-size frame
        if (m_hwnd && sizeChanged)
        {
            SetWindowPos(m_hwnd, NULL, -32000, -32000, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        SetBrightnessVisual(m_syncBaseBrightness);
        if (rebuilt || sizeChanged)
        {
            RootGrid().UpdateLayout();
        }

        // Check if opened from Windows hidden icons overflow window
        HWND hOverflow = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", NULL);
        if (!hOverflow)
        {
            hOverflow = FindWindowW(L"NotifyIconOverflowWindow", NULL);
        }
        RECT rcOverflow = { 0 };
        bool isFromOverflow = false;
        if (hOverflow && ::IsWindowVisible(hOverflow))
        {
            RECT rcFrame = { 0 };
            if (SUCCEEDED(DwmGetWindowAttribute(hOverflow, DWMWA_EXTENDED_FRAME_BOUNDS, &rcFrame, sizeof(rcFrame))) &&
                (rcFrame.right > rcFrame.left) && (rcFrame.bottom > rcFrame.top))
            {
                rcOverflow = rcFrame;
            }
            else
            {
                GetWindowRect(hOverflow, &rcOverflow);
            }

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
        nii.uID = (m_targetTrayId != 0) ? m_targetTrayId : g_TrayIcon.GetId();
        if (m_targetTrayId == 0 && !m_targetMonitorId.empty())
        {
            for (const auto& slot : g_TrayIcon.GetExtraSlots())
            {
                if (slot.created && _wcsicmp(slot.monitorId.c_str(), m_targetMonitorId.c_str()) == 0)
                {
                    nii.uID = slot.uId;
                    break;
                }
            }
        }
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

        // Determine taskbar docking edge
        enum class TaskbarEdge { Bottom, Top, Left, Right };
        TaskbarEdge edge = TaskbarEdge::Bottom;

        int diffLeft = mi.rcWork.left - mi.rcMonitor.left;
        int diffRight = mi.rcMonitor.right - mi.rcWork.right;
        int diffTop = mi.rcWork.top - mi.rcMonitor.top;
        int diffBottom = mi.rcMonitor.bottom - mi.rcWork.bottom;

        int maxDiff = diffBottom;
        if (diffTop > maxDiff) { maxDiff = diffTop; edge = TaskbarEdge::Top; }
        if (diffLeft > maxDiff) { maxDiff = diffLeft; edge = TaskbarEdge::Left; }
        if (diffRight > maxDiff) { maxDiff = diffRight; edge = TaskbarEdge::Right; }

        if (maxDiff <= 4)
        {
            // Auto-hide taskbar or secondary display without work area inset: infer edge from tray icon position
            int distLeft = abs(pt.x - mi.rcMonitor.left);
            int distRight = abs(mi.rcMonitor.right - pt.x);
            int distTop = abs(pt.y - mi.rcMonitor.top);
            int distBottom = abs(mi.rcMonitor.bottom - pt.y);

            int minDist = distBottom;
            edge = TaskbarEdge::Bottom;
            if (distTop < minDist) { minDist = distTop; edge = TaskbarEdge::Top; }
            if (distLeft < minDist) { minDist = distLeft; edge = TaskbarEdge::Left; }
            if (distRight < minDist) { minDist = distRight; edge = TaskbarEdge::Right; }
        }

        int x = pt.x - width / 2;
        int y = pt.y - height / 2;

        if (isFromOverflow && (rcOverflow.bottom > rcOverflow.top))
        {
            switch (edge)
            {
            case TaskbarEdge::Left:
                x = rcOverflow.right + DpiHelper::Scale(2, dpi);
                y = rcOverflow.bottom - height;
                break;
            case TaskbarEdge::Right:
                x = rcOverflow.left - width - DpiHelper::Scale(2, dpi);
                y = rcOverflow.bottom - height;
                break;
            case TaskbarEdge::Top:
                x = pt.x - width / 2;
                y = rcOverflow.bottom + DpiHelper::Scale(2, dpi);
                break;
            case TaskbarEdge::Bottom:
            default:
                x = pt.x - width / 2;
                y = rcOverflow.top - height - DpiHelper::Scale(2, dpi);
                if (y < mi.rcWork.top)
                {
                    // If it does not fit vertically above the overflow window, position to its left
                    if (rcOverflow.left - width - DpiHelper::Scale(2, dpi) >= mi.rcWork.left)
                    {
                        x = rcOverflow.left - width - DpiHelper::Scale(2, dpi);
                        y = mi.rcWork.bottom - height - DpiHelper::Scale(12, dpi);
                    }
                    else
                    {
                        y = mi.rcWork.top + DpiHelper::Scale(2, dpi);
                    }
                }
                break;
            }
        }
        else
        {
            switch (edge)
            {
            case TaskbarEdge::Left:
                x = mi.rcWork.left + DpiHelper::Scale(12, dpi);
                y = (rcIcon.bottom > rcIcon.top) ? (rcIcon.bottom - height) : (pt.y - height / 2);
                break;
            case TaskbarEdge::Right:
                x = mi.rcWork.right - width - DpiHelper::Scale(12, dpi);
                y = (rcIcon.bottom > rcIcon.top) ? (rcIcon.bottom - height) : (pt.y - height / 2);
                break;
            case TaskbarEdge::Top:
                x = pt.x - width / 2;
                y = mi.rcWork.top + DpiHelper::Scale(12, dpi);
                break;
            case TaskbarEdge::Bottom:
            default:
                x = pt.x - width / 2;
                y = mi.rcWork.bottom - height - DpiHelper::Scale(12, dpi);
                break;
            }
        }

        if (x + width > mi.rcWork.right)
        {
            x = mi.rcWork.right - width - DpiHelper::Scale(12, dpi);
        }
        if (x < mi.rcWork.left)
        {
            x = mi.rcWork.left + DpiHelper::Scale(12, dpi);
        }
        if (y + height > mi.rcWork.bottom)
        {
            y = mi.rcWork.bottom - height - DpiHelper::Scale(12, dpi);
        }
        if (y < mi.rcWork.top)
        {
            y = mi.rcWork.top + DpiHelper::Scale(12, dpi);
        }

        m_showTick = GetTickCount64();
        m_isVisible = true;

        if (m_hwnd)
        {
            if (!rebuilt && !sizeChanged)
            {
                SetWindowPos(m_hwnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                BOOL cloak = FALSE;
                DwmSetWindowAttribute(m_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
                SetForegroundWindow(m_hwnd);
                SetFocus(m_hwnd);
                try
                {
                    BrightnessSlider().IsTabStop(g_Settings.flyoutKeyboardEnabled);
                    if (g_Settings.flyoutKeyboardEnabled)
                    {
                        BrightnessSlider().Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
                    }
                }
                catch (...) {}
            }
            else
            {
                m_pendingShowFrames = 2;
                m_pendingShowX = x;
                m_pendingShowY = y;
                m_showRenderToken = winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(
                    [this](auto&&, auto&&)
                    {
                        if (m_pendingShowFrames <= 0) return;
                        if (--m_pendingShowFrames > 0) return;
                        int targetX = m_pendingShowX;
                        int targetY = m_pendingShowY;
                        CancelPendingShow();
                        if (!m_isVisible || !m_hwnd) return;
                        m_showTick = GetTickCount64();
                        SetWindowPos(m_hwnd, HWND_TOP, targetX, targetY, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                        BOOL cloak = FALSE;
                        DwmSetWindowAttribute(m_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
                        SetForegroundWindow(m_hwnd);
                        SetFocus(m_hwnd);
                        try
                        {
                            BrightnessSlider().IsTabStop(g_Settings.flyoutKeyboardEnabled);
                            if (g_Settings.flyoutKeyboardEnabled)
                            {
                                BrightnessSlider().Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
                            }
                        }
                        catch (...) {}
                    });
            }
        }
    }

    void FlyoutWindow::CancelPendingShow()
    {
        m_pendingShowFrames = 0;
        if (m_showRenderToken.value != 0)
        {
            try
            {
                winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(m_showRenderToken);
            }
            catch (...) {}
            m_showRenderToken = {};
        }
    }

    void FlyoutWindow::HideWindow()
    {
        CancelPendingShow();
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
            if (m_hasSeparatePendingSave)
            {
                m_hasSeparatePendingSave = false;
                g_Settings.Save();
            }
            if (m_hwnd)
            {
                BOOL cloak = TRUE;
                DwmSetWindowAttribute(m_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
                SetWindowPos(m_hwnd, NULL, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }

    void FlyoutWindow::DestroyWindow()
    {
        CancelPendingShow();
        ApplyPendingBrightness();
        if (m_hasSeparatePendingSave)
        {
            m_hasSeparatePendingSave = false;
            g_Settings.Save();
        }
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

        if (uMsg == WM_KEYDOWN)
        {
            if (wParam == VK_ESCAPE)
            {
                pThis->HideWindow();
                return 0;
            }

            if (!g_Settings.flyoutKeyboardEnabled)
            {
                switch (wParam)
                {
                case VK_UP:
                case VK_RIGHT:
                case VK_DOWN:
                case VK_LEFT:
                case VK_PRIOR:
                case VK_NEXT:
                case VK_HOME:
                case VK_END:
                    return 0;
                }
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);
            }

            int step = g_Settings.flyoutScrollStep > 0 ? g_Settings.flyoutScrollStep : 5;
            switch (wParam)
            {
            case VK_UP:
            case VK_RIGHT:
                pThis->OnScrollAdjust(pThis->m_brightness + step);
                return 0;
            case VK_DOWN:
            case VK_LEFT:
                pThis->OnScrollAdjust(pThis->m_brightness - step);
                return 0;
            case VK_PRIOR: // Page Up
                pThis->OnScrollAdjust(pThis->m_brightness + (step * 2 > 10 ? step * 2 : 10));
                return 0;
            case VK_NEXT: // Page Down
                pThis->OnScrollAdjust(pThis->m_brightness - (step * 2 > 10 ? step * 2 : 10));
                return 0;
            case VK_HOME:
                pThis->OnButtonClicked(0);
                return 0;
            case VK_END:
                pThis->OnButtonClicked(100);
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
