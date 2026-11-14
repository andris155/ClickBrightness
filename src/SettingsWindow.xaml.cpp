#include "pch.h"
#include "SettingsWindow.xaml.h"
#if __has_include("SettingsWindow.g.cpp")
#include "SettingsWindow.g.cpp"
#endif
#if __has_include("SettingsWindow.xaml.g.hpp")
#include "SettingsWindow.xaml.g.hpp"
#endif
#if __has_include("SettingsWindow.xaml.g.cpp")
#include "SettingsWindow.xaml.g.cpp"
#endif

#include "Settings.h"
#include "HotkeyManager.h"
#include "MonitorManager.h"
#include "TrayIcon.h"
#include "OverlayWindow.h"
#include "FlyoutWindow.h"
#include "DpiHelper.h"
#include "ThemeHelper.h"
#include "LocalizationManager.h"

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <robuffer.h>
#include "../resources/resource.h"
#include <functional>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

static HHOOK s_hKeyboardHook = NULL;
static winrt::BrightnessApp::implementation::SettingsWindow* s_currentCaptureInstance = nullptr;
static UINT s_captureMods = 0;
static winrt::BrightnessApp::SettingsWindow s_settingsWindow{ nullptr };

static winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap HIconToWriteableBitmap(HICON hIcon, int width, int height)
{
    if (!hIcon) return nullptr;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hbm || !pBits)
    {
        if (hbm) DeleteObject(hbm);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return nullptr;
    }

    HBITMAP hOldBm = (HBITMAP)SelectObject(hdcMem, hbm);
    memset(pBits, 0, width * height * 4);

    DrawIconEx(hdcMem, 0, 0, hIcon, width, height, 0, NULL, DI_NORMAL);

    uint32_t* pPixels = reinterpret_cast<uint32_t*>(pBits);
    int totalPixels = width * height;
    bool hasAlpha = false;
    for (int i = 0; i < totalPixels; ++i)
    {
        if ((pPixels[i] & 0xFF000000) != 0)
        {
            hasAlpha = true;
            break;
        }
    }

    if (!hasAlpha)
    {
        for (int i = 0; i < totalPixels; ++i)
        {
            if ((pPixels[i] & 0x00FFFFFF) != 0)
            {
                pPixels[i] |= 0xFF000000;
            }
        }
    }

    winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap wb(width, height);
    auto buffer = wb.PixelBuffer();
    auto byteAccess = buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();
    byte* destPixels = nullptr;
    if (SUCCEEDED(byteAccess->Buffer(&destPixels)) && destPixels)
    {
        memcpy(destPixels, pBits, width * height * 4);
    }

    SelectObject(hdcMem, hOldBm);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    wb.Invalidate();
    return wb;
}

namespace winrt::BrightnessApp::implementation
{
    SettingsWindow::SettingsWindow()
    {
        InitializeComponent();
        bool isDark = ThemeHelper::IsAppInDarkMode(g_Settings.appTheme);
        RootGrid().RequestedTheme(isDark ? ElementTheme::Dark : ElementTheme::Light);
        SetupWindow();
        SetupBackdrop();
        UpdateLocalizedStrings();
        InitializeControls();
        PopulateLanguageList();
        LoadSettingsToUI();
        ApplyTheme();
    }

    SettingsWindow::~SettingsWindow()
    {
        StopCapture(false);
        if (m_hwnd)
        {
            RemoveWindowSubclass(m_hwnd, SubclassProc, 1);
        }
        if (m_hIconBig)
        {
            ::DestroyIcon(m_hIconBig);
            m_hIconBig = nullptr;
        }
        if (m_hIconSmall)
        {
            ::DestroyIcon(m_hIconSmall);
            m_hIconSmall = nullptr;
        }
    }

