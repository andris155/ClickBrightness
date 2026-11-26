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

void TrimProcessWorkingSetAsync();

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
            : effCfg.flyoutAccentColor;

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
            : effCfg.flyoutAccentColor;

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
    }

    void FlyoutWindow::ApplySliderBrushes()
    {
        ApplySliderBrushesForControl(BrightnessSlider(), m_targetMonitorId);
        for (const auto& entry : m_syncEntries)
        {
            ApplySliderBrushesForControl(entry.slider, entry.monitorId);
        }
    }

    void FlyoutWindow::RebuildSyncMonitorsPanel()
    {
        m_syncEntries.clear();
        SyncHeadersPanel().Children().Clear();
        SyncMonitorsPanel().Children().Clear();

        bool isSyncGroup = m_targetMonitorId.empty() || _wcsicmp(m_targetMonitorId.c_str(), L"__SYNC__") == 0;
        if (!isSyncGroup)
        {
            TitleText().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            MainColumnHeader().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            SyncHeadersPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            SyncMonitorsPanel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            return;
        }

        // Two passes: sync-mode monitors first, then separate-mode monitors (each with individual preset buttons)
        for (int pass = 0; pass < 2; ++pass)
        {
            bool wantSeparate = (pass == 1);
            for (const auto& mon : g_MonitorManager.GetMonitors())
            {
                if (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0) continue;
                const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
                if (!cfg || !cfg->enabled) continue;
                bool isSeparate = (cfg->controlMode == 1);
                if (isSeparate != wantSeparate) continue;

                std::wstring monName = !cfg->customName.empty() ? cfg->customName : AppSettings::SanitizeMonitorName(mon.friendlyName, 20);
                if (monName.empty()) monName = mon.id;
                int effB = isSeparate
                    ? (std::clamp)(g_MonitorManager.GetMonitorBrightnessById(mon.id), 0, 100)
                    : AppSettings::ComputeSyncedBrightness(m_syncBaseBrightness, *cfg);

                winrt::Microsoft::UI::Xaml::Controls::TextBlock hdr;
                hdr.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Segoe UI Variable Text, Segoe UI"));
                hdr.FontSize(12);
                hdr.TextAlignment(winrt::Microsoft::UI::Xaml::TextAlignment::Center);
                hdr.TextTrimming(winrt::Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
                hdr.Width(64);
                hdr.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Left);
                if (isSeparate)
                {
                    hdr.Margin({ 25, 0, 0, 0 });
                }
                hdr.Text(monName + L"\n" + std::to_wstring(effB) + L"%");

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
                    hdr.Text(monName + L"\n" + std::to_wstring(v) + L"%");
                    g_MonitorManager.SetMonitorBrightnessTarget(monIdCopy, v, true);
                    g_TrayIcon.UpdateBrightnessForTarget(monIdCopy, v);
                });

                sl.Loaded([this, sl, monIdCopy](auto&&, auto&&)
                {
                    ApplySliderBrushesForControl(sl, monIdCopy);
                });

                winrt::Microsoft::UI::Xaml::Controls::Grid colHost;
                colHost.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Top);
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

                    auto applySeparateValue = [this, monIdCopy, hdr, monName, sl](int val)
                    {
                        int clamped = (std::clamp)(val, 0, 100);
                        m_isUpdatingSlider = true;
                        sl.Value(clamped);
                        m_isUpdatingSlider = false;
                        hdr.Text(monName + L"\n" + std::to_wstring(clamped) + L"%");
                        g_MonitorManager.SetMonitorBrightnessTarget(monIdCopy, clamped, true);
                        g_TrayIcon.UpdateBrightnessForTarget(monIdCopy, clamped);
                        g_Settings.SaveBrightnessOnly();
                    };

                    auto onSepWheel = [this, monIdCopy, sl, applySeparateValue](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e)
                    {
                        if (e.Handled()) return;
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
                            applySeparateValue(cur + step);
                            e.Handled(true);
                        }
                    };
                    sl.PointerWheelChanged(onSepWheel);
                    colHost.PointerWheelChanged(onSepWheel);

                    winrt::Microsoft::UI::Xaml::Controls::StackPanel sepButtons;
                    sepButtons.Spacing(2);
                    sepButtons.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Top);
                    PopulatePresetButtons(sepButtons, applySeparateValue);
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

                SyncHeadersPanel().Children().Append(hdr);
                SyncMonitorsPanel().Children().Append(colHost);
                m_syncEntries.push_back({ mon.id, monName, isSeparate, hdr, sl });
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
        int minB = 0, maxB = 100;
        g_MonitorManager.GetTargetBounds(m_targetMonitorId, minB, maxB);
        m_syncBaseBrightness = (std::clamp)(val, minB, maxB);
        m_brightness = (std::clamp)(m_syncBaseBrightness, 0, 100);
        int enabledCount = 0;
        for (const auto& m : g_MonitorManager.GetMonitors())
        {
            if (g_Settings.IsMonitorSelected(m.id)) enabledCount++;
        }

        std::wstring targetId = m_targetMonitorId.empty() ? g_Settings.mainMonitorId : m_targetMonitorId;
        const auto* cfg = g_Settings.FindMonitorConfig(targetId);
        if (enabledCount > 1 && cfg && !cfg->customName.empty())
        {
            TitleText().Text(cfg->customName + L"\n" + std::to_wstring(m_brightness) + L"%");
        }
        else
        {
            std::wstring fmt = g_Loc.Get(L"Flyout_Brightness", L"Brightness %d%%");
            wchar_t bbuf[64];
            swprintf_s(bbuf, fmt.c_str(), m_brightness);
            TitleText().Text(bbuf);
        }

        m_isUpdatingSlider = true;
        BrightnessSlider().Value(m_brightness);
        if (!m_syncEntries.empty())
        {
            std::wstring mainName = (cfg && !cfg->customName.empty()) ? cfg->customName : g_Loc.Get(L"MainDisplay", L"Main").c_str();
            MainColumnHeader().Text(mainName + L"\n" + std::to_wstring(m_brightness) + L"%");
            for (const auto& entry : m_syncEntries)
            {
                const auto* sCfg = g_Settings.FindMonitorConfig(entry.monitorId);
                int effB = entry.isSeparate
                    ? (std::clamp)(g_MonitorManager.GetMonitorBrightnessById(entry.monitorId), 0, 100)
                    : (sCfg ? AppSettings::ComputeSyncedBrightness(m_syncBaseBrightness, *sCfg) : m_brightness);
                entry.header.Text(entry.displayName + L"\n" + std::to_wstring(effB) + L"%");
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
            g_Settings.SaveBrightnessOnly();
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

    void FlyoutWindow::ShowWindow(const POINT* pTrayPos)
    {
        CancelPendingShow();
        g_OverlayWindow.ApplyPendingBrightness();
        ApplyPendingBrightness();
        m_syncBaseBrightness = g_MonitorManager.GetTargetControlBrightness(m_targetMonitorId);
        m_brightness = (std::clamp)(m_syncBaseBrightness, 0, 100);
        m_pendingBrightness = m_syncBaseBrightness;
        m_hasPendingBrightness = false;
        RebuildSyncMonitorsPanel();
        UpdateTheme();
        ApplySliderBrushes();

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
        int width = DpiHelper::Scale(!m_syncEntries.empty() ? (137 + extraWidth) : 121, dpi);
        int height = DpiHelper::Scale(!m_syncEntries.empty() ? 590 : 576, dpi);

        bool sizeChanged = (m_lastW != width || m_lastH != height);
        m_lastW = width;
        m_lastH = height;

        // Resize while still offscreen before XAML layout so moving onscreen never shows an old-size frame
        if (m_hwnd && sizeChanged)
        {
            SetWindowPos(m_hwnd, NULL, -32000, -32000, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        SetBrightnessVisual(m_syncBaseBrightness);
        RootGrid().UpdateLayout();

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
        nii.uID = g_TrayIcon.GetId();
        if (!m_targetMonitorId.empty())
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
                    SetWindowPos(m_hwnd, HWND_TOP, targetX, targetY, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
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
            if (m_hwnd)
            {
                SetWindowPos(m_hwnd, NULL, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            TrimProcessWorkingSetAsync();
        }
    }

    void FlyoutWindow::DestroyWindow()
    {
        CancelPendingShow();
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