    void SettingsWindow::SetupWindow()
    {
        auto windowNative = this->try_as<::IWindowNative>();
        if (windowNative)
        {
            windowNative->get_WindowHandle(&m_hwnd);
        }

        if (m_hwnd)
        {
            SetWindowSubclass(m_hwnd, SubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

            bool isDark = ThemeHelper::IsAppInDarkMode(g_Settings.appTheme);
            BOOL darkMode = isDark ? TRUE : FALSE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

            COLORREF captionColor = isDark ? RGB(32, 32, 32) : RGB(243, 243, 243);
            DwmSetWindowAttribute(m_hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

            BOOL forceDisableTransitions = FALSE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &forceDisableTransitions, sizeof(forceDisableTransitions));

            // Load and apply application icons for window titlebar, taskbar button, preview thumbnail, and Alt-Tab
            HINSTANCE hInst = ::GetModuleHandleW(NULL);
            UINT dpi = GetDpiForWindow(m_hwnd);
            if (dpi == 0) dpi = 96;

            int iconBigSize = ::GetSystemMetricsForDpi(SM_CXICON, dpi);
            int iconSmallSize = ::GetSystemMetricsForDpi(SM_CXSMICON, dpi);
            if (iconBigSize <= 0) iconBigSize = 32;
            if (iconSmallSize <= 0) iconSmallSize = 16;

            m_hIconBig = (HICON)::LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                iconBigSize, iconBigSize, LR_DEFAULTCOLOR);
            if (!m_hIconBig)
            {
                m_hIconBig = (HICON)::LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
            }

            m_hIconSmall = (HICON)::LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                iconSmallSize, iconSmallSize, LR_DEFAULTCOLOR);
            if (!m_hIconSmall)
            {
                m_hIconSmall = (HICON)::LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
            }

            std::wstring icoFullPath;
            wchar_t exePath[MAX_PATH];
            if (::GetModuleFileNameW(NULL, exePath, MAX_PATH))
            {
                std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
                std::filesystem::path icoPath = dir / L"resources" / L"program.ico";
                if (std::filesystem::exists(icoPath))
                {
                    icoFullPath = icoPath.wstring();
                    if (!m_hIconBig)
                    {
                        m_hIconBig = (HICON)::LoadImageW(NULL, icoFullPath.c_str(), IMAGE_ICON, iconBigSize, iconBigSize, LR_LOADFROMFILE);
                    }
                    if (!m_hIconSmall)
                    {
                        m_hIconSmall = (HICON)::LoadImageW(NULL, icoFullPath.c_str(), IMAGE_ICON, iconSmallSize, iconSmallSize, LR_LOADFROMFILE);
                    }
                }
            }

            if (m_hIconBig)
            {
                ::SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(m_hIconBig));
                ::SetClassLongPtrW(m_hwnd, GCLP_HICON, reinterpret_cast<LONG_PTR>(m_hIconBig));
            }
            if (m_hIconSmall)
            {
                ::SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_hIconSmall));
                ::SetClassLongPtrW(m_hwnd, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(m_hIconSmall));
            }

            auto windowId = Microsoft::UI::GetWindowIdFromWindow(m_hwnd);
            m_appWindow = Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);

            if (m_appWindow)
            {
                m_appWindow.Title(L"ClickBrightness Settings");
                if (!icoFullPath.empty())
                {
                    m_appWindow.SetIcon(icoFullPath);
                }
                else if (m_hIconSmall || m_hIconBig)
                {
                    HICON hIconForApp = m_hIconSmall ? m_hIconSmall : m_hIconBig;
                    m_appWindow.SetIcon(winrt::Microsoft::UI::IconId{ reinterpret_cast<uint64_t>(hIconForApp) });
                }

                // Set size and center on screen
                UINT dpi = GetDpiForWindow(m_hwnd);
                if (dpi == 0) dpi = 96;

                HMONITOR hMon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfoW(hMon, &mi);
                int screenW = mi.rcWork.right - mi.rcWork.left;
                int screenH = mi.rcWork.bottom - mi.rcWork.top;

                int width = DpiHelper::Scale(860, dpi);
                int height = DpiHelper::Scale(760, dpi);
                if (height > screenH - DpiHelper::Scale(50, dpi))
                {
                    height = screenH - DpiHelper::Scale(50, dpi);
                }

                int x = mi.rcWork.left + (screenW - width) / 2;
                int y = mi.rcWork.top + (screenH - height) / 2;

                m_appWindow.MoveAndResize({ x, y, width, height });

                m_appWindow.Closing([this](auto&&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const&)
                {
                    m_isClosing = true;
                    StopCapture(false);

                    bool isDark = ThemeHelper::IsAppInDarkMode(g_Settings.appTheme);
                    if (m_hwnd)
                    {
                        BOOL darkMode = isDark ? TRUE : FALSE;
                        DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

                        COLORREF captionColor = isDark ? RGB(32, 32, 32) : RGB(243, 243, 243);
                        DwmSetWindowAttribute(m_hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
                    }

                    winrt::Windows::UI::Color solidBg = isDark
                        ? winrt::Microsoft::UI::ColorHelper::FromArgb(255, 32, 32, 32)
                        : winrt::Microsoft::UI::ColorHelper::FromArgb(255, 243, 243, 243);
                    RootGrid().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(solidBg));

                    s_settingsWindow = nullptr;
                });
            }
        }

        this->Closed([this](auto&&, auto&&)
        {
            StopCapture(false);
            s_settingsWindow = nullptr;
            TrimProcessWorkingSetAsync();
        });

        ExtendsContentIntoTitleBar(true);
        SetTitleBar(AppTitleBar());

        AppTitleBar().SizeChanged([this](auto&&, auto&&)
        {
            UpdateTitleBarDragRegions();
        });

        SettingsSearchBox().SizeChanged([this](auto&&, auto&&)
        {
            UpdateTitleBarDragRegions();
        });

        SettingsSearchBox().Loaded([this](auto&&, auto&&)
        {
            UpdateTitleBarDragRegions();
        });
    }

    void SettingsWindow::UpdateTitleBarDragRegions()
    {
        try
        {
            if (!m_appWindow) return;
            auto windowId = m_appWindow.Id();
            auto nonClientSource = winrt::Microsoft::UI::Input::InputNonClientPointerSource::GetForWindowId(windowId);
            if (!nonClientSource) return;

            auto searchBox = SettingsSearchBox();
            if (!searchBox) return;

            auto xamlRoot = searchBox.XamlRoot();
            if (!xamlRoot) return;

            double scale = xamlRoot.RasterizationScale();
            auto transform = searchBox.TransformToVisual(nullptr);
            auto bounds = transform.TransformBounds(winrt::Windows::Foundation::Rect{
                0.0f,
                0.0f,
                static_cast<float>(searchBox.ActualWidth()),
                static_cast<float>(searchBox.ActualHeight())
            });

            if (bounds.Width <= 0.0f || bounds.Height <= 0.0f)
            {
                return;
            }

            winrt::Windows::Graphics::RectInt32 passthroughRect{
                static_cast<int32_t>(std::round(bounds.X * scale)),
                static_cast<int32_t>(std::round(bounds.Y * scale)),
                static_cast<int32_t>(std::round(bounds.Width * scale)),
                static_cast<int32_t>(std::round(bounds.Height * scale))
            };

            nonClientSource.SetRegionRects(
                winrt::Microsoft::UI::Input::NonClientRegionKind::Passthrough,
                { passthroughRect }
            );
        }
        catch (...)
        {
        }
    }

    void SettingsWindow::SetupBackdrop()
    {
        try
        {
            if (g_Settings.transparencyEnabled)
            {
                SystemBackdrop(winrt::Microsoft::UI::Xaml::Media::MicaBackdrop());
            }
            else
            {
                SystemBackdrop(nullptr);
            }
        }
        catch (...)
        {
        }
    }

    void SettingsWindow::UpdateTitleBarButtons(bool isDark)
    {
        if (m_appWindow && m_appWindow.TitleBar())
        {
            auto tb = m_appWindow.TitleBar();
            tb.ButtonBackgroundColor(Microsoft::UI::Colors::Transparent());
            tb.ButtonInactiveBackgroundColor(Microsoft::UI::Colors::Transparent());
            if (isDark)
            {
                tb.ButtonForegroundColor(Microsoft::UI::Colors::White());
                tb.ButtonHoverForegroundColor(Microsoft::UI::Colors::White());
                tb.ButtonPressedForegroundColor(Microsoft::UI::Colors::White());
                tb.ButtonInactiveForegroundColor(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 160, 160, 160));
                tb.ButtonHoverBackgroundColor(winrt::Microsoft::UI::ColorHelper::FromArgb(40, 255, 255, 255));
                tb.ButtonPressedBackgroundColor(winrt::Microsoft::UI::ColorHelper::FromArgb(60, 255, 255, 255));
            }
            else
            {
                tb.ButtonForegroundColor(Microsoft::UI::Colors::Black());
                tb.ButtonHoverForegroundColor(Microsoft::UI::Colors::Black());
                tb.ButtonPressedForegroundColor(Microsoft::UI::Colors::Black());
                tb.ButtonInactiveForegroundColor(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 120, 120, 120));
                tb.ButtonHoverBackgroundColor(winrt::Microsoft::UI::ColorHelper::FromArgb(25, 0, 0, 0));
                tb.ButtonPressedBackgroundColor(winrt::Microsoft::UI::ColorHelper::FromArgb(50, 0, 0, 0));
            }
        }
    }

    void SettingsWindow::ApplyTheme()
    {
        bool isDark = ThemeHelper::IsAppInDarkMode(g_Settings.appTheme);
        RootGrid().RequestedTheme(isDark ? Microsoft::UI::Xaml::ElementTheme::Dark : Microsoft::UI::Xaml::ElementTheme::Light);
        if (g_Settings.transparencyEnabled)
        {
            RootGrid().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Transparent()));
        }
        else
        {
            winrt::Windows::UI::Color solidBg = isDark
                ? winrt::Microsoft::UI::ColorHelper::FromArgb(255, 32, 32, 32)
                : winrt::Microsoft::UI::ColorHelper::FromArgb(255, 243, 243, 243);
            RootGrid().Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(solidBg));
        }

        if (m_hwnd)
        {
            BOOL darkMode = isDark ? TRUE : FALSE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

            COLORREF captionColor = isDark ? RGB(32, 32, 32) : RGB(243, 243, 243);
            DwmSetWindowAttribute(m_hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

            SetWindowPos(m_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }

        UpdateTitleBarButtons(isDark);
    }

    void SettingsWindow::ApplyAndSave()
    {
        if (m_isUpdatingUI) return;

        g_Settings.Save();
        HWND hMain = FindWindowW(L"ClickBrightness_MessageWndClass", NULL);
        if (hMain)
        {
            g_HotkeyManager.RegisterAll(hMain);
        }
    }

    void SettingsWindow::InitializeControls()
    {
        // Cache panels initially
        m_generalPanel = GeneralPanel();
        m_shortcutsPanel = ShortcutsPanel();
        m_osdPanel = OsdPanel();
        m_trayPanel = TrayPanel();
        m_flyoutSliderPanel = FlyoutSliderPanel();
        m_aboutPanel = AboutPanel();

        // Load actual program icon for About page
        try
        {
            HICON hApp = (HICON)::LoadImageW(::GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR);
            if (!hApp)
            {
                hApp = (HICON)::LoadImageW(::GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
            }
            if (!hApp)
            {
                wchar_t exePath[MAX_PATH];
                if (GetModuleFileNameW(NULL, exePath, MAX_PATH))
                {
                    std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
                    std::filesystem::path icoPath = dir / L"resources" / L"program.ico";
                    if (std::filesystem::exists(icoPath))
                    {
                        hApp = (HICON)::LoadImageW(NULL, icoPath.c_str(), IMAGE_ICON, 48, 48, LR_LOADFROMFILE);
                    }
                }
            }
            if (hApp)
            {
                auto wbApp = HIconToWriteableBitmap(hApp, 48, 48);
                if (wbApp)
                {
                    AboutAppIconImage().Source(wbApp);
                }
                DestroyIcon(hApp);
            }
        }
        catch (...) {}

        // Start with only GeneralPanel attached to SubmenuContainer
        auto initialChildren = SubmenuContainer().Children();
        initialChildren.Clear();
        m_generalPanel.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        initialChildren.Append(m_generalPanel);

        // Navigation Pane Submenus
        NavView().SelectionChanged([this](winrt::Microsoft::UI::Xaml::Controls::NavigationView const&, winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args)
        {
            auto selectedItem = args.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem>();
            if (!selectedItem) return;
            auto tag = winrt::unbox_value_or<winrt::hstring>(selectedItem.Tag(), L"");

            auto children = SubmenuContainer().Children();
            children.Clear();

            if (tag == L"General")
            {
                m_generalPanel.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                children.Append(m_generalPanel);
            }
            else if (tag == L"Shortcuts")
            {
                m_shortcutsPanel.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                children.Append(m_shortcutsPanel);
            }
            else if (tag == L"OSD")
            {
                m_osdPanel.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                children.Append(m_osdPanel);
            }
            else if (tag == L"Tray")
            {
                m_trayPanel.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                children.Append(m_trayPanel);
            }
            else if (tag == L"FlyoutSlider")
            {
                m_flyoutSliderPanel.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                children.Append(m_flyoutSliderPanel);
            }
            else if (tag == L"About")
            {
                m_aboutPanel.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                children.Append(m_aboutPanel);
            }
        });

        // Settings Search AutoSuggestBox
        struct SettingSearchEntry
        {
            std::wstring title;
            std::wstring category;
            std::wstring keywords;
            std::wstring navTag;
            winrt::Microsoft::UI::Xaml::FrameworkElement targetControl{ nullptr };
        };

        auto toLower = [](std::wstring s) -> std::wstring
        {
            if (!s.empty())
            {
                CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
            }
            return s;
        };

        auto buildSearchEntries = [this]() -> std::vector<SettingSearchEntry>
        {
            std::wstring catGen = g_Loc.Get(L"Nav_General", L"General").c_str();
            std::wstring catSc = g_Loc.Get(L"Nav_Shortcuts", L"Shortcuts").c_str();
            std::wstring catOsd = g_Loc.Get(L"Nav_OSD", L"On-Screen Display").c_str();
            std::wstring catTray = g_Loc.Get(L"Nav_Tray", L"System Tray").c_str();
            std::wstring catFlyout = g_Loc.Get(L"Nav_FlyoutSlider", L"Flyout Slider").c_str();

            return {
                // General
                { g_Loc.Get(L"Opt_Language", L"Language").c_str(), catGen, L"language locale english translate", L"General", LanguageComboBox() },
                { g_Loc.Get(L"Opt_AppTheme", L"App Theme").c_str(), catGen, L"theme dark light windows appearance", L"General", AppThemeComboBox() },
                { g_Loc.Get(L"Opt_TransparencyEffects", L"Transparency Effects").c_str(), catGen, L"transparency mica acrylic blur solid opaque backdrop", L"General", TransparencyEffectsToggle() },
                { g_Loc.Get(L"Opt_StartWithWindows", L"Start with Windows").c_str(), catGen, L"startup boot autostart logon windows", L"General", StartWithWindowsToggle() },
                { g_Loc.Get(L"Opt_MonitorsToControl", L"Monitors to Control").c_str(), catGen, L"monitors displays screen multi-monitor all", L"General", AllMonitorsCheckBox() },
                { g_Loc.Get(L"Opt_PhysicalMonitors", L"Physical Monitors").c_str(), catGen, L"detect redetect hardware ddc/ci driver", L"General", RedetectMonitorsButton() },
                { g_Loc.Get(L"Opt_ResetDefaults", L"Reset Settings").c_str(), catGen, L"reset defaults restore factory", L"General", ResetDefaultsButton() },

                // Shortcuts
                { g_Loc.Get(L"Opt_HotkeyAdjust", L"Increase & Decrease Shortcuts").c_str(), catSc, L"enable disable stepping shortcuts hotkeys", L"Shortcuts", HotkeyAdjustToggle() },
                { g_Loc.Get(L"Opt_KeyUp", L"Increase Brightness").c_str(), catSc, L"hotkey shortcut up raise f14", L"Shortcuts", KeyUpButton() },
                { g_Loc.Get(L"Opt_KeyDown", L"Decrease Brightness").c_str(), catSc, L"hotkey shortcut down lower f13", L"Shortcuts", KeyDownButton() },
                { g_Loc.Get(L"Opt_Step", L"Brightness Step").c_str(), catSc, L"step percentage increment shortcut", L"Shortcuts", StepComboBox() },
                { g_Loc.Get(L"Opt_HotkeyAdjustShowOsd", L"Show OSD Banner").c_str(), catSc, L"show osd banner stepping hotkeys", L"Shortcuts", HotkeyAdjustShowOsdToggle() },
                { g_Loc.Get(L"Opt_HotkeyPresets", L"Preset Shortcuts").c_str(), catSc, L"enable disable preset shortcuts hotkeys", L"Shortcuts", HotkeyPresetsToggle() },
                { g_Loc.Get(L"Opt_KeyPreset1", L"Preset 1 Shortcut").c_str(), catSc, L"preset 1 shortcut hotkey f15", L"Shortcuts", KeyPreset1Button() },
                { g_Loc.Get(L"Opt_Preset1Brightness", L"Preset 1 Brightness (%)").c_str(), catSc, L"preset 1 brightness target level", L"Shortcuts", Preset1BrightnessSlider() },
                { g_Loc.Get(L"Opt_KeyPreset2", L"Preset 2 Shortcut").c_str(), catSc, L"preset 2 shortcut hotkey f16", L"Shortcuts", KeyPreset2Button() },
                { g_Loc.Get(L"Opt_Preset2Brightness", L"Preset 2 Brightness (%)").c_str(), catSc, L"preset 2 brightness target level", L"Shortcuts", Preset2BrightnessSlider() },
                { g_Loc.Get(L"Opt_HotkeyPresetsShowOsd", L"Show OSD Banner").c_str(), catSc, L"show osd banner preset shortcuts", L"Shortcuts", HotkeyPresetsShowOsdToggle() },
                { g_Loc.Get(L"Opt_ResetShortcuts", L"Reset Shortcuts").c_str(), catSc, L"reset hotkeys shortcuts", L"Shortcuts", ResetHotkeysButton() },
                { g_Loc.Get(L"Opt_DelayEnabled", L"Delayed Brightness Change").c_str(), catSc, L"delay debounce shortcut", L"Shortcuts", DelayEnabledToggle() },
                { g_Loc.Get(L"Opt_WaitTime", L"Wait Time (ms)").c_str(), catSc, L"wait time delay ms shortcut", L"Shortcuts", DelaySlider() },
                { g_Loc.Get(L"Opt_HoldSpeed", L"Hold Speed (ms)").c_str(), catSc, L"hold speed repeat interval shortcut", L"Shortcuts", RepeatSlider() },

                // On-Screen Display
                { g_Loc.Get(L"Opt_OverlayTheme", L"OSD Banner Theme").c_str(), catOsd, L"osd overlay banner theme dark light", L"OSD", OverlayThemeComboBox() },
                { g_Loc.Get(L"Opt_OverlayTimeout", L"Banner Duration (ms)").c_str(), catOsd, L"osd overlay timeout duration hide ms", L"OSD", OverlayTimeoutSlider() },
                { g_Loc.Get(L"Opt_OverlayUseWindowsAccent", L"Use Windows Accent Color").c_str(), catOsd, L"osd overlay accent windows color", L"OSD", OverlayUseWindowsAccentToggle() },
                { g_Loc.Get(L"Opt_OverlayCustomAccent", L"Custom Accent Color").c_str(), catOsd, L"osd overlay custom accent color picker", L"OSD", OverlayAccentColorPickerButton() },

                // System Tray
                { g_Loc.Get(L"Opt_TrayShowPercentage", L"Show Percentage Icon").c_str(), catTray, L"tray percentage icon show disable font number live", L"Tray", TrayShowPercentageToggle() },
                { g_Loc.Get(L"Opt_TrayFont", L"Tray Icon Font").c_str(), catTray, L"tray font family typography", L"Tray", TrayFontComboBox() },
                { g_Loc.Get(L"Opt_TrayFontSize", L"Tray Font Size (%)").c_str(), catTray, L"tray font size scale", L"Tray", TrayFontSizeSlider() },
                { g_Loc.Get(L"Opt_TrayFontSize100", L"100% Width (%)").c_str(), catTray, L"tray 100 width scale", L"Tray", TrayFontSize100Slider() },
                { g_Loc.Get(L"Opt_TrayYOffset", L"Vertical Offset (px)").c_str(), catTray, L"tray vertical offset y baseline", L"Tray", TrayYOffsetSlider() },
                { g_Loc.Get(L"Opt_TrayWheel", L"Scroll on Tray Icon").c_str(), catTray, L"tray scroll wheel mouse", L"Tray", TrayWheelToggle() },
                { g_Loc.Get(L"Opt_TrayWheelStep", L"Brightness Step").c_str(), catTray, L"tray scroll wheel step percentage", L"Tray", TrayWheelStepComboBox() },
                { g_Loc.Get(L"Opt_TrayWheelShowOsd", L"Show OSD When Scrolling").c_str(), catTray, L"tray scroll osd banner", L"Tray", TrayWheelShowOsdToggle() },
                { g_Loc.Get(L"Opt_TrayWheelDelay", L"Scrolling Hardware Delay").c_str(), catTray, L"tray scroll wheel delay debounce", L"Tray", TrayWheelDelayToggle() },
                { g_Loc.Get(L"Opt_TrayWheelWaitTime", L"Scrolling Wait Time (ms)").c_str(), catTray, L"tray scroll wheel wait time ms", L"Tray", TrayWheelDelaySlider() },
                { g_Loc.Get(L"Opt_TrayMiddleClick", L"Middle-Click Quick Action").c_str(), catTray, L"tray middle click quick action", L"Tray", TrayMiddleClickToggle() },
                { g_Loc.Get(L"Opt_TrayMiddleClickShowOsd", L"Show OSD On Middle-Click").c_str(), catTray, L"tray middle click osd banner", L"Tray", TrayMiddleClickShowOsdToggle() },
                { g_Loc.Get(L"Opt_TrayMiddleClickBrightness", L"Target Brightness (%)").c_str(), catTray, L"tray middle click brightness target", L"Tray", TrayMiddleClickSlider() },

                // Flyout Slider
                { g_Loc.Get(L"Opt_FlyoutScrollEnabled", L"Slider Scrolling").c_str(), catFlyout, L"flyout slider scroll wheel mouse enable", L"FlyoutSlider", FlyoutScrollEnabledToggle() },
                { g_Loc.Get(L"Opt_FlyoutScrollStep", L"Scrolling Brightness Step").c_str(), catFlyout, L"flyout slider scroll step percentage", L"FlyoutSlider", FlyoutScrollStepComboBox() },
                { g_Loc.Get(L"Opt_FlyoutScrollDelay", L"Slider Scrolling Delay").c_str(), catFlyout, L"flyout slider scroll delay debounce", L"FlyoutSlider", FlyoutScrollDelayToggle() },
                { g_Loc.Get(L"Opt_FlyoutScrollWaitTime", L"Scrolling Wait Time (ms)").c_str(), catFlyout, L"flyout slider scroll wait time ms", L"FlyoutSlider", FlyoutScrollDelaySlider() },
                { g_Loc.Get(L"Opt_FlyoutButtonDelay", L"Button Clicking Delay").c_str(), catFlyout, L"flyout button click delay debounce", L"FlyoutSlider", FlyoutButtonDelayToggle() },
                { g_Loc.Get(L"Opt_FlyoutButtonWaitTime", L"Button Wait Time (ms)").c_str(), catFlyout, L"flyout button wait time ms", L"FlyoutSlider", FlyoutButtonDelaySlider() },
                { g_Loc.Get(L"Opt_FlyoutUseWindowsAccent", L"Use Windows Accent Color").c_str(), catFlyout, L"flyout accent windows color", L"FlyoutSlider", FlyoutUseWindowsAccentToggle() },
                { g_Loc.Get(L"Opt_FlyoutCustomAccent", L"Custom Accent Color").c_str(), catFlyout, L"flyout custom accent color picker", L"FlyoutSlider", FlyoutAccentColorPickerButton() },
                { g_Loc.Get(L"About_Title", L"About ClickBrightness").c_str(), g_Loc.Get(L"Nav_About", L"About").c_str(), L"about version info license github open source", L"About", AboutPanel() }
            };
        };

        auto navigateToEntry = [this](const SettingSearchEntry& entry)
        {
            if (entry.navTag == L"General") NavView().SelectedItem(NavItemGeneral());
            else if (entry.navTag == L"Shortcuts") NavView().SelectedItem(NavItemShortcuts());
            else if (entry.navTag == L"OSD") NavView().SelectedItem(NavItemOSD());
            else if (entry.navTag == L"Tray") NavView().SelectedItem(NavItemTray());
            else if (entry.navTag == L"FlyoutSlider") NavView().SelectedItem(NavItemFlyoutSlider());
            else if (entry.navTag == L"About") NavView().SelectedItem(NavItemAbout());

            if (entry.targetControl)
            {
                this->DispatcherQueue().TryEnqueue([ctrl = entry.targetControl]()
                {
                    try
                    {
                        if (ctrl.Visibility() == winrt::Microsoft::UI::Xaml::Visibility::Visible)
                        {
                            ctrl.StartBringIntoView();
                            if (auto focusable = ctrl.try_as<winrt::Microsoft::UI::Xaml::Controls::Control>())
                            {
                                focusable.Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
                            }
                        }
                    }
                    catch (...) {}
                });
            }
        };

        SettingsSearchBox().GettingFocus([](winrt::Microsoft::UI::Xaml::UIElement const&, winrt::Microsoft::UI::Xaml::Input::GettingFocusEventArgs const& args)
        {
            if (args.FocusState() == winrt::Microsoft::UI::Xaml::FocusState::Programmatic)
            {
                args.Cancel(true);
            }
        });

        SettingsSearchBox().TextChanged([this, buildSearchEntries, toLower](AutoSuggestBox const& sender, AutoSuggestBoxTextChangedEventArgs const& args)
        {
            if (args.Reason() != AutoSuggestionBoxTextChangeReason::UserInput) return;

            std::wstring query = toLower(sender.Text().c_str());
            if (query.empty())
            {
                sender.ItemsSource(nullptr);
                return;
            }

            auto entries = buildSearchEntries();
            auto suggestions = winrt::single_threaded_observable_vector<winrt::hstring>();
            for (const auto& entry : entries)
            {
                std::wstring haystack = toLower(entry.title + L" " + entry.category + L" " + entry.keywords);
                if (haystack.find(query) != std::wstring::npos)
                {
                    suggestions.Append(winrt::hstring(entry.title + L"  (" + entry.category + L")"));
                }
            }

            if (suggestions.Size() == 0)
            {
                suggestions.Append(g_Loc.Get(L"Search_NoResults", L"No results found"));
            }
            sender.ItemsSource(suggestions);
        });

        SettingsSearchBox().SuggestionChosen([buildSearchEntries](AutoSuggestBox const& sender, AutoSuggestBoxSuggestionChosenEventArgs const& args)
        {
            auto chosen = winrt::unbox_value_or<winrt::hstring>(args.SelectedItem(), L"");
            if (chosen == g_Loc.Get(L"Search_NoResults", L"No results found"))
            {
                sender.Text(L"");
                return;
            }
            auto entries = buildSearchEntries();
            for (const auto& entry : entries)
            {
                std::wstring label = entry.title + L"  (" + entry.category + L")";
                if (chosen == label)
                {
                    sender.Text(winrt::hstring(entry.title));
                    break;
                }
            }
        });

        SettingsSearchBox().QuerySubmitted([buildSearchEntries, toLower, navigateToEntry](AutoSuggestBox const& sender, AutoSuggestBoxQuerySubmittedEventArgs const& args)
        {
            auto entries = buildSearchEntries();
            std::wstring chosenStr = winrt::unbox_value_or<winrt::hstring>(args.ChosenSuggestion(), L"").c_str();
            if (!chosenStr.empty())
            {
                for (const auto& entry : entries)
                {
                    std::wstring label = entry.title + L"  (" + entry.category + L")";
                    if (chosenStr == label)
                    {
                        navigateToEntry(entry);
                        sender.Text(L"");
                        sender.ItemsSource(nullptr);
                        return;
                    }
                }
            }

            std::wstring query = toLower(args.QueryText().c_str());
            if (!query.empty())
            {
                for (const auto& entry : entries)
                {
                    std::wstring haystack = toLower(entry.title + L" " + entry.category + L" " + entry.keywords);
                    if (haystack.find(query) != std::wstring::npos)
                    {
                        navigateToEntry(entry);
                        sender.Text(L"");
                        sender.ItemsSource(nullptr);
                        return;
                    }
                }
            }
        });

        // Populate standard fonts
        TrayFontComboBox().Items().Append(box_value(L"Segoe UI Variable Text"));
        TrayFontComboBox().Items().Append(box_value(L"Segoe UI Variable Small"));
        TrayFontComboBox().Items().Append(box_value(L"Segoe UI Variable Display"));
        TrayFontComboBox().Items().Append(box_value(L"Segoe UI"));
        TrayFontComboBox().Items().Append(box_value(L"Segoe UI Semibold"));
        TrayFontComboBox().Items().Append(box_value(L"Trebuchet MS"));
        TrayFontComboBox().Items().Append(box_value(L"Arial"));
        TrayFontComboBox().Items().Append(box_value(L"Consolas"));
        TrayFontComboBox().Items().Append(box_value(L"Tahoma"));
        TrayFontComboBox().Items().Append(box_value(L"Verdana"));

        // Language ComboBox
        LanguageComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int idx = LanguageComboBox().SelectedIndex();
            auto langs = g_Loc.GetAvailableLanguages();
            if (idx >= 0 && idx < (int)langs.size())
            {
                g_Settings.language = langs[idx].code;
                g_Loc.SetLanguage(g_Settings.language);

                UpdateLocalizedStrings();
                PopulateLanguageList();
                PopulateMonitorsInfo();
                LoadSettingsToUI();

                g_TrayIcon.OnLanguageChanged();
                g_FlyoutWindow.OnThemeChanged();
                ApplyAndSave();
            }
        });

        // All Monitors CheckBox
        AllMonitorsCheckBox().Click([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool isAll = AllMonitorsCheckBox().IsChecked().GetBoolean();
            if (isAll)
            {
                g_Settings.selectedMonitors = L"all";
            }
            else
            {
                if (!m_cachedMonitorIds.empty())
                {
                    g_Settings.selectedMonitors = m_cachedMonitorIds[0];
                }
            }
            UpdateMonitorsSelectionUI();
            ApplyAndSave();
        });

        // App Theme ComboBox
        AppThemeComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int idx = AppThemeComboBox().SelectedIndex();
            if (idx >= 0 && idx <= 2)
            {
                g_Settings.appTheme = idx;
                ApplyTheme();
                ApplyAndSave();
            }
        });

        // Transparency Effects Toggle
        TransparencyEffectsToggle().Toggled([this](auto&&, auto&&)
        {
            bool on = TransparencyEffectsToggle().IsOn();
            TransparencyEffectsStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            if (m_isUpdatingUI) return;
            g_Settings.transparencyEnabled = on;
            SetupBackdrop();
            ApplyTheme();
            ApplyAndSave();
            g_OverlayWindow.OnThemeChanged();
            g_FlyoutWindow.OnThemeChanged();
        });

        // Hotkey Adjust (Up/Down) Toggle
        HotkeyAdjustToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = HotkeyAdjustToggle().IsOn();
            g_Settings.hotkeyAdjustEnabled = on;
            HotkeyAdjustStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            KeyUpCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            KeyDownCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            StepCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            HotkeyAdjustShowOsdCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ApplyAndSave();
        });

        // Hotkey Adjust Show OSD Toggle
        HotkeyAdjustShowOsdToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = HotkeyAdjustShowOsdToggle().IsOn();
            g_Settings.hotkeyAdjustShowOsd = on;
            HotkeyAdjustShowOsdStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            ApplyAndSave();
        });

        // Key Up button capture
        KeyUpButton().Click([this](auto&&, auto&&)
        {
            StartCapture(1);
        });

        // Key Down button capture
        KeyDownButton().Click([this](auto&&, auto&&)
        {
            StartCapture(2);
        });

        // Hotkey Presets Toggle
        HotkeyPresetsToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = HotkeyPresetsToggle().IsOn();
            g_Settings.hotkeyPresetsEnabled = on;
            HotkeyPresetsStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            Preset1KeyCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            Preset1BrightnessCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            Preset2KeyCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            Preset2BrightnessCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            HotkeyPresetsShowOsdCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ApplyAndSave();
        });

        // Hotkey Presets Show OSD Toggle
        HotkeyPresetsShowOsdToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = HotkeyPresetsShowOsdToggle().IsOn();
            g_Settings.hotkeyPresetsShowOsd = on;
            HotkeyPresetsShowOsdStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            ApplyAndSave();
        });

        // Preset 1 button capture
        KeyPreset1Button().Click([this](auto&&, auto&&)
        {
            StartCapture(3);
        });

        // Preset 1 Brightness Slider & NumberBox sync
        Preset1BrightnessSlider().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(std::round(e.NewValue()));
            val = (std::clamp)(val, 0, 100);
            g_Settings.hotkeyPreset1Brightness = val;
            m_isUpdatingUI = true;
            Preset1BrightnessNumberBox().Value(val);
            m_isUpdatingUI = false;
            ApplyAndSave();
        });

        Preset1BrightnessNumberBox().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            double v = e.NewValue();
            if (std::isnan(v)) return;
            int val = static_cast<int>(std::round(v));
            val = (std::clamp)(val, 0, 100);
            g_Settings.hotkeyPreset1Brightness = val;
            m_isUpdatingUI = true;
            Preset1BrightnessSlider().Value(val);
            m_isUpdatingUI = false;
            ApplyAndSave();
        });

        // Preset 2 button capture
        KeyPreset2Button().Click([this](auto&&, auto&&)
        {
            StartCapture(4);
        });

        // Preset 2 Brightness Slider & NumberBox sync
        Preset2BrightnessSlider().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(std::round(e.NewValue()));
            val = (std::clamp)(val, 0, 100);
            g_Settings.hotkeyPreset2Brightness = val;
            m_isUpdatingUI = true;
            Preset2BrightnessNumberBox().Value(val);
            m_isUpdatingUI = false;
            ApplyAndSave();
        });

        Preset2BrightnessNumberBox().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            double v = e.NewValue();
            if (std::isnan(v)) return;
            int val = static_cast<int>(std::round(v));
            val = (std::clamp)(val, 0, 100);
            g_Settings.hotkeyPreset2Brightness = val;
            m_isUpdatingUI = true;
            Preset2BrightnessSlider().Value(val);
            m_isUpdatingUI = false;
            ApplyAndSave();
        });

        // Step ComboBox
        StepComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int stepIdx = StepComboBox().SelectedIndex();
            int steps[] = { 1, 2, 5, 10, 15, 20 };
            if (stepIdx >= 0 && stepIdx < 6)
            {
                g_Settings.hotkeyStep = steps[stepIdx];
                ApplyAndSave();
            }
        });

        // Reset Hotkeys Button
        ResetHotkeysButton().Click([this](auto&&, auto&&)
        {
            StopCapture(false);
            m_curUpKey = 0x7D; // VK_F14
            m_curUpMods = 0;
            m_curDownKey = 0x7C; // VK_F13
            m_curDownMods = 0;
            m_curPreset1Key = 0x7E; // VK_F15
            m_curPreset1Mods = 0;
            m_curPreset2Key = 0x7F; // VK_F16
            m_curPreset2Mods = 0;
            g_Settings.hotkeyUpKey = m_curUpKey;
            g_Settings.hotkeyUpMods = m_curUpMods;
            g_Settings.hotkeyDownKey = m_curDownKey;
            g_Settings.hotkeyDownMods = m_curDownMods;
            g_Settings.hotkeyPreset1Key = m_curPreset1Key;
            g_Settings.hotkeyPreset1Mods = m_curPreset1Mods;
            g_Settings.hotkeyPreset2Key = m_curPreset2Key;
            g_Settings.hotkeyPreset2Mods = m_curPreset2Mods;
            g_Settings.hotkeyAdjustShowOsd = true;
            g_Settings.hotkeyPresetsShowOsd = true;
            HotkeyAdjustShowOsdToggle().IsOn(true);
            HotkeyAdjustShowOsdStatusText().Text(g_Loc.Get(L"Common_On", L"On"));
            HotkeyPresetsShowOsdToggle().IsOn(true);
            HotkeyPresetsShowOsdStatusText().Text(g_Loc.Get(L"Common_On", L"On"));
            KeyUpButton().Content(box_value(HotkeyManager::FormatHotkey(m_curUpMods, m_curUpKey)));
            KeyDownButton().Content(box_value(HotkeyManager::FormatHotkey(m_curDownMods, m_curDownKey)));
            KeyPreset1Button().Content(box_value(HotkeyManager::FormatHotkey(m_curPreset1Mods, m_curPreset1Key)));
            KeyPreset2Button().Content(box_value(HotkeyManager::FormatHotkey(m_curPreset2Mods, m_curPreset2Key)));
            ApplyAndSave();
        });

        // Delay Toggle
        DelayEnabledToggle().Toggled([this](auto&&, auto&&)
        {
            bool on = DelayEnabledToggle().IsOn();
            DelayEnabledStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            if (m_isUpdatingUI) return;
            g_Settings.hotkeyDelayEnabled = on;
            DelayWaitTimeCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            HoldSpeedCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ApplyAndSave();
        });

        // Delay Slider <-> NumberBox synchronization
        DelaySlider().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(DelaySlider().Value());
            m_isUpdatingUI = true;
            DelayNumberBox().Value(val);
            m_isUpdatingUI = false;
            g_Settings.hotkeyDelayMs = val;
            ApplyAndSave();
        });

        DelayNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(DelayNumberBox().Value());
            m_isUpdatingUI = true;
            DelaySlider().Value(val);
            m_isUpdatingUI = false;
            g_Settings.hotkeyDelayMs = val;
            ApplyAndSave();
        });

        // Repeat Slider <-> NumberBox synchronization
        RepeatSlider().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(RepeatSlider().Value());
            m_isUpdatingUI = true;
            RepeatNumberBox().Value(val);
            m_isUpdatingUI = false;
            g_Settings.hotkeyRepeatIntervalMs = val;
            ApplyAndSave();
        });

        RepeatNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(RepeatNumberBox().Value());
            m_isUpdatingUI = true;
            RepeatSlider().Value(val);
            m_isUpdatingUI = false;
            g_Settings.hotkeyRepeatIntervalMs = val;
            ApplyAndSave();
        });

        // Overlay Theme ComboBox
        OverlayThemeComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int oTheme = OverlayThemeComboBox().SelectedIndex();
            if (oTheme >= 0 && oTheme <= 2)
            {
                g_Settings.overlayTheme = oTheme;
                g_OverlayWindow.OnThemeChanged();
                ApplyAndSave();
            }
        });

        // Overlay Timeout Slider <-> NumberBox synchronization
        OverlayTimeoutSlider().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(OverlayTimeoutSlider().Value());
            m_isUpdatingUI = true;
            OverlayTimeoutNumberBox().Value(val);
            m_isUpdatingUI = false;
            g_Settings.overlayTimeoutMs = val;
            ApplyAndSave();
        });

        OverlayTimeoutNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(OverlayTimeoutNumberBox().Value());
            m_isUpdatingUI = true;
            OverlayTimeoutSlider().Value(val);
            m_isUpdatingUI = false;
            g_Settings.overlayTimeoutMs = val;
            ApplyAndSave();
        });

        // Overlay Use Windows Accent Color Toggle
        auto updateOverlayAccentUI = [this](bool useWinAccent)
        {
            OverlayAccentColorCard().Visibility(useWinAccent ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);
        };

        OverlayUseWindowsAccentToggle().Toggled([this, updateOverlayAccentUI](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool enabled = OverlayUseWindowsAccentToggle().IsOn();
            OverlayUseWindowsAccentStatusText().Text(enabled ? L"On" : L"Off");
            g_Settings.overlayUseWindowsAccent = enabled;
            updateOverlayAccentUI(enabled);
            ApplyAndSave();
            g_OverlayWindow.OnThemeChanged();
        });

        // Tray Show Percentage Toggle
        TrayShowPercentageToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = TrayShowPercentageToggle().IsOn();
            g_Settings.trayShowPercentage = on;
            TrayShowPercentageStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            TrayTypographyPanel().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            g_TrayIcon.OnThemeChanged();
            ApplyAndSave();
        });

        // Tray Font ComboBox
        TrayFontComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            if (TrayFontComboBox().SelectedItem())
            {
                auto selectedFont = unbox_value<hstring>(TrayFontComboBox().SelectedItem());
                g_Settings.trayFont = selectedFont.c_str();
                g_TrayIcon.OnThemeChanged();
                ApplyAndSave();
            }
        });

        // Tray Font Size Slider <-> NumberBox synchronization
        TrayFontSizeSlider().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(TrayFontSizeSlider().Value());
            m_isUpdatingUI = true;
            TrayFontSizeNumberBox().Value(val);
            m_isUpdatingUI = false;
            g_Settings.trayFontSize = val;
            g_TrayIcon.OnThemeChanged();
            ApplyAndSave();
        });

        TrayFontSizeNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(TrayFontSizeNumberBox().Value());
            m_isUpdatingUI = true;
            TrayFontSizeSlider().Value(val);
            m_isUpdatingUI = false;
            g_Settings.trayFontSize = val;
            g_TrayIcon.OnThemeChanged();
            ApplyAndSave();
        });

        // Tray Font Size 100 Slider <-> NumberBox synchronization
        TrayFontSize100Slider().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(TrayFontSize100Slider().Value());
            m_isUpdatingUI = true;
            TrayFontSize100NumberBox().Value(val);
            m_isUpdatingUI = false;
            g_Settings.trayFontSize100 = val;
            g_TrayIcon.OnThemeChanged();
            ApplyAndSave();
        });

        TrayFontSize100NumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(TrayFontSize100NumberBox().Value());
            m_isUpdatingUI = true;
            TrayFontSize100Slider().Value(val);
            m_isUpdatingUI = false;
            g_Settings.trayFontSize100 = val;
            g_TrayIcon.OnThemeChanged();
            ApplyAndSave();
        });

        // Tray Y Offset Slider & TextBox sync
        TrayYOffsetSlider().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            float val = static_cast<float>(std::round(e.NewValue() * 10.0) / 10.0);
            val = (std::clamp)(val, -10.0f, 10.0f);
            g_Settings.trayYOffset = val;
            m_isUpdatingUI = true;
            TrayYOffsetTextBox().Text(AppSettings::FormatYOffset(val, g_Settings.trayYOffsetUseComma));
            m_isUpdatingUI = false;
            g_TrayIcon.OnThemeChanged();
            ApplyAndSave();
        });

        TrayYOffsetTextBox().TextChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            std::wstring str = TrayYOffsetTextBox().Text().c_str();
            bool useComma = g_Settings.trayYOffsetUseComma;
            float val = AppSettings::ParseYOffset(str, &useComma);
            g_Settings.trayYOffset = val;
            g_Settings.trayYOffsetUseComma = useComma;
            m_isUpdatingUI = true;
            TrayYOffsetSlider().Value(val);
            m_isUpdatingUI = false;
            g_TrayIcon.OnThemeChanged();
            ApplyAndSave();
        });

        // Tray Wheel Toggle
        TrayWheelToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = TrayWheelToggle().IsOn();
            g_Settings.trayWheelEnabled = on;
            TrayWheelStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            TrayWheelStepCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            TrayWheelShowOsdCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            TrayWheelDelayCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            TrayWheelWaitTimeCard().Visibility((on && g_Settings.trayWheelDelayEnabled) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            g_TrayIcon.UpdateWheelHook();
            ApplyAndSave();
        });

        // Tray Wheel Brightness Step ComboBox
        TrayWheelStepComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int stepIdx = TrayWheelStepComboBox().SelectedIndex();
            int steps[] = { 1, 2, 5, 10, 15, 20 };
            if (stepIdx >= 0 && stepIdx < 6)
            {
                g_Settings.trayWheelStep = steps[stepIdx];
                ApplyAndSave();
            }
        });

        // Tray Wheel Show OSD Toggle
        TrayWheelShowOsdToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = TrayWheelShowOsdToggle().IsOn();
            g_Settings.trayWheelShowOsd = on;
            TrayWheelShowOsdStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            ApplyAndSave();
        });

        // Tray Wheel Delay Toggle
        TrayWheelDelayToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = TrayWheelDelayToggle().IsOn();
            g_Settings.trayWheelDelayEnabled = on;
            TrayWheelDelayStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            TrayWheelWaitTimeCard().Visibility((g_Settings.trayWheelEnabled && on) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ApplyAndSave();
        });

        // Tray Wheel Delay Slider & NumberBox sync
        TrayWheelDelaySlider().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(std::round(e.NewValue()));
            val = (std::clamp)(val, 100, 3000);
            g_Settings.trayWheelDelayMs = val;
            m_isUpdatingUI = true;
            TrayWheelDelayNumberBox().Value(val);
            m_isUpdatingUI = false;
            ApplyAndSave();
        });

        TrayWheelDelayNumberBox().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            double v = e.NewValue();
            if (std::isnan(v)) return;
            int val = static_cast<int>(std::round(v));
            val = (std::clamp)(val, 100, 3000);
            g_Settings.trayWheelDelayMs = val;
            m_isUpdatingUI = true;
            TrayWheelDelaySlider().Value(val);
            m_isUpdatingUI = false;
            ApplyAndSave();
        });

        // Tray Middle-Click Toggle
        TrayMiddleClickToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = TrayMiddleClickToggle().IsOn();
            g_Settings.trayMiddleClickEnabled = on;
            TrayMiddleClickStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            TrayMiddleClickShowOsdCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            TrayMiddleClickCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ApplyAndSave();
        });

        // Tray Middle-Click Show OSD Toggle
        TrayMiddleClickShowOsdToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = TrayMiddleClickShowOsdToggle().IsOn();
            g_Settings.trayMiddleClickShowOsd = on;
            TrayMiddleClickShowOsdStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            ApplyAndSave();
        });

        // Tray Middle-Click Slider & NumberBox sync
        TrayMiddleClickSlider().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(std::round(e.NewValue()));
            val = (std::clamp)(val, 0, 100);
            g_Settings.trayMiddleClickBrightness = val;
            m_isUpdatingUI = true;
            TrayMiddleClickNumberBox().Value(val);
            m_isUpdatingUI = false;
            ApplyAndSave();
        });

        TrayMiddleClickNumberBox().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            double v = e.NewValue();
            if (std::isnan(v)) return;
            int val = static_cast<int>(std::round(v));
            val = (std::clamp)(val, 0, 100);
            g_Settings.trayMiddleClickBrightness = val;
            m_isUpdatingUI = true;
            TrayMiddleClickSlider().Value(val);
            m_isUpdatingUI = false;
            ApplyAndSave();
        });

        // Start With Windows Toggle
        StartWithWindowsToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            if (!AppSettings::IsInstalledInProgramFiles())
            {
                m_isUpdatingUI = true;
                StartWithWindowsToggle().IsOn(false);
                StartWithWindowsToggle().IsEnabled(false);
                StartWithWindowsStatusText().Text(g_Loc.Get(L"Common_Off", L"Off"));
                m_isUpdatingUI = false;
                return;
            }
            bool enabled = StartWithWindowsToggle().IsOn();
            StartWithWindowsStatusText().Text(enabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            AppSettings::SetStartWithWindows(enabled);
            g_Settings.startWithWindows = enabled;
            ApplyAndSave();
        });

        // Flyout Use Windows Accent Color Toggle
        auto updateFlyoutAccentUI = [this](bool useWinAccent)
        {
            FlyoutAccentColorCard().Visibility(useWinAccent ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);
        };

        // Flyout Scroll Enabled Toggle
        FlyoutScrollEnabledToggle().Toggled([this](auto&&, auto&&)
        {
            bool on = FlyoutScrollEnabledToggle().IsOn();
            FlyoutScrollEnabledStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            if (m_isUpdatingUI) return;
            g_Settings.flyoutScrollEnabled = on;
            FlyoutScrollStepCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            FlyoutScrollDelayCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            FlyoutScrollWaitTimeCard().Visibility((on && g_Settings.flyoutScrollDelayEnabled) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ApplyAndSave();
        });

        // Flyout Scroll Brightness Step ComboBox
        FlyoutScrollStepComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int stepIdx = FlyoutScrollStepComboBox().SelectedIndex();
            int steps[] = { 1, 2, 5, 10, 15, 20 };
            if (stepIdx >= 0 && stepIdx < 6)
            {
                g_Settings.flyoutScrollStep = steps[stepIdx];
                ApplyAndSave();
            }
        });

        // Flyout Scroll Delay Toggle
        FlyoutScrollDelayToggle().Toggled([this](auto&&, auto&&)
        {
            bool on = FlyoutScrollDelayToggle().IsOn();
            FlyoutScrollDelayStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            if (m_isUpdatingUI) return;
            g_Settings.flyoutScrollDelayEnabled = on;
            FlyoutScrollWaitTimeCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ApplyAndSave();
        });

        // Flyout Scroll Delay Slider <-> NumberBox synchronization
        FlyoutScrollDelaySlider().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(FlyoutScrollDelaySlider().Value());
            m_isUpdatingUI = true;
            FlyoutScrollDelayNumberBox().Value(val);
            m_isUpdatingUI = false;
            g_Settings.flyoutScrollDelayMs = val;
            ApplyAndSave();
        });

        FlyoutScrollDelayNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(FlyoutScrollDelayNumberBox().Value());
            m_isUpdatingUI = true;
            FlyoutScrollDelaySlider().Value(val);
            m_isUpdatingUI = false;
            g_Settings.flyoutScrollDelayMs = val;
            ApplyAndSave();
        });

        // Flyout Button Delay Toggle
        FlyoutButtonDelayToggle().Toggled([this](auto&&, auto&&)
        {
            bool on = FlyoutButtonDelayToggle().IsOn();
            FlyoutButtonDelayStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            if (m_isUpdatingUI) return;
            g_Settings.flyoutButtonDelayEnabled = on;
            FlyoutButtonWaitTimeCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ApplyAndSave();
        });

        // Flyout Button Delay Slider <-> NumberBox synchronization
        FlyoutButtonDelaySlider().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(FlyoutButtonDelaySlider().Value());
            m_isUpdatingUI = true;
            FlyoutButtonDelayNumberBox().Value(val);
            m_isUpdatingUI = false;
            g_Settings.flyoutButtonDelayMs = val;
            ApplyAndSave();
        });

        FlyoutButtonDelayNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(FlyoutButtonDelayNumberBox().Value());
            m_isUpdatingUI = true;
            FlyoutButtonDelaySlider().Value(val);
            m_isUpdatingUI = false;
            g_Settings.flyoutButtonDelayMs = val;
            ApplyAndSave();
        });

        FlyoutUseWindowsAccentToggle().Toggled([this, updateFlyoutAccentUI](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool enabled = FlyoutUseWindowsAccentToggle().IsOn();
            FlyoutUseWindowsAccentStatusText().Text(enabled ? L"On" : L"Off");
            g_Settings.flyoutUseWindowsAccent = enabled;
            updateFlyoutAccentUI(enabled);
            ApplyAndSave();
            g_FlyoutWindow.OnThemeChanged();
        });

        // Color Control Helper
        auto setupColorControl = [this](
            winrt::Microsoft::UI::Xaml::Controls::ColorPicker const& picker,
            winrt::Microsoft::UI::Xaml::Controls::TextBox const& textBox,
            winrt::Microsoft::UI::Xaml::Controls::Border const& preview,
            COLORREF& settingRef,
            std::function<void()> onColorChange = nullptr)
        {
            picker.ColorChanged([this, &settingRef, textBox, preview, onColorChange](auto&&, winrt::Microsoft::UI::Xaml::Controls::ColorChangedEventArgs const& args)
            {
                if (m_isUpdatingUI) return;
                auto c = args.NewColor();
                COLORREF col = RGB(c.R, c.G, c.B);
                settingRef = col;
                m_isUpdatingUI = true;
                textBox.Text(AppSettings::FormatColor(col));
                preview.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(c));
                m_isUpdatingUI = false;
                ApplyAndSave();
                if (onColorChange) onColorChange();
                else g_FlyoutWindow.OnThemeChanged();
            });

            textBox.TextChanged([this, &settingRef, picker, preview, textBox, onColorChange](auto&&, auto&&)
            {
                if (m_isUpdatingUI) return;
                std::wstring str = textBox.Text().c_str();
                COLORREF parsed = AppSettings::ParseColor(str, settingRef);
                if (parsed != settingRef || str.length() >= 7)
                {
                    settingRef = parsed;
                    auto c = winrt::Microsoft::UI::ColorHelper::FromArgb(255, GetRValue(parsed), GetGValue(parsed), GetBValue(parsed));
                    m_isUpdatingUI = true;
                    picker.Color(c);
                    preview.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(c));
                    m_isUpdatingUI = false;
                    ApplyAndSave();
                    if (onColorChange) onColorChange();
                    else g_FlyoutWindow.OnThemeChanged();
                }
            });
        };

        // Overlay Accent Color
        setupColorControl(OverlayAccentColorPicker(), OverlayAccentColorTextBox(), OverlayAccentColorPreview(), g_Settings.overlayAccentColor, []() {
            g_OverlayWindow.OnThemeChanged();
        });

        // Flyout Accent Color
        setupColorControl(FlyoutAccentColorPicker(), FlyoutAccentColorTextBox(), FlyoutAccentColorPreview(), g_Settings.flyoutAccentColor, []() {
            g_FlyoutWindow.OnThemeChanged();
        });

        // Re-detect monitors
        RedetectMonitorsButton().Click([this](auto&&, auto&&)
        {
            g_MonitorManager.Reinitialize();
            PopulateMonitorsInfo();
        });

        // Confirm Reset All Defaults Button (inside ResetDefaultsButton Flyout)
        ConfirmResetDefaultsButton().Click([this](auto&&, auto&&)
        {
            ResetDefaultsFlyout().Hide();
            StopCapture(false);

            g_Settings.appTheme = 0;
            g_Settings.transparencyEnabled = true;
            g_Settings.hotkeyAdjustEnabled = false;
            g_Settings.hotkeyUpKey = 0x7D; // VK_F14
            g_Settings.hotkeyUpMods = 0;
            g_Settings.hotkeyDownKey = 0x7C; // VK_F13
            g_Settings.hotkeyDownMods = 0;
            g_Settings.hotkeyPresetsEnabled = false;
            g_Settings.hotkeyPreset1Key = 0x7E; // VK_F15
            g_Settings.hotkeyPreset1Mods = 0;
            g_Settings.hotkeyPreset1Brightness = 30;
            g_Settings.hotkeyPreset2Key = 0x7F; // VK_F16
            g_Settings.hotkeyPreset2Mods = 0;
            g_Settings.hotkeyPreset2Brightness = 80;
            g_Settings.hotkeyStep = 5;
            g_Settings.hotkeyDelayEnabled = true;
            g_Settings.hotkeyDelayMs = 1000;
            g_Settings.hotkeyRepeatIntervalMs = 90;
            g_Settings.overlayTheme = 0;
            g_Settings.overlayTimeoutMs = 2000;
            g_Settings.overlayUseWindowsAccent = true;
            g_Settings.overlayAccentColor = RGB(0, 120, 212);
            g_Settings.flyoutUseWindowsAccent = true;
            g_Settings.flyoutAccentColor = RGB(0, 120, 212);
            g_Settings.flyoutScrollEnabled = true;
            g_Settings.flyoutScrollStep = 5;
            g_Settings.flyoutScrollDelayEnabled = true;
            g_Settings.flyoutScrollDelayMs = 1000;
            g_Settings.flyoutButtonDelayEnabled = false;
            g_Settings.flyoutButtonDelayMs = 500;
            g_Settings.trayShowPercentage = true;
            g_Settings.trayFont = L"Segoe UI Variable Text";
            g_Settings.trayFontSize = 102;
            g_Settings.trayFontSize100 = 63;
            g_Settings.trayYOffset = -0.5f;
            g_Settings.trayYOffsetUseComma = false;
            g_Settings.trayWheelEnabled = false;
            g_Settings.trayWheelStep = 5;
            g_Settings.trayWheelShowOsd = true;
            g_Settings.trayWheelDelayEnabled = true;
            g_Settings.trayWheelDelayMs = 1000;
            g_Settings.trayMiddleClickEnabled = false;
            g_Settings.trayMiddleClickShowOsd = true;
            g_Settings.trayMiddleClickBrightness = 50;
            g_TrayIcon.UpdateWheelHook();
            g_Settings.language = L"default";
            g_Settings.selectedMonitors = L"all";
            g_Loc.SetLanguage(L"default");
            UpdateLocalizedStrings();
            PopulateLanguageList();
            PopulateMonitorsSelectionList();

            LoadSettingsToUI();
            SetupBackdrop();
            ApplyTheme();
            ApplyAndSave();

            g_OverlayWindow.OnThemeChanged();
            g_FlyoutWindow.OnThemeChanged();
            g_TrayIcon.OnThemeChanged();
            g_TrayIcon.OnLanguageChanged();
        });
    }

    void SettingsWindow::ShowWindow()
    {
        LoadSettingsToUI();
        ApplyTheme();
        PopulateMonitorsInfo();

        if (m_hwnd && IsIconic(m_hwnd))
        {
            ::ShowWindow(m_hwnd, SW_RESTORE);
        }
        this->Activate();
        if (m_hwnd)
        {
            SetForegroundWindow(m_hwnd);
        }
        UpdateTitleBarDragRegions();
    }

    void SettingsWindow::HideWindow()
    {
        StopCapture(false);
        if (m_hwnd)
        {
            ::ShowWindow(m_hwnd, SW_HIDE);
        }
    }

    void SettingsWindow::PopulateMonitorsInfo()
    {
        bool driverOk = g_MonitorManager.IsDriverAvailable();
        if (driverOk)
        {
            HardwareStatusInfoBar().IsOpen(false);
            MonitorsSummaryText().Text(g_Loc.Get(L"Status_Summary_Ready", L"DDC/CI driver active. Hardware brightness control enabled."));
        }
        else
        {
            HardwareStatusInfoBar().IsOpen(true);
            HardwareStatusInfoBar().Severity(winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
            HardwareStatusInfoBar().Title(g_Loc.Get(L"Status_Offline_Title", L"DDC/CI Driver Offline"));
            HardwareStatusInfoBar().Message(g_Loc.Get(L"Status_Offline_Msg", L"No DDC/CI compatible monitor answered, or driver is still initializing."));
            MonitorsSummaryText().Text(g_Loc.Get(L"Status_Summary_Offline", L"DDC/CI communication unavailable. Using fallback state."));
        }
        PopulateMonitorsSelectionList();
    }

    void SettingsWindow::PopulateLanguageList()
    {
        m_isUpdatingUI = true;

        auto langs = g_Loc.GetAvailableLanguages();
        auto items = winrt::single_threaded_observable_vector<winrt::hstring>();
        int selectedIndex = 0;
        for (size_t i = 0; i < langs.size(); ++i)
        {
            std::wstring display = langs[i].displayName;
            if (langs[i].code == L"default")
            {
                display = g_Loc.Get(L"Lang_WindowsDefault", L"Follow Windows");
                std::wstring winLang = g_Loc.GetWindowsDisplayLanguage();
                if (!winLang.empty())
                {
                    display += L" (" + winLang + L")";
                }
            }
            items.Append(winrt::hstring(display));
            if (_wcsicmp(langs[i].code.c_str(), g_Settings.language.c_str()) == 0)
            {
                selectedIndex = static_cast<int>(i);
            }
        }

        LanguageComboBox().ItemsSource(items);
        LanguageComboBox().SelectedIndex(selectedIndex);
        m_isUpdatingUI = false;
    }

    void SettingsWindow::PopulateMonitorsSelectionList()
    {
        auto monitors = g_MonitorManager.GetMonitors();
        m_cachedMonitorIds.clear();
        MonitorsListStackPanel().Children().Clear();

        if (monitors.empty())
        {
            auto tb = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
            tb.Text(g_Loc.Get(L"Monitors_NoneDetected", L"No DDC/CI monitors detected yet."));
            tb.FontSize(12);
            tb.Foreground(Application::Current().Resources().Lookup(box_value(L"TextFillColorSecondaryBrush")).as<winrt::Microsoft::UI::Xaml::Media::Brush>());
            MonitorsListStackPanel().Children().Append(tb);
            UpdateMonitorsSelectionUI();
            return;
        }

        // If current saved selection doesn't match ANY of the detected monitors
        // (which happens after GPU driver update re-enumerates devices),
        // automatically fallback to "all" so the user never has 0 monitors selected!
        if (!g_Settings.selectedMonitors.empty() && _wcsicmp(g_Settings.selectedMonitors.c_str(), L"all") != 0)
        {
            bool anyFound = false;
            for (const auto& mon : monitors)
            {
                if (g_Settings.IsMonitorSelected(mon.id))
                {
                    anyFound = true;
                    break;
                }
            }
            if (!anyFound)
            {
                g_Settings.selectedMonitors = L"all";
                g_Settings.Save();
            }
        }

        int displayIndex = 1;
        for (const auto& mon : monitors)
        {
            m_cachedMonitorIds.push_back(mon.id);

            // Display numbering: sequential 1, 2, 3... matching Windows Display Settings
            std::wstring dispNum = std::to_wstring(displayIndex);

            std::wstring monName = !mon.friendlyName.empty() ? mon.friendlyName : mon.description;
            std::wstring fmt = g_Loc.Get(L"Monitors_DisplayFormat", L"Display %s: %s");
            wchar_t buf[256];
            swprintf_s(buf, fmt.c_str(), dispNum.c_str(), monName.c_str());
            std::wstring label = buf;

            if (mon.isPrimary)
            {
                label += L" (" + g_Loc.Get(L"PrimaryBadge", L"Primary") + L")";
            }

            auto cb = winrt::Microsoft::UI::Xaml::Controls::CheckBox();
            cb.Content(box_value(label));
            cb.Tag(box_value(mon.id));
            cb.FontSize(13);

            displayIndex++;

            cb.Click([this, id = mon.id](auto&& sender, auto&&)
            {
                if (m_isUpdatingUI) return;
                auto clickedBox = sender.as<winrt::Microsoft::UI::Xaml::Controls::CheckBox>();
                bool isChecked = clickedBox.IsChecked().GetBoolean();
                g_Settings.SetMonitorSelected(id, isChecked, m_cachedMonitorIds);
                UpdateMonitorsSelectionUI();
                ApplyAndSave();
            });

            MonitorsListStackPanel().Children().Append(cb);
        }

        UpdateMonitorsSelectionUI();
    }

    void SettingsWindow::UpdateMonitorsSelectionUI()
    {
        m_isUpdatingUI = true;

        bool isAll = (g_Settings.selectedMonitors.empty() || _wcsicmp(g_Settings.selectedMonitors.c_str(), L"all") == 0);
        AllMonitorsCheckBox().IsChecked(isAll);

        bool allChildrenChecked = !m_cachedMonitorIds.empty();

        for (auto child : MonitorsListStackPanel().Children())
        {
            if (auto cb = child.try_as<winrt::Microsoft::UI::Xaml::Controls::CheckBox>())
            {
                std::wstring id = unbox_value<hstring>(cb.Tag()).c_str();
                bool checked = isAll || g_Settings.IsMonitorSelected(id);
                cb.IsChecked(checked);
                if (!checked)
                {
                    allChildrenChecked = false;
                }
            }
        }

        if (allChildrenChecked && !m_cachedMonitorIds.empty())
        {
            AllMonitorsCheckBox().IsChecked(true);
        }

        m_isUpdatingUI = false;
    }

    void SettingsWindow::UpdateLocalizedStrings()
    {
        bool prevUpdating = m_isUpdatingUI;
        m_isUpdatingUI = true;

        if (m_appWindow)
        {
            m_appWindow.Title(g_Loc.Get(L"AppTitle", L"ClickBrightness Settings"));
        }
        AppTitleBarText().Text(g_Loc.Get(L"AppTitle", L"ClickBrightness Settings"));

        // Navigation
        NavItemGeneral().Content(box_value(g_Loc.Get(L"Nav_General", L"General")));
        NavItemShortcuts().Content(box_value(g_Loc.Get(L"Nav_Shortcuts", L"Shortcuts")));
        NavItemOSD().Content(box_value(g_Loc.Get(L"Nav_OSD", L"On-Screen Display")));
        NavItemTray().Content(box_value(g_Loc.Get(L"Nav_Tray", L"System Tray")));
        NavItemFlyoutSlider().Content(box_value(g_Loc.Get(L"Nav_FlyoutSlider", L"Flyout Slider")));
        NavItemAbout().Content(box_value(g_Loc.Get(L"Nav_About", L"About")));

        auto ensureNavIcon = [](winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem const& item, winrt::hstring const& glyph)
        {
            if (!item) return;
            auto fi = item.Icon().try_as<winrt::Microsoft::UI::Xaml::Controls::FontIcon>();
            if (!fi)
            {
                winrt::Microsoft::UI::Xaml::Controls::FontIcon newIcon;
                newIcon.Glyph(glyph);
                newIcon.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
                item.Icon(newIcon);
            }
            else
            {
                fi.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
            }
        };

        ensureNavIcon(NavItemGeneral(), L"\uE713");
        ensureNavIcon(NavItemShortcuts(), L"\uE765");
        ensureNavIcon(NavItemOSD(), L"\uE7F4");
        ensureNavIcon(NavItemTray(), L"\uE718");
        ensureNavIcon(NavItemFlyoutSlider(), L"\uE9E9");
        ensureNavIcon(NavItemAbout(), L"\uE946");

        SettingsSearchBox().PlaceholderText(g_Loc.Get(L"Search_Placeholder", L"Search settings"));

        // General
        GeneralHeaderTitle().Text(g_Loc.Get(L"General_Title", L"General Settings"));
        GeneralHeaderSubtitle().Text(g_Loc.Get(L"General_Subtitle", L"Windows 11 DDC/CI Hardware Monitor Brightness Control"));
        ResetDefaultsTitleText().Text(g_Loc.Get(L"Opt_ResetDefaults", L"Reset Settings"));
        ResetDefaultsDescText().Text(g_Loc.Get(L"Desc_ResetDefaults", L"Restore all settings to their default values"));
        ResetDefaultsButtonText().Text(g_Loc.Get(L"Btn_ResetDefaults", L"Reset Defaults"));
        ResetDefaultsConfirmText().Text(g_Loc.Get(L"Confirm_ResetDefaults", L"All settings will be reset. Do you want to continue?"));
        ConfirmResetDefaultsButton().Content(box_value(g_Loc.Get(L"Btn_ConfirmResetDefaults", L"Yes, reset settings")));
        ToolTipService::SetToolTip(ResetDefaultsButton(), box_value(g_Loc.Get(L"Tip_ResetDefaults", L"Reset all settings to default values")));

        AppearanceSectionTitle().Text(g_Loc.Get(L"Sec_Appearance", L"Appearance"));
        LanguageTitleText().Text(g_Loc.Get(L"Opt_Language", L"Language"));
        LanguageDescText().Text(g_Loc.Get(L"Desc_Language", L"Select application display language. If the Windows language is unsupported, English (United States) is used."));

        AppThemeTitleText().Text(g_Loc.Get(L"Opt_AppTheme", L"App Theme"));
        AppThemeDescText().Text(g_Loc.Get(L"Desc_AppTheme", L"Visual appearance of the settings window"));
        AppThemeItemFollowWindows().Content(box_value(g_Loc.Get(L"Theme_FollowWindows", L"Follow Windows (app)")));
        AppThemeItemDark().Content(box_value(g_Loc.Get(L"Theme_Dark", L"Dark")));
        AppThemeItemLight().Content(box_value(g_Loc.Get(L"Theme_Light", L"Light")));
        int curAppTheme = AppThemeComboBox().SelectedIndex();
        if (curAppTheme >= 0)
        {
            AppThemeComboBox().SelectedIndex(-1);
            AppThemeComboBox().SelectedIndex(curAppTheme);
        }

        TransparencyEffectsTitleText().Text(g_Loc.Get(L"Opt_TransparencyEffects", L"Transparency Effects"));
        TransparencyEffectsDescText().Text(g_Loc.Get(L"Desc_TransparencyEffects", L"Enable Mica and Acrylic backdrop transparency across all app windows"));
        TransparencyEffectsStatusText().Text(TransparencyEffectsToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        StartupDisplaysSectionTitle().Text(g_Loc.Get(L"Sec_StartupDisplays", L"Startup & Displays"));
        StartWithWindowsTitleText().Text(g_Loc.Get(L"Opt_StartWithWindows", L"Start with Windows"));
        bool isInstalled = AppSettings::IsInstalledInProgramFiles();
        if (isInstalled)
        {
            StartWithWindowsDescText().Text(g_Loc.Get(L"Desc_StartWithWindows", L"Launch ClickBrightness automatically on system logon"));
            StartWithWindowsToggle().IsEnabled(true);
            StartWithWindowsStatusText().Text(StartWithWindowsToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        }
        else
        {
            StartWithWindowsDescText().Text(g_Loc.Get(L"Desc_StartWithWindows_NotInstalled", L"Launch ClickBrightness automatically on system logon (Available when installed in Program Files)"));
            StartWithWindowsToggle().IsEnabled(false);
            StartWithWindowsToggle().IsOn(false);
            StartWithWindowsStatusText().Text(g_Loc.Get(L"Common_Off", L"Off"));
        }

        MonitorsToControlTitleText().Text(g_Loc.Get(L"Opt_MonitorsToControl", L"Monitors to Control"));
        MonitorsToControlDescText().Text(g_Loc.Get(L"Desc_MonitorsToControl", L"Select which monitors have their brightness adjusted"));
        AllMonitorsCheckBox().Content(box_value(g_Loc.Get(L"Monitors_All", L"All Monitors")));

        PhysicalMonitorsTitleText().Text(g_Loc.Get(L"Opt_PhysicalMonitors", L"Physical Monitors"));
        RedetectMonitorsButton().Content(box_value(g_Loc.Get(L"Btn_Redetect", L"Re-detect")));

        // Shortcuts
        ShortcutsHeaderTitle().Text(g_Loc.Get(L"Shortcuts_Title", L"Keyboard Shortcuts"));
        ShortcutsHeaderSubtitle().Text(g_Loc.Get(L"Shortcuts_Subtitle", L"Global hotkeys for brightness stepping and preset jumping"));
        ShortcutsAdjustSectionTitle().Text(g_Loc.Get(L"Sec_ShortcutsAdjust", L"Brightness Stepping Shortcuts"));
        HotkeyAdjustTitleText().Text(g_Loc.Get(L"Opt_HotkeyAdjust", L"Increase & Decrease Shortcuts"));
        HotkeyAdjustDescText().Text(g_Loc.Get(L"Desc_HotkeyAdjust", L"Enable global keyboard shortcuts to raise and lower brightness"));
        HotkeyAdjustStatusText().Text(HotkeyAdjustToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        KeyUpTitleText().Text(g_Loc.Get(L"Opt_KeyUp", L"Increase Brightness"));
        KeyUpDescText().Text(g_Loc.Get(L"Desc_KeyUp", L"Global shortcut to raise hardware brightness"));
        KeyDownTitleText().Text(g_Loc.Get(L"Opt_KeyDown", L"Decrease Brightness"));
        KeyDownDescText().Text(g_Loc.Get(L"Desc_KeyDown", L"Global shortcut to lower hardware brightness"));
        StepTitleText().Text(g_Loc.Get(L"Opt_Step", L"Brightness Step"));
        StepDescText().Text(g_Loc.Get(L"Desc_Step", L"Percentage adjustment per hotkey press"));
        HotkeyAdjustShowOsdTitleText().Text(g_Loc.Get(L"Opt_HotkeyAdjustShowOsd", L"Show OSD Banner"));
        HotkeyAdjustShowOsdDescText().Text(g_Loc.Get(L"Desc_HotkeyAdjustShowOsd", L"Display the on-screen brightness banner when pressing stepping hotkeys"));
        HotkeyAdjustShowOsdStatusText().Text(HotkeyAdjustShowOsdToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        ShortcutsPresetsSectionTitle().Text(g_Loc.Get(L"Sec_ShortcutsPresets", L"Preset Shortcuts"));
        HotkeyPresetsTitleText().Text(g_Loc.Get(L"Opt_HotkeyPresets", L"Preset Shortcuts"));
        HotkeyPresetsDescText().Text(g_Loc.Get(L"Desc_HotkeyPresets", L"Enable global keyboard shortcuts to jump to brightness presets"));
        HotkeyPresetsStatusText().Text(HotkeyPresetsToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        Preset1KeyTitleText().Text(g_Loc.Get(L"Opt_KeyPreset1", L"Preset 1 Shortcut"));
        Preset1KeyDescText().Text(g_Loc.Get(L"Desc_KeyPreset1", L"Global shortcut to set brightness to Preset 1"));
        Preset1BrightnessTitleText().Text(g_Loc.Get(L"Opt_Preset1Brightness", L"Preset 1 Brightness (%)"));
        Preset1BrightnessDescText().Text(g_Loc.Get(L"Desc_Preset1Brightness", L"Target brightness level applied by the Preset 1 shortcut"));

        Preset2KeyTitleText().Text(g_Loc.Get(L"Opt_KeyPreset2", L"Preset 2 Shortcut"));
        Preset2KeyDescText().Text(g_Loc.Get(L"Desc_KeyPreset2", L"Global shortcut to set brightness to Preset 2"));
        Preset2BrightnessTitleText().Text(g_Loc.Get(L"Opt_Preset2Brightness", L"Preset 2 Brightness (%)"));
        Preset2BrightnessDescText().Text(g_Loc.Get(L"Desc_Preset2Brightness", L"Target brightness level applied by the Preset 2 shortcut"));
        HotkeyPresetsShowOsdTitleText().Text(g_Loc.Get(L"Opt_HotkeyPresetsShowOsd", L"Show OSD Banner"));
        HotkeyPresetsShowOsdDescText().Text(g_Loc.Get(L"Desc_HotkeyPresetsShowOsd", L"Display the on-screen brightness banner when switching presets"));
        HotkeyPresetsShowOsdStatusText().Text(HotkeyPresetsShowOsdToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        ResetShortcutsTitleText().Text(g_Loc.Get(L"Opt_ResetShortcuts", L"Reset Shortcuts"));
        ResetShortcutsDescText().Text(g_Loc.Get(L"Desc_ResetShortcuts", L"Restore hotkeys to defaults (F14, F13, F15, F16)"));
        ResetHotkeysButton().Content(box_value(g_Loc.Get(L"Btn_ResetHotkeys", L"Reset Hotkeys")));

        TimingDelaySectionTitle().Text(g_Loc.Get(L"Sec_TimingDelay", L"Timing & Hardware Delay"));
        DelayEnabledTitleText().Text(g_Loc.Get(L"Opt_DelayEnabled", L"Delayed Brightness Change"));
        DelayEnabledDescText().Text(g_Loc.Get(L"Desc_DelayEnabled", L"Debounce rapid key presses before hardware adjustments"));
        DelayEnabledStatusText().Text(DelayEnabledToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        WaitTimeTitleText().Text(g_Loc.Get(L"Opt_WaitTime", L"Wait Time (ms)"));
        WaitTimeDescText().Text(g_Loc.Get(L"Desc_WaitTime", L"Hardware debounce delay before updating monitor"));
        HoldSpeedTitleText().Text(g_Loc.Get(L"Opt_HoldSpeed", L"Hold Speed (ms)"));
        HoldSpeedDescText().Text(g_Loc.Get(L"Desc_HoldSpeed", L"Key hold interval for smooth stepping (higher = slower)"));

        // OSD
        OsdHeaderTitle().Text(g_Loc.Get(L"OSD_Title", L"On-Screen Display (OSD)"));
        OsdHeaderSubtitle().Text(g_Loc.Get(L"OSD_Subtitle", L"Visual overlay popup for real-time brightness adjustments"));
        OverlayThemeTitleText().Text(g_Loc.Get(L"Opt_OverlayTheme", L"OSD Banner Theme"));
        OverlayThemeDescText().Text(g_Loc.Get(L"Desc_OverlayTheme", L"Visual appearance and color theme of the brightness overlay banner"));
        OverlayThemeItemFollowWindows().Content(box_value(g_Loc.Get(L"Theme_FollowWindowsSystem", L"Follow Windows (system)")));
        OverlayThemeItemDark().Content(box_value(g_Loc.Get(L"Theme_Dark", L"Dark")));
        OverlayThemeItemLight().Content(box_value(g_Loc.Get(L"Theme_Light", L"Light")));
        int curOverlayTheme = OverlayThemeComboBox().SelectedIndex();
        if (curOverlayTheme >= 0)
        {
            OverlayThemeComboBox().SelectedIndex(-1);
            OverlayThemeComboBox().SelectedIndex(curOverlayTheme);
        }

        OsdAccentSectionTitle().Text(g_Loc.Get(L"Sec_OverlayAccent", L"Accent Color"));
        OverlayUseWindowsAccentTitleText().Text(g_Loc.Get(L"Opt_OverlayUseWindowsAccent", L"Use Windows Accent Color"));
        OverlayUseWindowsAccentDescText().Text(g_Loc.Get(L"Desc_OverlayUseWindowsAccent", L"Apply system accent color to the overlay brightness bar"));
        OverlayUseWindowsAccentStatusText().Text(OverlayUseWindowsAccentToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        OverlayCustomAccentTitleText().Text(g_Loc.Get(L"Opt_OverlayCustomAccent", L"Custom Accent Color"));
        OverlayCustomAccentDescText().Text(g_Loc.Get(L"Desc_OverlayCustomAccent", L"Fill color for overlay brightness bar (configured when Windows accent is off)"));

        OsdTimingSectionTitle().Text(g_Loc.Get(L"Sec_Timing", L"Timing"));
        OverlayTimeoutTitleText().Text(g_Loc.Get(L"Opt_OverlayTimeout", L"Hide Time (ms)"));
        OverlayTimeoutDescText().Text(g_Loc.Get(L"Desc_OverlayTimeout", L"Auto-hide timeout for banner overlay"));

        // System Tray
        TrayHeaderTitle().Text(g_Loc.Get(L"Tray_Title", L"System Tray Icon"));
        TrayHeaderSubtitle().Text(g_Loc.Get(L"Tray_Subtitle", L"Taskbar indicator, live percentage typography, and mouse actions"));
        TrayAppearanceSectionTitle().Text(g_Loc.Get(L"Sec_TrayAppearance", L"System Tray Icon"));
        TrayShowPercentageTitleText().Text(g_Loc.Get(L"Opt_TrayShowPercentage", L"Show Percentage Icon"));
        TrayShowPercentageDescText().Text(g_Loc.Get(L"Desc_TrayShowPercentage", L"Display current brightness percentage on the system tray icon"));
        TrayShowPercentageStatusText().Text(TrayShowPercentageToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayTypographySectionTitle().Text(g_Loc.Get(L"Sec_TrayTypography", L"System Tray Icon Typography"));
        TrayFontTitleText().Text(g_Loc.Get(L"Opt_TrayFont", L"Number Font"));
        TrayFontDescText().Text(g_Loc.Get(L"Desc_TrayFont", L"DirectWrite rendered font for tray icon"));
        TrayFontSizeTitleText().Text(g_Loc.Get(L"Opt_TrayFontSize", L"Font Size (%)"));
        TrayFontSizeDescText().Text(g_Loc.Get(L"Desc_TrayFontSize", L"Scale for 1-2 digits (0-99%)"));
        TrayFontSize100TitleText().Text(g_Loc.Get(L"Opt_TrayFontSize100", L"100% Width (%)"));
        TrayFontSize100DescText().Text(g_Loc.Get(L"Desc_TrayFontSize100", L"Condensed horizontal scale for 3 digits (100%)"));
        TrayYOffsetTitleText().Text(g_Loc.Get(L"Opt_TrayYOffset", L"Vertical Offset (px)"));
        TrayYOffsetDescText().Text(g_Loc.Get(L"Desc_TrayYOffset", L"Baseline vertical adjustment (e.g. 0.0, -0.5, 1.5)"));

        TrayWheelSectionTitle().Text(g_Loc.Get(L"Sec_TrayWheel", L"System Tray Mouse Wheel Control"));
        TrayWheelTitleText().Text(g_Loc.Get(L"Opt_TrayWheel", L"Scroll on Tray Icon"));
        TrayWheelDescText().Text(g_Loc.Get(L"Desc_TrayWheel", L"Adjust brightness by scrolling the mouse wheel directly over the notification tray icon"));
        TrayWheelStatusText().Text(TrayWheelToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayWheelStepTitleText().Text(g_Loc.Get(L"Opt_TrayWheelStep", L"Brightness Step"));
        TrayWheelStepDescText().Text(g_Loc.Get(L"Desc_TrayWheelStep", L"Percentage adjustment per mouse wheel notch"));
        TrayWheelShowOsdTitleText().Text(g_Loc.Get(L"Opt_TrayWheelShowOsd", L"Show OSD When Scrolling"));
        TrayWheelShowOsdDescText().Text(g_Loc.Get(L"Desc_TrayWheelShowOsd", L"Display the on-screen brightness banner while scrolling over the tray icon"));
        TrayWheelShowOsdStatusText().Text(TrayWheelShowOsdToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayWheelDelayTitleText().Text(g_Loc.Get(L"Opt_TrayWheelDelay", L"Scrolling Hardware Delay"));
        TrayWheelDelayDescText().Text(g_Loc.Get(L"Desc_TrayWheelDelay", L"Debounce rapid mouse wheel scrolling before updating monitor hardware"));
        TrayWheelDelayStatusText().Text(TrayWheelDelayToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayWheelWaitTimeTitleText().Text(g_Loc.Get(L"Opt_TrayWheelWaitTime", L"Scrolling Wait Time (ms)"));
        TrayWheelWaitTimeDescText().Text(g_Loc.Get(L"Desc_TrayWheelWaitTime", L"Delay in milliseconds before sending hardware brightness commands to the display"));

        TrayMiddleClickSectionTitle().Text(g_Loc.Get(L"Sec_TrayMiddleClick", L"System Tray Middle-Click Action"));
        TrayMiddleClickTitleText().Text(g_Loc.Get(L"Opt_TrayMiddleClick", L"Middle-Click Quick Action"));
        TrayMiddleClickDescText().Text(g_Loc.Get(L"Desc_TrayMiddleClick", L"Set a specific brightness level when clicking the middle mouse button on the tray icon"));
        TrayMiddleClickStatusText().Text(TrayMiddleClickToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayMiddleClickShowOsdTitleText().Text(g_Loc.Get(L"Opt_TrayMiddleClickShowOsd", L"Show OSD On Middle-Click"));
        TrayMiddleClickShowOsdDescText().Text(g_Loc.Get(L"Desc_TrayMiddleClickShowOsd", L"Display the on-screen brightness banner when middle-clicking the tray icon"));
        TrayMiddleClickShowOsdStatusText().Text(TrayMiddleClickShowOsdToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayMiddleClickBrightnessTitleText().Text(g_Loc.Get(L"Opt_TrayMiddleClickBrightness", L"Target Brightness (%)"));
        TrayMiddleClickBrightnessDescText().Text(g_Loc.Get(L"Desc_TrayMiddleClickBrightness", L"Brightness percentage applied when middle-clicking the tray icon"));

        // Flyout
        FlyoutSliderHeaderTitle().Text(g_Loc.Get(L"Flyout_Title", L"Flyout Slider"));
        FlyoutSliderHeaderSubtitle().Text(g_Loc.Get(L"FlyoutSlider_Subtitle", L"Hardware brightness slider popup with mouse wheel control"));
        FlyoutScrollEnabledTitleText().Text(g_Loc.Get(L"Opt_FlyoutScrollEnabled", L"Slider Scrolling"));
        FlyoutScrollEnabledDescText().Text(g_Loc.Get(L"Desc_FlyoutScrollEnabled", L"Adjust brightness by scrolling the mouse wheel over the flyout slider"));
        FlyoutScrollEnabledStatusText().Text(FlyoutScrollEnabledToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        FlyoutScrollStepTitleText().Text(g_Loc.Get(L"Opt_FlyoutScrollStep", L"Scrolling Brightness Step"));
        FlyoutScrollStepDescText().Text(g_Loc.Get(L"Desc_FlyoutScrollStep", L"Percentage adjustment per mouse wheel notch on the flyout slider"));
        FlyoutTimingDelaySectionTitle().Text(g_Loc.Get(L"Sec_FlyoutTimingDelay", L"Timing & Hardware Delay"));

        FlyoutScrollDelayTitleText().Text(g_Loc.Get(L"Opt_FlyoutScrollDelay", L"Slider Scrolling Delay"));
        FlyoutScrollDelayDescText().Text(g_Loc.Get(L"Desc_FlyoutScrollDelay", L"Debounce rapid mouse wheel scrolling before hardware adjustments"));
        FlyoutScrollDelayStatusText().Text(FlyoutScrollDelayToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        FlyoutScrollWaitTimeTitleText().Text(g_Loc.Get(L"Opt_FlyoutScrollWaitTime", L"Scrolling Wait Time (ms)"));
        FlyoutScrollWaitTimeDescText().Text(g_Loc.Get(L"Desc_FlyoutScrollWaitTime", L"Hardware debounce delay after scrolling before updating monitor"));

        FlyoutButtonDelayTitleText().Text(g_Loc.Get(L"Opt_FlyoutButtonDelay", L"Button Clicking Delay"));
        FlyoutButtonDelayDescText().Text(g_Loc.Get(L"Desc_FlyoutButtonDelay", L"Debounce rapid preset button clicks before hardware adjustments"));
        FlyoutButtonDelayStatusText().Text(FlyoutButtonDelayToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        FlyoutButtonWaitTimeTitleText().Text(g_Loc.Get(L"Opt_FlyoutButtonWaitTime", L"Button Wait Time (ms)"));
        FlyoutButtonWaitTimeDescText().Text(g_Loc.Get(L"Desc_FlyoutButtonWaitTime", L"Hardware debounce delay after clicking buttons before updating monitor"));

        FlyoutAccentSectionTitle().Text(g_Loc.Get(L"Sec_FlyoutAccent", L"Accent Color"));
        FlyoutUseWindowsAccentTitleText().Text(g_Loc.Get(L"Opt_FlyoutUseWindowsAccent", L"Use Windows Accent Color"));
        FlyoutUseWindowsAccentDescText().Text(g_Loc.Get(L"Desc_FlyoutUseWindowsAccent", L"Apply system accent color to the flyout slider filled track"));
        FlyoutUseWindowsAccentStatusText().Text(FlyoutUseWindowsAccentToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        FlyoutCustomAccentTitleText().Text(g_Loc.Get(L"Opt_FlyoutCustomAccent", L"Custom Accent Color"));
        FlyoutCustomAccentDescText().Text(g_Loc.Get(L"Desc_FlyoutCustomAccent", L"Fill color for flyout slider track (configured when Windows accent is off)"));

        // About
        AboutHeaderTitle().Text(g_Loc.Get(L"About_Title", L"About ClickBrightness"));
        AboutHeaderSubtitle().Text(g_Loc.Get(L"About_Subtitle", L"Lightweight, modern Windows 11 DDC/CI monitor brightness utility"));
        AboutAppNameText().Text(g_Loc.Get(L"About_AppInfo", L"ClickBrightness"));
        AboutVersionText().Text(g_Loc.Get(L"About_Version", L"Version 1.0.3 (x64 Release)"));
        AboutAppTypeText().Text(g_Loc.Get(L"About_AppType", L"Native WinUI 3 / Windows App SDK 2.6 Desktop Application"));
        AboutCopyrightText().Text(g_Loc.Get(L"About_Copyright", L"Copyright (C) 2026 andris155"));
        AboutDescTitleText().Text(g_Loc.Get(L"About_DescTitle", L"Description"));
        AboutDescContentText().Text(g_Loc.Get(L"About_Description", L"A modern, lightweight Windows 11 utility for controlling hardware monitor brightness via DDC/CI (VCP code 0x10). Supports on-screen display (OSD), system tray live indicator with mouse wheel stepping, and a smooth flyout slider."));
        AboutLicenseTitleText().Text(g_Loc.Get(L"About_LicenseTitle", L"License"));
        AboutLicenseContentText().Text(g_Loc.Get(L"About_License", L"Free and Open Source under the MIT License."));

        m_isUpdatingUI = prevUpdating;
    }

    void SettingsWindow::StartCapture(int target)
    {
        StopCapture(false);
        s_currentCaptureInstance = this;
        s_captureMods = 0;
        m_captureTarget = target;

        UpdateCaptureHint(0);

        s_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(NULL), 0);
    }

    void SettingsWindow::StopCapture(bool saveKey)
    {
        if (s_hKeyboardHook)
        {
            UnhookWindowsHookEx(s_hKeyboardHook);
            s_hKeyboardHook = NULL;
        }
        s_currentCaptureInstance = nullptr;
        s_captureMods = 0;

        m_captureTarget = 0;
        try
        {
            KeyUpButton().Content(box_value(HotkeyManager::FormatHotkey(m_curUpMods, m_curUpKey)));
            KeyDownButton().Content(box_value(HotkeyManager::FormatHotkey(m_curDownMods, m_curDownKey)));
            KeyPreset1Button().Content(box_value(HotkeyManager::FormatHotkey(m_curPreset1Mods, m_curPreset1Key)));
            KeyPreset2Button().Content(box_value(HotkeyManager::FormatHotkey(m_curPreset2Mods, m_curPreset2Key)));
        }
        catch (...)
        {
        }
    }

    void SettingsWindow::UpdateCaptureHint(UINT mods)
    {
        if (m_captureTarget == 0) return;

        std::wstring hint = L"[ ";
        if (mods != 0)
        {
            hint += HotkeyManager::GetModifiersDisplayName(mods);
            hint += L"Press key... ]";
        }
        else
        {
            hint += L"Press any key... ]";
        }

        if (m_captureTarget == 1) KeyUpButton().Content(box_value(hint));
        else if (m_captureTarget == 2) KeyDownButton().Content(box_value(hint));
        else if (m_captureTarget == 3) KeyPreset1Button().Content(box_value(hint));
        else if (m_captureTarget == 4) KeyPreset2Button().Content(box_value(hint));
    }

    void SettingsWindow::HandleCapturedKey(UINT vkCode, UINT mods)
    {
        if (m_captureTarget == 0) return;

        if (vkCode == VK_ESCAPE)
        {
            StopCapture(false);
            return;
        }

        if (m_captureTarget == 1)
        {
            m_curUpKey = vkCode;
            m_curUpMods = mods;
            g_Settings.hotkeyUpKey = m_curUpKey;
            g_Settings.hotkeyUpMods = m_curUpMods;
        }
        else if (m_captureTarget == 2)
        {
            m_curDownKey = vkCode;
            m_curDownMods = mods;
            g_Settings.hotkeyDownKey = m_curDownKey;
            g_Settings.hotkeyDownMods = m_curDownMods;
        }
        else if (m_captureTarget == 3)
        {
            m_curPreset1Key = vkCode;
            m_curPreset1Mods = mods;
            g_Settings.hotkeyPreset1Key = m_curPreset1Key;
            g_Settings.hotkeyPreset1Mods = m_curPreset1Mods;
        }
        else if (m_captureTarget == 4)
        {
            m_curPreset2Key = vkCode;
            m_curPreset2Mods = mods;
            g_Settings.hotkeyPreset2Key = m_curPreset2Key;
            g_Settings.hotkeyPreset2Mods = m_curPreset2Mods;
        }

        StopCapture(true);
        ApplyAndSave();
    }

    LRESULT CALLBACK SettingsWindow::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
    {
        if (nCode == HC_ACTION && s_currentCaptureInstance)
        {
            bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
            if (isDown || isUp)
            {
                KBDLLHOOKSTRUCT* pKey = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
                if (pKey)
                {
                    UINT vkCode = pKey->vkCode;
                    UINT modBit = 0;
                    if (vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL) modBit = MOD_CONTROL;
                    else if (vkCode == VK_MENU || vkCode == VK_LMENU || vkCode == VK_RMENU) modBit = MOD_ALT;
                    else if (vkCode == VK_SHIFT || vkCode == VK_LSHIFT || vkCode == VK_RSHIFT) modBit = MOD_SHIFT;
                    else if (vkCode == VK_LWIN || vkCode == VK_RWIN) modBit = MOD_WIN;

                    if (modBit != 0)
                    {
                        if (isDown) s_captureMods |= modBit;
                        else s_captureMods &= ~modBit;
                        s_currentCaptureInstance->UpdateCaptureHint(s_captureMods);
                        return 1;
                    }

                    if (isDown)
                    {
                        s_currentCaptureInstance->HandleCapturedKey(vkCode, s_captureMods);
                    }
                    return 1;
                }
            }
        }
        return CallNextHookEx(s_hKeyboardHook, nCode, wParam, lParam);
    }

    LRESULT CALLBACK SettingsWindow::SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        auto* pThis = reinterpret_cast<SettingsWindow*>(dwRefData);
        if (!pThis) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        if (uMsg == WM_GETICON)
        {
            if (wParam == ICON_BIG || wParam == 2 /* ICON_BIG2 */)
            {
                if (pThis->m_hIconBig) return reinterpret_cast<LRESULT>(pThis->m_hIconBig);
            }
            else if (wParam == ICON_SMALL)
            {
                if (pThis->m_hIconSmall) return reinterpret_cast<LRESULT>(pThis->m_hIconSmall);
            }
            if (pThis->m_hIconSmall) return reinterpret_cast<LRESULT>(pThis->m_hIconSmall);
            if (pThis->m_hIconBig) return reinterpret_cast<LRESULT>(pThis->m_hIconBig);
        }
        else if (uMsg == WM_ERASEBKGND)
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            if (hdc)
            {
                RECT rc;
                GetClientRect(hWnd, &rc);
                bool isDark = ThemeHelper::IsAppInDarkMode(g_Settings.appTheme);
                COLORREF bg = isDark ? RGB(32, 32, 32) : RGB(243, 243, 243);
                HBRUSH hbr = CreateSolidBrush(bg);
                FillRect(hdc, &rc, hbr);
                DeleteObject(hbr);
                return 1;
            }
        }
        else if (uMsg == WM_NCDESTROY)
        {
            RemoveWindowSubclass(hWnd, SubclassProc, uIdSubclass);
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    void SettingsWindow::LoadSettingsToUI()
    {
        m_isUpdatingUI = true;

        // Select current language in LanguageComboBox without rebuilding the items
        auto langs = g_Loc.GetAvailableLanguages();
        int selectedLangIndex = 0;
        for (size_t i = 0; i < langs.size(); ++i)
        {
            if (_wcsicmp(langs[i].code.c_str(), g_Settings.language.c_str()) == 0)
            {
                selectedLangIndex = static_cast<int>(i);
                break;
            }
        }
        LanguageComboBox().SelectedIndex(selectedLangIndex);

        // App Theme (0 = Follow Windows, 1 = Dark, 2 = Light)
        int aTheme = g_Settings.appTheme;
        if (aTheme < 0 || aTheme > 2) aTheme = 0;
        AppThemeComboBox().SelectedIndex(aTheme);

        // Transparency Effects
        TransparencyEffectsToggle().IsOn(g_Settings.transparencyEnabled);
        TransparencyEffectsStatusText().Text(g_Settings.transparencyEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        // Hotkey Adjust Toggle & Cards
        HotkeyAdjustToggle().IsOn(g_Settings.hotkeyAdjustEnabled);
        HotkeyAdjustStatusText().Text(g_Settings.hotkeyAdjustEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        KeyUpCard().Visibility(g_Settings.hotkeyAdjustEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        KeyDownCard().Visibility(g_Settings.hotkeyAdjustEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        StepCard().Visibility(g_Settings.hotkeyAdjustEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        HotkeyAdjustShowOsdCard().Visibility(g_Settings.hotkeyAdjustEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        HotkeyAdjustShowOsdToggle().IsOn(g_Settings.hotkeyAdjustShowOsd);
        HotkeyAdjustShowOsdStatusText().Text(g_Settings.hotkeyAdjustShowOsd ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        m_curUpKey = g_Settings.hotkeyUpKey;
        m_curUpMods = g_Settings.hotkeyUpMods;
        m_curDownKey = g_Settings.hotkeyDownKey;
        m_curDownMods = g_Settings.hotkeyDownMods;

        // Hotkey Presets Toggle & Cards
        HotkeyPresetsToggle().IsOn(g_Settings.hotkeyPresetsEnabled);
        HotkeyPresetsStatusText().Text(g_Settings.hotkeyPresetsEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        Preset1KeyCard().Visibility(g_Settings.hotkeyPresetsEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        Preset1BrightnessCard().Visibility(g_Settings.hotkeyPresetsEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        Preset2KeyCard().Visibility(g_Settings.hotkeyPresetsEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        Preset2BrightnessCard().Visibility(g_Settings.hotkeyPresetsEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        HotkeyPresetsShowOsdCard().Visibility(g_Settings.hotkeyPresetsEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        HotkeyPresetsShowOsdToggle().IsOn(g_Settings.hotkeyPresetsShowOsd);
        HotkeyPresetsShowOsdStatusText().Text(g_Settings.hotkeyPresetsShowOsd ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        m_curPreset1Key = g_Settings.hotkeyPreset1Key;
        m_curPreset1Mods = g_Settings.hotkeyPreset1Mods;
        m_curPreset2Key = g_Settings.hotkeyPreset2Key;
        m_curPreset2Mods = g_Settings.hotkeyPreset2Mods;
        KeyUpButton().Content(box_value(HotkeyManager::FormatHotkey(m_curUpMods, m_curUpKey)));
        KeyDownButton().Content(box_value(HotkeyManager::FormatHotkey(m_curDownMods, m_curDownKey)));
        KeyPreset1Button().Content(box_value(HotkeyManager::FormatHotkey(m_curPreset1Mods, m_curPreset1Key)));
        KeyPreset2Button().Content(box_value(HotkeyManager::FormatHotkey(m_curPreset2Mods, m_curPreset2Key)));
        Preset1BrightnessSlider().Value(g_Settings.hotkeyPreset1Brightness);
        Preset1BrightnessNumberBox().Value(g_Settings.hotkeyPreset1Brightness);
        Preset2BrightnessSlider().Value(g_Settings.hotkeyPreset2Brightness);
        Preset2BrightnessNumberBox().Value(g_Settings.hotkeyPreset2Brightness);

        // Step ComboBox (1, 2, 5, 10, 15, 20)
        int stepIdx = 2; // default 5%
        if (g_Settings.hotkeyStep == 1) stepIdx = 0;
        else if (g_Settings.hotkeyStep == 2) stepIdx = 1;
        else if (g_Settings.hotkeyStep == 5) stepIdx = 2;
        else if (g_Settings.hotkeyStep == 10) stepIdx = 3;
        else if (g_Settings.hotkeyStep == 15) stepIdx = 4;
        else if (g_Settings.hotkeyStep == 20) stepIdx = 5;
        StepComboBox().SelectedIndex(stepIdx);

        // Delay & Repeat
        DelayEnabledToggle().IsOn(g_Settings.hotkeyDelayEnabled);
        DelayEnabledStatusText().Text(g_Settings.hotkeyDelayEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        DelayWaitTimeCard().Visibility(g_Settings.hotkeyDelayEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        HoldSpeedCard().Visibility(g_Settings.hotkeyDelayEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        DelaySlider().Value(g_Settings.hotkeyDelayMs);
        DelayNumberBox().Value(g_Settings.hotkeyDelayMs);

        RepeatSlider().Value(g_Settings.hotkeyRepeatIntervalMs);
        RepeatNumberBox().Value(g_Settings.hotkeyRepeatIntervalMs);

        // Overlay
        int oTheme = g_Settings.overlayTheme;
        if (oTheme < 0 || oTheme > 2) oTheme = 0;
        OverlayThemeComboBox().SelectedIndex(oTheme);

        OverlayTimeoutSlider().Value(g_Settings.overlayTimeoutMs);
        OverlayTimeoutNumberBox().Value(g_Settings.overlayTimeoutMs);

        OverlayUseWindowsAccentToggle().IsOn(g_Settings.overlayUseWindowsAccent);
        OverlayUseWindowsAccentStatusText().Text(g_Settings.overlayUseWindowsAccent ? L"On" : L"Off");
        OverlayAccentColorCard().Visibility(g_Settings.overlayUseWindowsAccent ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);

        // Tray Percentage Icon
        TrayShowPercentageToggle().IsOn(g_Settings.trayShowPercentage);
        TrayShowPercentageStatusText().Text(g_Settings.trayShowPercentage ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayTypographyPanel().Visibility(g_Settings.trayShowPercentage ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        // Tray Font
        std::wstring curFont = g_Settings.trayFont;
        int foundFontIdx = -1;
        for (uint32_t i = 0; i < TrayFontComboBox().Items().Size(); ++i)
        {
            auto itemStr = unbox_value<hstring>(TrayFontComboBox().Items().GetAt(i));
            if (itemStr == curFont)
            {
                foundFontIdx = static_cast<int>(i);
                break;
            }
        }
        if (foundFontIdx >= 0)
        {
            TrayFontComboBox().SelectedIndex(foundFontIdx);
        }
        else
        {
            TrayFontComboBox().Items().Append(box_value(curFont));
            TrayFontComboBox().SelectedIndex(static_cast<int>(TrayFontComboBox().Items().Size() - 1));
        }

        // Font sizes
        TrayFontSizeSlider().Value(g_Settings.trayFontSize);
        TrayFontSizeNumberBox().Value(g_Settings.trayFontSize);

        TrayFontSize100Slider().Value(g_Settings.trayFontSize100);
        TrayFontSize100NumberBox().Value(g_Settings.trayFontSize100);

        TrayYOffsetSlider().Value(g_Settings.trayYOffset);
        TrayYOffsetTextBox().Text(AppSettings::FormatYOffset(g_Settings.trayYOffset, g_Settings.trayYOffsetUseComma));

        // Tray Mouse Wheel
        auto stepToIndex = [](int step) -> int
        {
            if (step == 1) return 0;
            if (step == 2) return 1;
            if (step == 5) return 2;
            if (step == 10) return 3;
            if (step == 15) return 4;
            if (step == 20) return 5;
            return 2; // default 5%
        };

        TrayWheelToggle().IsOn(g_Settings.trayWheelEnabled);
        TrayWheelStatusText().Text(g_Settings.trayWheelEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayWheelStepCard().Visibility(g_Settings.trayWheelEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TrayWheelStepComboBox().SelectedIndex(stepToIndex(g_Settings.trayWheelStep));
        TrayWheelShowOsdCard().Visibility(g_Settings.trayWheelEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TrayWheelDelayCard().Visibility(g_Settings.trayWheelEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        TrayWheelShowOsdToggle().IsOn(g_Settings.trayWheelShowOsd);
        TrayWheelShowOsdStatusText().Text(g_Settings.trayWheelShowOsd ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        TrayWheelDelayToggle().IsOn(g_Settings.trayWheelDelayEnabled);
        TrayWheelDelayStatusText().Text(g_Settings.trayWheelDelayEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayWheelWaitTimeCard().Visibility((g_Settings.trayWheelEnabled && g_Settings.trayWheelDelayEnabled) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TrayWheelDelaySlider().Value(g_Settings.trayWheelDelayMs);
        TrayWheelDelayNumberBox().Value(g_Settings.trayWheelDelayMs);

        // Tray Middle-Click Quick Action
        TrayMiddleClickToggle().IsOn(g_Settings.trayMiddleClickEnabled);
        TrayMiddleClickStatusText().Text(g_Settings.trayMiddleClickEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayMiddleClickShowOsdCard().Visibility(g_Settings.trayMiddleClickEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TrayMiddleClickCard().Visibility(g_Settings.trayMiddleClickEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        TrayMiddleClickShowOsdToggle().IsOn(g_Settings.trayMiddleClickShowOsd);
        TrayMiddleClickShowOsdStatusText().Text(g_Settings.trayMiddleClickShowOsd ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        TrayMiddleClickSlider().Value(g_Settings.trayMiddleClickBrightness);
        TrayMiddleClickNumberBox().Value(g_Settings.trayMiddleClickBrightness);

        // Start with Windows
        bool isInstalled = AppSettings::IsInstalledInProgramFiles();
        if (isInstalled)
        {
            bool startWithWin = AppSettings::IsStartWithWindowsEnabled();
            StartWithWindowsToggle().IsEnabled(true);
            StartWithWindowsToggle().IsOn(startWithWin);
            StartWithWindowsStatusText().Text(startWithWin ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        }
        else
        {
            StartWithWindowsToggle().IsEnabled(false);
            StartWithWindowsToggle().IsOn(false);
            StartWithWindowsStatusText().Text(g_Loc.Get(L"Common_Off", L"Off"));
        }

        // Flyout Handle Colors
        auto loadColorToUI = [](
            winrt::Microsoft::UI::Xaml::Controls::ColorPicker const& picker,
            winrt::Microsoft::UI::Xaml::Controls::TextBox const& textBox,
            winrt::Microsoft::UI::Xaml::Controls::Border const& preview,
            COLORREF col)
        {
            auto c = winrt::Microsoft::UI::ColorHelper::FromArgb(255, GetRValue(col), GetGValue(col), GetBValue(col));
            picker.Color(c);
            textBox.Text(AppSettings::FormatColor(col));
            preview.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(c));
        };

        // Accent Colors
        loadColorToUI(OverlayAccentColorPicker(), OverlayAccentColorTextBox(), OverlayAccentColorPreview(), g_Settings.overlayAccentColor);

        FlyoutUseWindowsAccentToggle().IsOn(g_Settings.flyoutUseWindowsAccent);
        FlyoutUseWindowsAccentStatusText().Text(g_Settings.flyoutUseWindowsAccent ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        FlyoutAccentColorCard().Visibility(g_Settings.flyoutUseWindowsAccent ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);
        loadColorToUI(FlyoutAccentColorPicker(), FlyoutAccentColorTextBox(), FlyoutAccentColorPreview(), g_Settings.flyoutAccentColor);

        FlyoutScrollEnabledToggle().IsOn(g_Settings.flyoutScrollEnabled);
        FlyoutScrollEnabledStatusText().Text(g_Settings.flyoutScrollEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        FlyoutScrollStepCard().Visibility(g_Settings.flyoutScrollEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        FlyoutScrollDelayCard().Visibility(g_Settings.flyoutScrollEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        FlyoutScrollStepComboBox().SelectedIndex(stepToIndex(g_Settings.flyoutScrollStep));

        FlyoutScrollDelayToggle().IsOn(g_Settings.flyoutScrollDelayEnabled);
        FlyoutScrollDelayStatusText().Text(g_Settings.flyoutScrollDelayEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        FlyoutScrollWaitTimeCard().Visibility((g_Settings.flyoutScrollEnabled && g_Settings.flyoutScrollDelayEnabled) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        FlyoutScrollDelaySlider().Value(g_Settings.flyoutScrollDelayMs);
        FlyoutScrollDelayNumberBox().Value(g_Settings.flyoutScrollDelayMs);

        FlyoutButtonDelayToggle().IsOn(g_Settings.flyoutButtonDelayEnabled);
        FlyoutButtonDelayStatusText().Text(g_Settings.flyoutButtonDelayEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        FlyoutButtonWaitTimeCard().Visibility(g_Settings.flyoutButtonDelayEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        FlyoutButtonDelaySlider().Value(g_Settings.flyoutButtonDelayMs);
        FlyoutButtonDelayNumberBox().Value(g_Settings.flyoutButtonDelayMs);

        UpdateMonitorsSelectionUI();

        m_isUpdatingUI = false;
    }

    void SettingsWindow::OnDisplayChanged()
    {
        try
        {
            PopulateMonitorsInfo();
            PopulateMonitorsSelectionList();
            UpdateMonitorsSelectionUI();
            UpdateTitleBarDragRegions();
            SetupBackdrop();
        }
        catch (...)
        {
        }
    }
}

void InitSettingsWindow(bool show)
{
    if (!s_settingsWindow)
    {
        try
        {
            s_settingsWindow = winrt::make<winrt::BrightnessApp::implementation::SettingsWindow>();
        }
        catch (...)
        {
        }
    }
    if (show)
    {
        ShowSettingsWindow();
    }
}

void ShowSettingsWindow()
{
    if (s_settingsWindow)
    {
        auto impl = winrt::get_self<winrt::BrightnessApp::implementation::SettingsWindow>(s_settingsWindow);
        if (impl && impl->IsClosing())
        {
            s_settingsWindow = nullptr;
        }
    }

    if (!s_settingsWindow)
    {
        s_settingsWindow = winrt::make<winrt::BrightnessApp::implementation::SettingsWindow>();
    }

    auto impl = winrt::get_self<winrt::BrightnessApp::implementation::SettingsWindow>(s_settingsWindow);
    if (impl)
    {
        impl->ShowWindow();
    }
}

void CloseSettingsWindow()
{
    if (s_settingsWindow)
    {
        try
        {
            auto impl = winrt::get_self<winrt::BrightnessApp::implementation::SettingsWindow>(s_settingsWindow);
            if (impl)
            {
                impl->SetExiting(true);
                impl->StopCapture(false);
                impl->SetClosing(true);
            }
            s_settingsWindow.Close();
            s_settingsWindow = nullptr;
        }
        catch (...) {}
    }
}

bool IsSettingsWindowOpen()
{
    if (!s_settingsWindow) return false;
    auto impl = winrt::get_self<winrt::BrightnessApp::implementation::SettingsWindow>(s_settingsWindow);
    return (impl && !impl->IsClosing());
}

void OnSettingsThemeChanged()
{
    if (s_settingsWindow)
    {
        auto impl = winrt::get_self<winrt::BrightnessApp::implementation::SettingsWindow>(s_settingsWindow);
        if (impl)
        {
            impl->ApplyTheme();
        }
    }
}

void OnSettingsDisplayChanged()
{
    if (s_settingsWindow)
    {
        auto impl = winrt::get_self<winrt::BrightnessApp::implementation::SettingsWindow>(s_settingsWindow);
        if (impl)
        {
            impl->OnDisplayChanged();
        }
    }
}

void TrimProcessWorkingSetAsync()
{
    std::thread([]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    }).detach();
}
