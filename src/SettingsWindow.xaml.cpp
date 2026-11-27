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
#include "ScheduleManager.h"

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Devices.Geolocation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <robuffer.h>
#include <commdlg.h>

namespace
{
    struct SunIconPreset
    {
        const wchar_t* spec;
        const wchar_t* label;
    };

    static const SunIconPreset kSunIconPresets[] = {
        { L"preset:sun",        L"Sun (Default)" },
        { L"preset:sun-dark",   L"Sun (Dark)" },
        { L"preset:sun-black",  L"Sun (Black)" },
        { L"preset:sun-white",  L"Sun (White)" },
        { L"preset:sun-red",    L"Sun (Red)" },
        { L"preset:sun-orange", L"Sun (Orange)" },
        { L"preset:sun-green",  L"Sun (Green)" },
        { L"preset:sun-teal",   L"Sun (Teal)" },
        { L"preset:sun-blue",   L"Sun (Blue)" },
        { L"preset:sun-purple", L"Sun (Purple)" },
        { L"preset:sun-pink",   L"Sun (Pink)" },
        { L"preset:sun-gray",   L"Sun (Gray)" },
    };

    static void PopulateSunPresetComboBox(winrt::Microsoft::UI::Xaml::Controls::ComboBox const& cb)
    {
        if (!cb || cb.Items().Size() > 0) return;
        for (const auto& p : kSunIconPresets)
        {
            cb.Items().Append(winrt::box_value(winrt::hstring(p.label)));
        }
        cb.Items().Append(winrt::box_value(winrt::hstring(L"Custom File...")));
    }

    static void SelectSunPresetInComboBox(winrt::Microsoft::UI::Xaml::Controls::ComboBox const& cb, const std::wstring& spec)
    {
        if (!cb) return;
        PopulateSunPresetComboBox(cb);
        int presetCount = static_cast<int>(std::size(kSunIconPresets));
        for (int i = 0; i < presetCount; ++i)
        {
            if (_wcsicmp(spec.c_str(), kSunIconPresets[i].spec) == 0)
            {
                cb.SelectedIndex(i);
                return;
            }
        }
        if (!spec.empty() && spec.rfind(L"preset:", 0) != 0)
        {
            cb.SelectedIndex(presetCount); // "Custom File..."
        }
        else
        {
            cb.SelectedIndex(0);
        }
    }

    static std::wstring BrowseForIcoFile(HWND owner)
    {
        wchar_t path[MAX_PATH] = { 0 };
        OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = L"Icon Files (*.ico)\0*.ico\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn))
        {
            return path;
        }
        return L"";
    }

    static void UpdateCustomIconPreview(winrt::Microsoft::UI::Xaml::Controls::Image const& img, const std::wstring& spec)
    {
        if (!img) return;
        const int sz = 32;
        HICON hIcon = TrayIcon::LoadCustomIconForSpec(spec, sz);
        if (!hIcon)
        {
            hIcon = TrayIcon::LoadCustomIconForSpec(L"preset:sun", sz);
        }
        if (!hIcon) return;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = sz;
        bmi.bmiHeader.biHeight = -sz; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HDC hdcScreen = GetDC(NULL);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hBmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (hBmp && bits)
        {
            HGDIOBJ oldObj = SelectObject(hdcMem, hBmp);
            memset(bits, 0, sz * sz * 4);
            DrawIconEx(hdcMem, 0, 0, hIcon, sz, sz, 0, NULL, DI_NORMAL);
            SelectObject(hdcMem, oldObj);

            winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap wb(sz, sz);
            auto buf = wb.PixelBuffer();
            auto byteAccess = buf.as<Windows::Storage::Streams::IBufferByteAccess>();
            byte* dst = nullptr;
            if (SUCCEEDED(byteAccess->Buffer(&dst)) && dst)
            {
                memcpy(dst, bits, sz * sz * 4);
            }
            wb.Invalidate();
            img.Source(wb);
        }
        if (hBmp) DeleteObject(hBmp);
        if (hdcMem) DeleteDC(hdcMem);
        if (hdcScreen) ReleaseDC(NULL, hdcScreen);
        DestroyIcon(hIcon);
    }
}
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
                m_appWindow.Title(L"ClickBrightness settings");
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
            FlushPendingSave();
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
            std::vector<winrt::Windows::Graphics::RectInt32> passthroughRects;
            auto addPassthrough = [&](winrt::Microsoft::UI::Xaml::FrameworkElement const& el)
            {
                if (!el) return;
                auto transform = el.TransformToVisual(nullptr);
                auto bounds = transform.TransformBounds(winrt::Windows::Foundation::Rect{
                    0.0f,
                    0.0f,
                    static_cast<float>(el.ActualWidth()),
                    static_cast<float>(el.ActualHeight())
                });
                if (bounds.Width > 0.0f && bounds.Height > 0.0f)
                {
                    passthroughRects.push_back({
                        static_cast<int32_t>(std::round(bounds.X * scale)),
                        static_cast<int32_t>(std::round(bounds.Y * scale)),
                        static_cast<int32_t>(std::round(bounds.Width * scale)),
                        static_cast<int32_t>(std::round(bounds.Height * scale))
                    });
                }
            };

            addPassthrough(SettingsSearchBox());
            if (TitleBarBackButton().Visibility() == winrt::Microsoft::UI::Xaml::Visibility::Visible)
            {
                addPassthrough(TitleBarBackButton());
            }

            if (!passthroughRects.empty())
            {
                nonClientSource.SetRegionRects(
                    winrt::Microsoft::UI::Input::NonClientRegionKind::Passthrough,
                    passthroughRects
                );
            }
        }
        catch (...)
        {
        }
    }

    void SettingsWindow::SetBackButtonUsable(bool usable)
    {
        auto btn = TitleBarBackButton();
        auto targetVis = usable ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
        bool visChanged = (btn.Visibility() != targetVis);
        btn.IsEnabled(usable);
        btn.Visibility(targetVis);
        TitleBarLeftPanel().Padding(usable
            ? winrt::Microsoft::UI::Xaml::Thickness{ 8, 0, 16, 0 }
            : winrt::Microsoft::UI::Xaml::Thickness{ 16, 0, 16, 0 });
        if (visChanged)
        {
            AppTitleBar().UpdateLayout();
            UpdateTitleBarDragRegions();
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

        if (m_hwnd)
        {
            KillTimer(m_hwnd, 102);
        }
        m_pendingSave = false;
        m_pendingSaveHotkeys = false;
        m_pendingSaveTray = false;

        SaveCurrentMonitorConfigFromUI();
        g_Settings.Save();
        HWND hMain = FindWindowW(L"ClickBrightness_MessageWndClass", NULL);
        if (hMain)
        {
            g_HotkeyManager.RegisterAll(hMain);
        }
        g_TrayIcon.SyncExtraTrayIcons();
    }

    void SettingsWindow::ScheduleSave(bool reRegisterHotkeys, bool syncTray)
    {
        if (m_isUpdatingUI) return;
        m_pendingSave = true;
        m_pendingSaveHotkeys |= reRegisterHotkeys;
        m_pendingSaveTray |= syncTray;
        if (m_hwnd)
        {
            KillTimer(m_hwnd, 102);
            SetTimer(m_hwnd, 102, 250, NULL);
        }
        else
        {
            FlushPendingSave();
        }
    }

    void SettingsWindow::FlushPendingSave()
    {
        if (m_hwnd)
        {
            KillTimer(m_hwnd, 102);
        }
        if (!m_pendingSave) return;
        bool doHotkeys = m_pendingSaveHotkeys;
        bool doTray = m_pendingSaveTray;
        m_pendingSave = false;
        m_pendingSaveHotkeys = false;
        m_pendingSaveTray = false;

        SaveCurrentMonitorConfigFromUI();
        g_Settings.Save();
        if (doHotkeys)
        {
            HWND hMain = FindWindowW(L"ClickBrightness_MessageWndClass", NULL);
            if (hMain)
            {
                g_HotkeyManager.RegisterAll(hMain);
            }
        }
        if (doTray)
        {
            g_TrayIcon.SyncExtraTrayIcons();
        }
    }

    void SettingsWindow::InitializeControls()
    {
        // Cache panels initially
        m_generalPanel = GeneralPanel();
        m_monitorsPanel = MonitorsPanel();
        m_monitorsMainView = MonitorsMainView();
        m_monitorsSubpageView = MonitorsSubpageView();
        m_timingsPanel = TimingsPanel();
        m_timingsMainView = TimingsMainView();
        m_timingsSubpageView = TimingsSubpageView();
        m_aboutPanel = AboutPanel();

        // Load actual program icon for Titlebar and About page
        try
        {
            HICON hTitleIcon = m_hIconSmall;
            bool destroyTitleIcon = false;
            if (!hTitleIcon)
            {
                hTitleIcon = (HICON)::LoadImageW(::GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
                if (hTitleIcon) destroyTitleIcon = true;
            }
            if (!hTitleIcon)
            {
                wchar_t exePath[MAX_PATH];
                if (GetModuleFileNameW(NULL, exePath, MAX_PATH))
                {
                    std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
                    std::filesystem::path icoPath = dir / L"resources" / L"program.ico";
                    if (std::filesystem::exists(icoPath))
                    {
                        hTitleIcon = (HICON)::LoadImageW(NULL, icoPath.c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
                        if (hTitleIcon) destroyTitleIcon = true;
                    }
                }
            }
            if (hTitleIcon)
            {
                UINT dpi = m_hwnd ? GetDpiForWindow(m_hwnd) : 96;
                if (dpi == 0) dpi = 96;
                int smIconSize = ::GetSystemMetricsForDpi(SM_CXSMICON, dpi);
                if (smIconSize <= 0) smIconSize = 16;
                auto wbTitle = HIconToWriteableBitmap(hTitleIcon, smIconSize, smIconSize);
                if (wbTitle)
                {
                    AppTitleBarIcon().Source(wbTitle);
                }
                if (destroyTitleIcon)
                {
                    ::DestroyIcon(hTitleIcon);
                }
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
                SetBackButtonUsable(false);
            }
            else if (tag == L"Monitors")
            {
                EnsureTrayTabLoaded();
                m_monitorsPanel.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                children.Append(m_monitorsPanel);
                OpenMonitorsSubpage(L"");
            }
            else if (tag == L"Timings")
            {
                m_timingsPanel.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                children.Append(m_timingsPanel);
                OpenTimingsSubpage(L"");
            }
            else if (tag == L"About")
            {
                EnsureAboutTabLoaded();
                m_aboutPanel.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                children.Append(m_aboutPanel);
                SetBackButtonUsable(false);
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
            std::wstring catMon = g_Loc.Get(L"Nav_Monitors", L"Monitors").c_str();
            std::wstring catTim = g_Loc.Get(L"Nav_Timings", L"Timings & Global").c_str();

            return {
                // General
                { g_Loc.Get(L"Opt_Language", L"Language").c_str(), catGen, L"language locale english translate", L"General", LanguageComboBox() },
                { g_Loc.Get(L"Opt_AppTheme", L"App Theme").c_str(), catGen, L"theme dark light windows appearance", L"General", AppThemeComboBox() },
                { g_Loc.Get(L"Opt_TransparencyEffects", L"Transparency Effects").c_str(), catGen, L"transparency mica acrylic blur solid opaque backdrop", L"General", TransparencyEffectsToggle() },
                { g_Loc.Get(L"Opt_StartWithWindows", L"Start with Windows").c_str(), catGen, L"startup boot autostart logon windows", L"General", StartWithWindowsToggle() },
                { g_Loc.Get(L"Opt_PhysicalMonitors", L"Physical Monitors").c_str(), catGen, L"detect redetect hardware ddc/ci driver", L"General", RedetectMonitorsButton() },
                { g_Loc.Get(L"Opt_ResetDefaults", L"Reset Settings").c_str(), catGen, L"reset defaults restore factory", L"General", ResetDefaultsButton() },

                // Monitors & Per-Monitor Controls
                { g_Loc.Get(L"Opt_MainMonitor", L"Main Display").c_str(), catMon, L"main display primary monitor reference sync", L"Monitors", MainMonitorComboBox() },
                { g_Loc.Get(L"Opt_SyncLaptopFnKeys", L"Sync External Monitors with Laptop Fn Keys").c_str(), catMon, L"laptop fn keys internal display sync external", L"Monitors", SyncLaptopFnKeysToggle() },
                { g_Loc.Get(L"Opt_ShowSeparateInMultiFlyout", L"Show Separate Monitors in Multi-Monitor Flyout").c_str(), catMon, L"separate monitors multi monitor flyout show hide sliders", L"Monitors", ShowSeparateInMultiFlyoutToggle() },
                { g_Loc.Get(L"Opt_DisableTurnedOffControl", L"Disable Control for Turned-Off Monitors").c_str(), catMon, L"disable turned off monitor powered off standby brightness control", L"Monitors", DisableTurnedOffControlToggle() },
                { g_Loc.Get(L"Opt_SyncOverscrollMax", L"Overscroll when a display is at 100%").c_str(), catMon, L"overscroll 100 max sync offset", L"Monitors", SyncOverscrollMaxToggle() },
                { g_Loc.Get(L"Opt_SyncOverscrollMin", L"Overscroll when a display is at 0%").c_str(), catMon, L"overscroll 0 min sync offset", L"Monitors", SyncOverscrollMinToggle() },
                { g_Loc.Get(L"Opt_ConfigMonitor", L"Selected Display").c_str(), catMon, L"monitor display select configure", L"Monitors", ConfigMonitorComboBox() },
                { g_Loc.Get(L"Opt_MonitorEnabled", L"Brightness Control").c_str(), catMon, L"enable disable monitor brightness control", L"Monitors", MonitorEnabledToggle() },
                { g_Loc.Get(L"Opt_MonitorCustomName", L"Monitor Name").c_str(), catMon, L"monitor name label rename osd flyout", L"Monitors", MonitorCustomNameTextBox() },
                { g_Loc.Get(L"Opt_MonitorControlMode", L"Control Mode").c_str(), catMon, L"control mode sync separate independent", L"Monitors", MonitorControlModeComboBox() },
                { g_Loc.Get(L"Opt_MonitorOffset", L"Brightness Offset (%)").c_str(), catMon, L"offset relative sync main display", L"Monitors", MonitorOffsetSlider() },

                // Shortcuts (under Monitors)
                { g_Loc.Get(L"Opt_HotkeyAdjust", L"Increase & Decrease Shortcuts").c_str(), catMon, L"enable disable stepping shortcuts hotkeys", L"Monitors_Shortcuts", HotkeyAdjustToggle() },
                { g_Loc.Get(L"Opt_KeyUp", L"Increase Brightness").c_str(), catMon, L"hotkey shortcut up raise f14", L"Monitors_Shortcuts", KeyUpButton() },
                { g_Loc.Get(L"Opt_KeyDown", L"Decrease Brightness").c_str(), catMon, L"hotkey shortcut down lower f13", L"Monitors_Shortcuts", KeyDownButton() },
                { g_Loc.Get(L"Opt_Step", L"Brightness Step").c_str(), catMon, L"step percentage increment shortcut", L"Monitors_Shortcuts", StepComboBox() },
                { g_Loc.Get(L"Opt_HotkeyAdjustShowOsd", L"Show OSD Banner").c_str(), catMon, L"show osd banner stepping hotkeys", L"Monitors_Shortcuts", HotkeyAdjustShowOsdToggle() },
                { g_Loc.Get(L"Opt_HotkeyPresets", L"Preset Shortcuts").c_str(), catMon, L"enable disable preset shortcuts hotkeys", L"Monitors_Shortcuts", HotkeyPresetsToggle() },
                { g_Loc.Get(L"Opt_KeyPreset1", L"Preset 1 Shortcut").c_str(), catMon, L"preset 1 shortcut hotkey f15", L"Monitors_Shortcuts", KeyPreset1Button() },
                { g_Loc.Get(L"Opt_Preset1Brightness", L"Preset 1 Brightness (%)").c_str(), catMon, L"preset 1 brightness target level", L"Monitors_Shortcuts", Preset1BrightnessSlider() },
                { g_Loc.Get(L"Opt_KeyPreset2", L"Preset 2 Shortcut").c_str(), catMon, L"preset 2 shortcut hotkey f16", L"Monitors_Shortcuts", KeyPreset2Button() },
                { g_Loc.Get(L"Opt_Preset2Brightness", L"Preset 2 Brightness (%)").c_str(), catMon, L"preset 2 brightness target level", L"Monitors_Shortcuts", Preset2BrightnessSlider() },
                { g_Loc.Get(L"Opt_HotkeyPresetsShowOsd", L"Show OSD Banner").c_str(), catMon, L"show osd banner preset shortcuts", L"Monitors_Shortcuts", HotkeyPresetsShowOsdToggle() },
                { g_Loc.Get(L"Opt_HotkeyPresetToggle", L"Preset Toggle Shortcut").c_str(), catMon, L"enable disable preset toggle shortcut hotkey f17", L"Monitors_Shortcuts", HotkeyPresetToggleToggle() },
                { g_Loc.Get(L"Opt_KeyPresetToggle", L"Preset Toggle Shortcut").c_str(), catMon, L"preset toggle shortcut hotkey f17", L"Monitors_Shortcuts", KeyPresetToggleButton() },
                { g_Loc.Get(L"Opt_PresetToggle1Brightness", L"Preset 1 Brightness (%)").c_str(), catMon, L"preset toggle 1 brightness target level", L"Monitors_Shortcuts", PresetToggle1BrightnessSlider() },
                { g_Loc.Get(L"Opt_PresetToggle2Brightness", L"Preset 2 Brightness (%)").c_str(), catMon, L"preset toggle 2 brightness target level", L"Monitors_Shortcuts", PresetToggle2BrightnessSlider() },
                { g_Loc.Get(L"Opt_HotkeyPresetToggleShowOsd", L"Show OSD Banner").c_str(), catMon, L"show osd banner preset toggle shortcut", L"Monitors_Shortcuts", HotkeyPresetToggleShowOsdToggle() },
                { g_Loc.Get(L"Opt_ResetShortcuts", L"Reset Shortcuts").c_str(), catMon, L"reset hotkeys shortcuts", L"Monitors_Shortcuts", ResetHotkeysButton() },

                // OSD Accent (under Monitors)
                { g_Loc.Get(L"Opt_OverlayUseWindowsAccent", L"Use Windows Accent Color").c_str(), catMon, L"osd overlay accent windows color", L"Monitors_OSD", OverlayUseWindowsAccentToggle() },
                { g_Loc.Get(L"Opt_OverlayCustomAccent", L"Custom Accent Color").c_str(), catMon, L"osd overlay custom accent color picker", L"Monitors_OSD", OverlayAccentColorPickerButton() },

                // System Tray (under Monitors)
                { g_Loc.Get(L"Opt_TrayShowPercentage", L"Tray Icon").c_str(), catMon, L"tray icon percentage show disable colored hide follow windows font number live", L"Monitors_Tray", TrayPercentageModeComboBox() },
                { g_Loc.Get(L"Opt_TrayIconColor", L"Icon Color").c_str(), catMon, L"tray icon percentage custom color picker light mode", L"Monitors_Tray", TrayIconColorPickerButton() },
                { g_Loc.Get(L"Opt_TrayIconDualColor", L"Separate Dark & Light Colors").c_str(), catMon, L"tray icon percentage dual separate dark light color", L"Monitors_Tray", TrayIconDualColorToggle() },
                { g_Loc.Get(L"Opt_TrayIconColorDark", L"Dark Mode Color").c_str(), catMon, L"tray icon percentage custom dark mode color picker", L"Monitors_Tray", TrayIconDarkColorPickerButton() },
                { g_Loc.Get(L"Opt_TrayCustomIcon", L"Custom Icon").c_str(), catMon, L"tray custom icon preset sun browse ico", L"Monitors_Tray", TrayCustomIconPresetComboBox() },
                { g_Loc.Get(L"Opt_TrayIconDualCustomIcons", L"Separate Dark & Light Icons").c_str(), catMon, L"tray custom icon dual separate dark light mode", L"Monitors_Tray", TrayIconDualCustomIconToggle() },
                { g_Loc.Get(L"Opt_TrayCustomIconDark", L"Dark Mode Icon").c_str(), catMon, L"tray custom dark mode icon preset sun blue browse ico", L"Monitors_Tray", TrayCustomIconDarkPresetComboBox() },
                { g_Loc.Get(L"Opt_TrayFont", L"Tray Icon Font").c_str(), catMon, L"tray font family typography", L"Monitors_Tray", TrayFontComboBox() },
                { g_Loc.Get(L"Opt_TrayFontWeight", L"Font Weight").c_str(), catMon, L"tray font weight bold light semilight thin", L"Monitors_Tray", TrayFontWeightSlider() },
                { g_Loc.Get(L"Opt_TrayFontSize", L"Tray Font Size (%)").c_str(), catMon, L"tray font size scale", L"Monitors_Tray", TrayFontSizeSlider() },
                { g_Loc.Get(L"Opt_TrayFontSize100", L"100% Width (%)").c_str(), catMon, L"tray 100 width scale", L"Monitors_Tray", TrayFontSize100Slider() },
                { g_Loc.Get(L"Opt_TrayYOffset", L"Vertical Offset (px)").c_str(), catMon, L"tray vertical offset y baseline", L"Monitors_Tray", TrayYOffsetSlider() },
                { g_Loc.Get(L"Opt_TrayWheel", L"Scroll on Tray Icon").c_str(), catMon, L"tray scroll wheel mouse", L"Monitors_Tray", TrayWheelToggle() },
                { g_Loc.Get(L"Opt_TrayWheelStep", L"Brightness Step").c_str(), catMon, L"tray scroll wheel step percentage", L"Monitors_Tray", TrayWheelStepComboBox() },
                { g_Loc.Get(L"Opt_TrayWheelShowOsd", L"Show OSD When Scrolling").c_str(), catMon, L"tray scroll osd banner", L"Monitors_Tray", TrayWheelShowOsdToggle() },
                { g_Loc.Get(L"Opt_TrayMiddleClick", L"Middle-Click Quick Action").c_str(), catMon, L"tray middle click quick action", L"Monitors_Tray", TrayMiddleClickToggle() },
                { g_Loc.Get(L"Opt_TrayMiddleClickShowOsd", L"Show OSD On Middle-Click").c_str(), catMon, L"tray middle click osd banner", L"Monitors_Tray", TrayMiddleClickShowOsdToggle() },
                { g_Loc.Get(L"Opt_TrayMiddleClickBrightness", L"Target Brightness (%)").c_str(), catMon, L"tray middle click brightness target", L"Monitors_Tray", TrayMiddleClickSlider() },

                // Flyout Slider (under Monitors)
                { g_Loc.Get(L"Opt_FlyoutScrollEnabled", L"Slider Scrolling").c_str(), catMon, L"flyout slider scroll wheel mouse enable", L"Monitors_Flyout", FlyoutScrollEnabledToggle() },
                { g_Loc.Get(L"Opt_FlyoutScrollStep", L"Scrolling Brightness Step").c_str(), catMon, L"flyout slider scroll step percentage", L"Monitors_Flyout", FlyoutScrollStepComboBox() },
                { g_Loc.Get(L"Opt_FlyoutUseWindowsAccent", L"Use Windows Accent Color").c_str(), catMon, L"flyout accent windows color", L"Monitors_Flyout", FlyoutUseWindowsAccentToggle() },
                { g_Loc.Get(L"Opt_FlyoutCustomAccent", L"Custom Accent Color").c_str(), catMon, L"flyout custom accent color picker", L"Monitors_Flyout", FlyoutAccentColorPickerButton() },

                // Schedule (under Monitors)
                { g_Loc.Get(L"Opt_ScheduleMode", L"Automatic Schedule").c_str(), catMon, L"schedule automatic day night custom time sunrise sunset", L"Monitors_Schedule", ScheduleModeComboBox() },
                { g_Loc.Get(L"Opt_ScheduleDayBrightness", L"Daytime Brightness (%)").c_str(), catMon, L"schedule day brightness level", L"Monitors_Schedule", ScheduleDayBrightnessSlider() },
                { g_Loc.Get(L"Opt_ScheduleNightBrightness", L"Nighttime Brightness (%)").c_str(), catMon, L"schedule night brightness level", L"Monitors_Schedule", ScheduleNightBrightnessSlider() },

                // Timings & Global
                { g_Loc.Get(L"Opt_DelayEnabled", L"Delayed Brightness Change").c_str(), catTim, L"delay debounce shortcut", L"Timings_Shortcuts", DelayEnabledToggle() },
                { g_Loc.Get(L"Opt_WaitTime", L"Wait Time (ms)").c_str(), catTim, L"wait time delay ms shortcut", L"Timings_Shortcuts", DelaySlider() },
                { g_Loc.Get(L"Opt_HoldSpeed", L"Hold Speed (ms)").c_str(), catTim, L"hold speed repeat interval shortcut", L"Timings_Shortcuts", RepeatSlider() },
                { g_Loc.Get(L"Opt_OverlayTheme", L"OSD Banner Theme").c_str(), catTim, L"osd overlay banner theme dark light", L"Timings_OSD", OverlayThemeComboBox() },
                { g_Loc.Get(L"Opt_OverlayTimeout", L"Banner Duration (ms)").c_str(), catTim, L"osd overlay timeout duration hide ms", L"Timings_OSD", OverlayTimeoutSlider() },
                { g_Loc.Get(L"Opt_TrayWheelDelay", L"Scrolling Hardware Delay").c_str(), catTim, L"tray scroll wheel delay debounce", L"Timings_Tray", TrayWheelDelayToggle() },
                { g_Loc.Get(L"Opt_TrayWheelWaitTime", L"Scrolling Wait Time (ms)").c_str(), catTim, L"tray scroll wheel wait time ms", L"Timings_Tray", TrayWheelDelaySlider() },
                { g_Loc.Get(L"Opt_FlyoutKeyboardEnabled", L"Keyboard Controls").c_str(), catTim, L"flyout keyboard arrow keys buttons navigation", L"Timings_Flyout", FlyoutKeyboardEnabledToggle() },
                { g_Loc.Get(L"Opt_FlyoutScrollDelay", L"Slider Scrolling Delay").c_str(), catTim, L"flyout slider scroll delay debounce", L"Timings_Flyout", FlyoutScrollDelayToggle() },
                { g_Loc.Get(L"Opt_FlyoutScrollWaitTime", L"Scrolling Wait Time (ms)").c_str(), catTim, L"flyout slider scroll wait time ms", L"Timings_Flyout", FlyoutScrollDelaySlider() },
                { g_Loc.Get(L"Opt_FlyoutButtonDelay", L"Button Clicking Delay").c_str(), catTim, L"flyout button click delay debounce", L"Timings_Flyout", FlyoutButtonDelayToggle() },
                { g_Loc.Get(L"Opt_FlyoutButtonWaitTime", L"Button Wait Time (ms)").c_str(), catTim, L"flyout button wait time ms", L"Timings_Flyout", FlyoutButtonDelaySlider() },

                { g_Loc.Get(L"About_Title", L"About ClickBrightness").c_str(), g_Loc.Get(L"Nav_About", L"About").c_str(), L"about version info license github open source", L"About", AboutPanel() }
            };
        };

        auto navigateToEntry = [this](const SettingSearchEntry& entry)
        {
            if (entry.navTag == L"General") NavView().SelectedItem(NavItemGeneral());
            else if (entry.navTag == L"Monitors") { EnsureTrayTabLoaded(); NavView().SelectedItem(NavItemMonitors()); OpenMonitorsSubpage(L""); }
            else if (entry.navTag == L"Monitors_Shortcuts") { EnsureTrayTabLoaded(); NavView().SelectedItem(NavItemMonitors()); OpenMonitorsSubpage(L"Shortcuts"); }
            else if (entry.navTag == L"Monitors_OSD") { EnsureTrayTabLoaded(); NavView().SelectedItem(NavItemMonitors()); OpenMonitorsSubpage(L"OSD"); }
            else if (entry.navTag == L"Monitors_Tray") { EnsureTrayTabLoaded(); NavView().SelectedItem(NavItemMonitors()); OpenMonitorsSubpage(L"Tray"); }
            else if (entry.navTag == L"Monitors_Flyout") { EnsureTrayTabLoaded(); NavView().SelectedItem(NavItemMonitors()); OpenMonitorsSubpage(L"Flyout"); }
            else if (entry.navTag == L"Monitors_Schedule") { EnsureTrayTabLoaded(); NavView().SelectedItem(NavItemMonitors()); OpenMonitorsSubpage(L"Schedule"); }
            else if (entry.navTag == L"Timings") { NavView().SelectedItem(NavItemTimings()); OpenTimingsSubpage(L""); }
            else if (entry.navTag == L"Timings_Shortcuts") { NavView().SelectedItem(NavItemTimings()); OpenTimingsSubpage(L"Shortcuts"); }
            else if (entry.navTag == L"Timings_OSD") { NavView().SelectedItem(NavItemTimings()); OpenTimingsSubpage(L"OSD"); }
            else if (entry.navTag == L"Timings_Tray") { NavView().SelectedItem(NavItemTimings()); OpenTimingsSubpage(L"Tray"); }
            else if (entry.navTag == L"Timings_Flyout") { NavView().SelectedItem(NavItemTimings()); OpenTimingsSubpage(L"Flyout"); }
            else if (entry.navTag == L"About") { EnsureAboutTabLoaded(); NavView().SelectedItem(NavItemAbout()); }

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

        // Monitors Submenu Navigation Cards & Breadcrumb Back
        MonitorsShortcutsCard().Click([this](auto&&, auto&&)
        {
            OpenMonitorsSubpage(L"Shortcuts");
        });
        MonitorsOsdCard().Click([this](auto&&, auto&&)
        {
            OpenMonitorsSubpage(L"OSD");
        });
        MonitorsTrayCard().Click([this](auto&&, auto&&)
        {
            OpenMonitorsSubpage(L"Tray");
        });
        MonitorsFlyoutCard().Click([this](auto&&, auto&&)
        {
            OpenMonitorsSubpage(L"Flyout");
        });
        MonitorsScheduleCard().Click([this](auto&&, auto&&)
        {
            OpenMonitorsSubpage(L"Schedule");
        });
        TitleBarBackButton().Click([this](auto&&, auto&&)
        {
            if (!m_currentMonitorsSubpage.empty()) OpenMonitorsSubpage(L"");
            else if (!m_currentTimingsSubpage.empty()) OpenTimingsSubpage(L"");
        });
        MonitorsBreadcrumbRootButton().Click([this](auto&&, auto&&)
        {
            OpenMonitorsSubpage(L"");
        });
        TimingsShortcutsCard().Click([this](auto&&, auto&&)
        {
            OpenTimingsSubpage(L"Shortcuts");
        });
        TimingsFlyoutCard().Click([this](auto&&, auto&&)
        {
            OpenTimingsSubpage(L"Flyout");
        });
        TimingsOsdCard().Click([this](auto&&, auto&&)
        {
            OpenTimingsSubpage(L"OSD");
        });
        TimingsTrayCard().Click([this](auto&&, auto&&)
        {
            OpenTimingsSubpage(L"Tray");
        });
        TimingsBreadcrumbRootButton().Click([this](auto&&, auto&&)
        {
            OpenTimingsSubpage(L"");
        });

        // Main Display ComboBox
        MainMonitorComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int idx = MainMonitorComboBox().SelectedIndex();
            if (idx >= 0 && idx < static_cast<int>(m_cachedMonitorIds.size()))
            {
                g_Settings.mainMonitorId = m_cachedMonitorIds[idx];
                if (g_Settings.FindMonitorConfig(g_Settings.mainMonitorId))
                {
                    auto& mainCfg = g_Settings.GetOrCreateMonitorConfig(g_Settings.mainMonitorId);
                    mainCfg.enabled = true;
                    mainCfg.controlMode = 0;
                    if (mainCfg.trayPercentageMode == 3)
                    {
                        mainCfg.trayPercentageMode = 0; // Follow Windows
                    }
                }
                g_MonitorManager.UpdateFnSyncListener();
                PopulateMonitorsSelectionList();
                LoadMonitorConfigToUI();
                ApplyAndSave();
            }
        });

        // Sync External Monitors with Laptop Fn Keys
        SyncLaptopFnKeysToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = SyncLaptopFnKeysToggle().IsOn();
            g_Settings.syncExternalWithLaptopFnKeys = on;
            SyncLaptopFnKeysStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            g_MonitorManager.UpdateFnSyncListener();
            ApplyAndSave();
        });

        // Show Separate Monitors in Multi-Monitor Flyout
        ShowSeparateInMultiFlyoutToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = ShowSeparateInMultiFlyoutToggle().IsOn();
            g_Settings.showSeparateInMultiFlyout = on;
            ShowSeparateInMultiFlyoutStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            ApplyAndSave();
            g_FlyoutWindow.RefreshOnDisplayChange();
        });

        // Disable Control for Turned-Off Monitors
        DisableTurnedOffControlToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = DisableTurnedOffControlToggle().IsOn();
            g_Settings.disableTurnedOffControl = on;
            DisableTurnedOffControlStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            ApplyAndSave();
            if (on)
            {
                g_MonitorManager.RefreshExternalPowerStatesAsync();
            }
            g_TrayIcon.UpdateBrightness(g_MonitorManager.GetCachedBrightness());
            g_TrayIcon.SyncExtraTrayIcons(true);
            g_FlyoutWindow.RefreshOnDisplayChange();
        });

        // Config Monitor Selector ComboBox
        ConfigMonitorComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int idx = ConfigMonitorComboBox().SelectedIndex();
            if (idx >= 0 && idx < static_cast<int>(m_cachedMonitorIds.size()))
            {
                SaveCurrentMonitorConfigFromUI();
                m_selectedConfigMonitorId = m_cachedMonitorIds[idx];
                LoadMonitorConfigToUI();
            }
        });

        // Per-Monitor Enabled Toggle
        MonitorEnabledToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI || m_selectedConfigMonitorId.empty()) return;
            auto& cfg = g_Settings.GetOrCreateMonitorConfig(m_selectedConfigMonitorId);
            cfg.enabled = MonitorEnabledToggle().IsOn();
            MonitorEnabledStatusText().Text(cfg.enabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            ApplyAndSave();
        });

        // Per-Monitor Custom Name TextBox
        MonitorCustomNameTextBox().TextChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI || m_selectedConfigMonitorId.empty()) return;
            auto& cfg = g_Settings.GetOrCreateMonitorConfig(m_selectedConfigMonitorId);
            cfg.customName = AppSettings::SanitizeMonitorName(MonitorCustomNameTextBox().Text().c_str(), 20);
            ScheduleSave(false, true);
        });

        // Per-Monitor Control Mode ComboBox
        MonitorControlModeComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI || m_selectedConfigMonitorId.empty()) return;
            auto& cfg = g_Settings.GetOrCreateMonitorConfig(m_selectedConfigMonitorId);
            int prevMode = cfg.controlMode;
            int idx = MonitorControlModeComboBox().SelectedIndex();
            cfg.controlMode = (idx == 1) ? 1 : 0;
            if (prevMode != 1 && cfg.controlMode == 1)
            {
                if (cfg.trayPercentageMode == 3)
                {
                    cfg.trayPercentageMode = 0; // Follow Windows
                }
                for (const auto& kv : g_Settings.monitorConfigs)
                {
                    if (_wcsicmp(kv.first.c_str(), m_selectedConfigMonitorId.c_str()) == 0) continue;
                    AppSettings::ClearMatchingHotkey(cfg, kv.second.hotkeyUpKey, kv.second.hotkeyUpMods);
                    AppSettings::ClearMatchingHotkey(cfg, kv.second.hotkeyDownKey, kv.second.hotkeyDownMods);
                    AppSettings::ClearMatchingHotkey(cfg, kv.second.hotkeyPreset1Key, kv.second.hotkeyPreset1Mods);
                    AppSettings::ClearMatchingHotkey(cfg, kv.second.hotkeyPreset2Key, kv.second.hotkeyPreset2Mods);
                    AppSettings::ClearMatchingHotkey(cfg, kv.second.hotkeyPresetToggleKey, kv.second.hotkeyPresetToggleMods);
                }
            }
            LoadMonitorConfigToUI();
            ApplyAndSave();
        });

        // Per-Monitor Offset Slider & NumberBox
        MonitorOffsetSlider().ValueChanged([this](auto&&, auto&& e)
        {
            if (m_isUpdatingUI || m_selectedConfigMonitorId.empty()) return;
            int val = static_cast<int>(std::round(e.NewValue()));
            val = (std::clamp)(val, -100, 100);
            g_Settings.GetOrCreateMonitorConfig(m_selectedConfigMonitorId).offset = val;
            m_isUpdatingUI = true;
            MonitorOffsetNumberBox().Value(val);
            m_isUpdatingUI = false;
            ScheduleSave(false, true);
        });

        MonitorOffsetNumberBox().ValueChanged([this](auto&&, auto&& e)
        {
            if (m_isUpdatingUI || m_selectedConfigMonitorId.empty()) return;
            if (std::isnan(e.NewValue())) return;
            int val = static_cast<int>(std::round(e.NewValue()));
            val = (std::clamp)(val, -100, 100);
            g_Settings.GetOrCreateMonitorConfig(m_selectedConfigMonitorId).offset = val;
            m_isUpdatingUI = true;
            MonitorOffsetSlider().Value(val);
            m_isUpdatingUI = false;
            ScheduleSave(false, true);
        });

        // Sync Overscroll Max (100%)
        SyncOverscrollMaxToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI || m_selectedConfigMonitorId.empty()) return;
            bool on = SyncOverscrollMaxToggle().IsOn();
            g_Settings.GetOrCreateMonitorConfig(m_selectedConfigMonitorId).syncOverscrollMax = on;
            SyncOverscrollMaxStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            ApplyAndSave();
        });

        // Sync Overscroll Min (0%)
        SyncOverscrollMinToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI || m_selectedConfigMonitorId.empty()) return;
            bool on = SyncOverscrollMinToggle().IsOn();
            g_Settings.GetOrCreateMonitorConfig(m_selectedConfigMonitorId).syncOverscrollMin = on;
            SyncOverscrollMinStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
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
                g_OverlayWindow.OnThemeChanged();
                g_FlyoutWindow.OnThemeChanged();
                g_TrayIcon.OnThemeChanged();
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

        // Preset Toggle Shortcut Toggle
        HotkeyPresetToggleToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = HotkeyPresetToggleToggle().IsOn();
            g_Settings.hotkeyPresetToggleEnabled = on;
            HotkeyPresetToggleStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            PresetToggleKeyCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            PresetToggle1BrightnessCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            PresetToggle2BrightnessCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            HotkeyPresetToggleShowOsdCard().Visibility(on ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ApplyAndSave();
        });

        // Preset Toggle Shortcut Key Button
        KeyPresetToggleButton().Click([this](auto&&, auto&&)
        {
            StartCapture(5);
        });

        // Preset Toggle 1 Brightness Slider & NumberBox sync
        PresetToggle1BrightnessSlider().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(std::round(e.NewValue()));
            val = (std::clamp)(val, 0, 100);
            g_Settings.hotkeyPresetToggle1Brightness = val;
            m_isUpdatingUI = true;
            PresetToggle1BrightnessNumberBox().Value(val);
            m_isUpdatingUI = false;
            ApplyAndSave();
        });

        PresetToggle1BrightnessNumberBox().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            double v = e.NewValue();
            if (std::isnan(v)) return;
            int val = static_cast<int>(std::round(v));
            val = (std::clamp)(val, 0, 100);
            g_Settings.hotkeyPresetToggle1Brightness = val;
            m_isUpdatingUI = true;
            PresetToggle1BrightnessSlider().Value(val);
            m_isUpdatingUI = false;
            ApplyAndSave();
        });

        // Preset Toggle 2 Brightness Slider & NumberBox sync
        PresetToggle2BrightnessSlider().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(std::round(e.NewValue()));
            val = (std::clamp)(val, 0, 100);
            g_Settings.hotkeyPresetToggle2Brightness = val;
            m_isUpdatingUI = true;
            PresetToggle2BrightnessNumberBox().Value(val);
            m_isUpdatingUI = false;
            ApplyAndSave();
        });

        PresetToggle2BrightnessNumberBox().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            double v = e.NewValue();
            if (std::isnan(v)) return;
            int val = static_cast<int>(std::round(v));
            val = (std::clamp)(val, 0, 100);
            g_Settings.hotkeyPresetToggle2Brightness = val;
            m_isUpdatingUI = true;
            PresetToggle2BrightnessSlider().Value(val);
            m_isUpdatingUI = false;
            ApplyAndSave();
        });

        // Preset Toggle Show OSD Toggle
        HotkeyPresetToggleShowOsdToggle().Toggled([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            bool on = HotkeyPresetToggleShowOsdToggle().IsOn();
            g_Settings.hotkeyPresetToggleShowOsd = on;
            HotkeyPresetToggleShowOsdStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
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
            bool isSeparateNonMain = !m_selectedConfigMonitorId.empty() &&
                                     !g_Settings.mainMonitorId.empty() &&
                                     _wcsicmp(m_selectedConfigMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) != 0;
            if (isSeparateNonMain)
            {
                m_curUpKey = 0; m_curUpMods = 0;
                m_curDownKey = 0; m_curDownMods = 0;
                m_curPreset1Key = 0; m_curPreset1Mods = 0;
                m_curPreset2Key = 0; m_curPreset2Mods = 0;
                m_curPresetToggleKey = 0; m_curPresetToggleMods = 0;
            }
            else
            {
                m_curUpKey = 0x7D; m_curUpMods = 0;         // VK_F14
                m_curDownKey = 0x7C; m_curDownMods = 0;     // VK_F13
                m_curPreset1Key = 0x7E; m_curPreset1Mods = 0; // VK_F15
                m_curPreset2Key = 0x7F; m_curPreset2Mods = 0; // VK_F16
                m_curPresetToggleKey = 0x80; m_curPresetToggleMods = 0; // VK_F17
                const UINT defaultKeys[] = { m_curUpKey, m_curDownKey, m_curPreset1Key, m_curPreset2Key, m_curPresetToggleKey };
                for (auto& kv : g_Settings.monitorConfigs)
                {
                    if (_wcsicmp(kv.first.c_str(), m_selectedConfigMonitorId.c_str()) == 0) continue;
                    for (UINT dk : defaultKeys)
                    {
                        AppSettings::ClearMatchingHotkey(kv.second, dk, 0);
                    }
                }
            }
            g_Settings.hotkeyUpKey = m_curUpKey;
            g_Settings.hotkeyUpMods = m_curUpMods;
            g_Settings.hotkeyDownKey = m_curDownKey;
            g_Settings.hotkeyDownMods = m_curDownMods;
            g_Settings.hotkeyPreset1Key = m_curPreset1Key;
            g_Settings.hotkeyPreset1Mods = m_curPreset1Mods;
            g_Settings.hotkeyPreset2Key = m_curPreset2Key;
            g_Settings.hotkeyPreset2Mods = m_curPreset2Mods;
            g_Settings.hotkeyPresetToggleKey = m_curPresetToggleKey;
            g_Settings.hotkeyPresetToggleMods = m_curPresetToggleMods;
            g_Settings.hotkeyPresetToggle1Brightness = 30;
            g_Settings.hotkeyPresetToggle2Brightness = 80;
            g_Settings.hotkeyAdjustShowOsd = true;
            g_Settings.hotkeyPresetsShowOsd = true;
            g_Settings.hotkeyPresetToggleShowOsd = true;
            HotkeyAdjustShowOsdToggle().IsOn(true);
            HotkeyAdjustShowOsdStatusText().Text(g_Loc.Get(L"Common_On", L"On"));
            HotkeyPresetsShowOsdToggle().IsOn(true);
            HotkeyPresetsShowOsdStatusText().Text(g_Loc.Get(L"Common_On", L"On"));
            HotkeyPresetToggleShowOsdToggle().IsOn(true);
            HotkeyPresetToggleShowOsdStatusText().Text(g_Loc.Get(L"Common_On", L"On"));
            PresetToggle1BrightnessSlider().Value(30);
            PresetToggle1BrightnessNumberBox().Value(30);
            PresetToggle2BrightnessSlider().Value(80);
            PresetToggle2BrightnessNumberBox().Value(80);
            KeyUpButton().Content(box_value(HotkeyManager::FormatHotkey(m_curUpMods, m_curUpKey)));
            KeyDownButton().Content(box_value(HotkeyManager::FormatHotkey(m_curDownMods, m_curDownKey)));
            KeyPreset1Button().Content(box_value(HotkeyManager::FormatHotkey(m_curPreset1Mods, m_curPreset1Key)));
            KeyPreset2Button().Content(box_value(HotkeyManager::FormatHotkey(m_curPreset2Mods, m_curPreset2Key)));
            KeyPresetToggleButton().Content(box_value(HotkeyManager::FormatHotkey(m_curPresetToggleMods, m_curPresetToggleKey)));
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
                ApplyAndSave();
                g_OverlayWindow.OnThemeChanged();
                g_FlyoutWindow.OnThemeChanged();
                g_TrayIcon.OnThemeChanged();
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

        // Overlay Use Windows Accent Color & Dual Colors Toggle
        auto updateOverlayAccentUI = [this](bool useWinAccent)
        {
            auto vis = useWinAccent ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible;
            OverlayAccentColorCard().Visibility(vis);
            OverlayDualColorCard().Visibility(vis);
            OverlayDarkColorCard().Visibility((!useWinAccent && g_Settings.overlayUseDualColors) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            OverlayCustomAccentTitleText().Text(g_Settings.overlayUseDualColors
                ? g_Loc.Get(L"Opt_TrayIconColorLight", L"Light Mode Color")
                : g_Loc.Get(L"Opt_OverlayCustomAccent", L"Custom Accent Color"));
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

        OverlayDualColorToggle().Toggled([this, updateOverlayAccentUI](auto&&, auto&&)
        {
            bool on = OverlayDualColorToggle().IsOn();
            OverlayDualColorStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            if (m_isUpdatingUI) return;
            g_Settings.overlayUseDualColors = on;
            updateOverlayAccentUI(g_Settings.overlayUseWindowsAccent);
            ApplyAndSave();
            g_OverlayWindow.OnThemeChanged();
        });

        // Tray Percentage Icon Mode ComboBox (0 = Follow Windows, 1 = Colored, 2 = Disabled, 3 = Hide)
        TrayPercentageModeComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int idx = TrayPercentageModeComboBox().SelectedIndex();
            bool isMain = (_wcsicmp(m_selectedConfigMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
            if (isMain && idx == 3)
            {
                idx = 0;
                m_isUpdatingUI = true;
                TrayPercentageModeComboBox().SelectedIndex(0);
                m_isUpdatingUI = false;
            }
            if (idx >= 0 && idx <= 4)
            {
                g_Settings.trayPercentageMode = idx;
                TrayIconColorCard().Visibility(idx == 1 ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                TrayIconDualColorCard().Visibility(idx == 1 ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                TrayIconDarkColorCard().Visibility((idx == 1 && g_Settings.trayIconUseDualColors) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                TrayCustomIconCard().Visibility(idx == 4 ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                TrayIconDualCustomIconCard().Visibility(idx == 4 ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                TrayCustomIconDarkCard().Visibility((idx == 4 && g_Settings.trayIconUseDualCustomIcons) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                TrayTypographyPanel().Visibility((idx == 0 || idx == 1) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                ApplyAndSave();
                g_TrayIcon.OnThemeChanged();
            }
        });

        // Tray Icon Separate Dark & Light Colors Toggle
        TrayIconDualColorToggle().Toggled([this](auto&&, auto&&)
        {
            bool on = TrayIconDualColorToggle().IsOn();
            TrayIconDualColorStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            TrayIconColorTitleText().Text(on ? g_Loc.Get(L"Opt_TrayIconColorLight", L"Light Mode Color") : g_Loc.Get(L"Opt_TrayIconColor", L"Icon Color"));
            if (m_isUpdatingUI) return;
            g_Settings.trayIconUseDualColors = on;
            TrayIconDarkColorCard().Visibility((g_Settings.trayPercentageMode == 1 && on) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ApplyAndSave();
            g_TrayIcon.OnThemeChanged();
        });

        // Tray Custom Icon Presets & Browse
        PopulateSunPresetComboBox(TrayCustomIconPresetComboBox());
        PopulateSunPresetComboBox(TrayCustomIconDarkPresetComboBox());

        TrayCustomIconPresetComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int idx = TrayCustomIconPresetComboBox().SelectedIndex();
            int presetCount = static_cast<int>(std::size(kSunIconPresets));
            if (idx >= 0 && idx < presetCount)
            {
                g_Settings.trayCustomIcon = kSunIconPresets[idx].spec;
                UpdateCustomIconPreview(TrayCustomIconPreview(), g_Settings.trayCustomIcon);
                ApplyAndSave();
                g_TrayIcon.OnThemeChanged();
            }
            else if (idx == presetCount)
            {
                std::wstring chosen = BrowseForIcoFile(m_hwnd);
                if (!chosen.empty())
                {
                    g_Settings.trayCustomIcon = chosen;
                    UpdateCustomIconPreview(TrayCustomIconPreview(), g_Settings.trayCustomIcon);
                    ApplyAndSave();
                    g_TrayIcon.OnThemeChanged();
                }
                else
                {
                    m_isUpdatingUI = true;
                    SelectSunPresetInComboBox(TrayCustomIconPresetComboBox(), g_Settings.trayCustomIcon);
                    m_isUpdatingUI = false;
                }
            }
        });

        TrayCustomIconBrowseButton().Click([this](auto&&, auto&&)
        {
            std::wstring chosen = BrowseForIcoFile(m_hwnd);
            if (!chosen.empty())
            {
                g_Settings.trayCustomIcon = chosen;
                m_isUpdatingUI = true;
                SelectSunPresetInComboBox(TrayCustomIconPresetComboBox(), g_Settings.trayCustomIcon);
                m_isUpdatingUI = false;
                UpdateCustomIconPreview(TrayCustomIconPreview(), g_Settings.trayCustomIcon);
                ApplyAndSave();
                g_TrayIcon.OnThemeChanged();
            }
        });

        TrayIconDualCustomIconToggle().Toggled([this](auto&&, auto&&)
        {
            bool on = TrayIconDualCustomIconToggle().IsOn();
            TrayIconDualCustomIconStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            TrayCustomIconTitleText().Text(on ? g_Loc.Get(L"Opt_TrayCustomIconLight", L"Light Mode Icon") : g_Loc.Get(L"Opt_TrayCustomIcon", L"Custom Icon"));
            if (m_isUpdatingUI) return;
            g_Settings.trayIconUseDualCustomIcons = on;
            TrayCustomIconDarkCard().Visibility((g_Settings.trayPercentageMode == 4 && on) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ApplyAndSave();
            g_TrayIcon.OnThemeChanged();
        });

        TrayCustomIconDarkPresetComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int idx = TrayCustomIconDarkPresetComboBox().SelectedIndex();
            int presetCount = static_cast<int>(std::size(kSunIconPresets));
            if (idx >= 0 && idx < presetCount)
            {
                g_Settings.trayCustomIconDark = kSunIconPresets[idx].spec;
                UpdateCustomIconPreview(TrayCustomIconDarkPreview(), g_Settings.trayCustomIconDark);
                ApplyAndSave();
                g_TrayIcon.OnThemeChanged();
            }
            else if (idx == presetCount)
            {
                std::wstring chosen = BrowseForIcoFile(m_hwnd);
                if (!chosen.empty())
                {
                    g_Settings.trayCustomIconDark = chosen;
                    UpdateCustomIconPreview(TrayCustomIconDarkPreview(), g_Settings.trayCustomIconDark);
                    ApplyAndSave();
                    g_TrayIcon.OnThemeChanged();
                }
                else
                {
                    m_isUpdatingUI = true;
                    SelectSunPresetInComboBox(TrayCustomIconDarkPresetComboBox(), g_Settings.trayCustomIconDark);
                    m_isUpdatingUI = false;
                }
            }
        });

        TrayCustomIconDarkBrowseButton().Click([this](auto&&, auto&&)
        {
            std::wstring chosen = BrowseForIcoFile(m_hwnd);
            if (!chosen.empty())
            {
                g_Settings.trayCustomIconDark = chosen;
                m_isUpdatingUI = true;
                SelectSunPresetInComboBox(TrayCustomIconDarkPresetComboBox(), g_Settings.trayCustomIconDark);
                m_isUpdatingUI = false;
                UpdateCustomIconPreview(TrayCustomIconDarkPreview(), g_Settings.trayCustomIconDark);
                ApplyAndSave();
                g_TrayIcon.OnThemeChanged();
            }
        });

        // Tray Font ComboBox
        TrayFontComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            if (TrayFontComboBox().SelectedItem())
            {
                auto selectedFont = unbox_value<hstring>(TrayFontComboBox().SelectedItem());
                g_Settings.trayFont = selectedFont.c_str();
                ApplyAndSave();
                g_TrayIcon.OnThemeChanged();
            }
        });

        // Tray Font Weight Slider <-> NumberBox synchronization
        TrayFontWeightSlider().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = (std::clamp)(static_cast<int>(std::round(TrayFontWeightSlider().Value())), 100, 900);
            m_isUpdatingUI = true;
            TrayFontWeightNumberBox().Value(val);
            m_isUpdatingUI = false;
            g_Settings.trayFontWeight = val;
            ApplyAndSave();
            g_TrayIcon.OnThemeChanged();
        });

        TrayFontWeightNumberBox().ValueChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            double v = e.NewValue();
            if (std::isnan(v)) return;
            int val = (std::clamp)(static_cast<int>(std::round(v)), 100, 900);
            m_isUpdatingUI = true;
            TrayFontWeightSlider().Value(val);
            m_isUpdatingUI = false;
            g_Settings.trayFontWeight = val;
            ApplyAndSave();
            g_TrayIcon.OnThemeChanged();
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
            ApplyAndSave();
            g_TrayIcon.OnThemeChanged();
        });

        TrayFontSizeNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(TrayFontSizeNumberBox().Value());
            m_isUpdatingUI = true;
            TrayFontSizeSlider().Value(val);
            m_isUpdatingUI = false;
            g_Settings.trayFontSize = val;
            ApplyAndSave();
            g_TrayIcon.OnThemeChanged();
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
            ApplyAndSave();
            g_TrayIcon.OnThemeChanged();
        });

        TrayFontSize100NumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = static_cast<int>(TrayFontSize100NumberBox().Value());
            m_isUpdatingUI = true;
            TrayFontSize100Slider().Value(val);
            m_isUpdatingUI = false;
            g_Settings.trayFontSize100 = val;
            ApplyAndSave();
            g_TrayIcon.OnThemeChanged();
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
            ApplyAndSave();
            g_TrayIcon.OnThemeChanged();
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
            ApplyAndSave();
            g_TrayIcon.OnThemeChanged();
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
                bool startWithWin = g_Settings.startWithWindows || AppSettings::IsStartWithWindowsEnabled();
                StartWithWindowsToggle().IsOn(startWithWin);
                StartWithWindowsToggle().IsEnabled(false);
                StartWithWindowsStatusText().Text(startWithWin ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
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
            auto vis = useWinAccent ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible;
            FlyoutAccentColorCard().Visibility(vis);
            FlyoutDualColorCard().Visibility(vis);
            FlyoutDarkColorCard().Visibility((!useWinAccent && g_Settings.flyoutUseDualColors) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            FlyoutCustomAccentTitleText().Text(g_Settings.flyoutUseDualColors
                ? g_Loc.Get(L"Opt_TrayIconColorLight", L"Light Mode Color")
                : g_Loc.Get(L"Opt_FlyoutCustomAccent", L"Custom Accent Color"));
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

        // Flyout Keyboard Enabled Toggle
        FlyoutKeyboardEnabledToggle().Toggled([this](auto&&, auto&&)
        {
            bool on = FlyoutKeyboardEnabledToggle().IsOn();
            FlyoutKeyboardEnabledStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            if (m_isUpdatingUI) return;
            g_Settings.flyoutKeyboardEnabled = on;
            ApplyAndSave();
        });

        // Schedule Mode ComboBox
        ScheduleModeComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int idx = ScheduleModeComboBox().SelectedIndex();
            g_Settings.scheduleMode = (idx >= 0 && idx <= 2) ? idx : 0;
            UpdateScheduleVisibility();
            ApplyAndSave();
            ScheduleManager::Refresh();
        });

        // Schedule Show OSD Toggle
        ScheduleShowOsdToggle().Toggled([this](auto&&, auto&&)
        {
            bool on = ScheduleShowOsdToggle().IsOn();
            ScheduleShowOsdStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            if (m_isUpdatingUI) return;
            g_Settings.scheduleShowOsd = on;
            ApplyAndSave();
        });

        // Schedule Daytime Brightness Slider & NumberBox
        ScheduleDayBrightnessSlider().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = (std::clamp)(static_cast<int>(std::round(ScheduleDayBrightnessSlider().Value())), 0, 100);
            m_isUpdatingUI = true;
            ScheduleDayBrightnessNumberBox().Value(val);
            m_isUpdatingUI = false;
            g_Settings.scheduleDayBrightness = val;
            ApplyAndSave();
            ScheduleManager::Refresh();
        });
        ScheduleDayBrightnessNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            double raw = ScheduleDayBrightnessNumberBox().Value();
            if (std::isnan(raw)) return;
            int val = (std::clamp)(static_cast<int>(std::round(raw)), 0, 100);
            m_isUpdatingUI = true;
            ScheduleDayBrightnessSlider().Value(val);
            m_isUpdatingUI = false;
            g_Settings.scheduleDayBrightness = val;
            ApplyAndSave();
            ScheduleManager::Refresh();
        });

        // Schedule Nighttime Brightness Slider & NumberBox
        ScheduleNightBrightnessSlider().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int val = (std::clamp)(static_cast<int>(std::round(ScheduleNightBrightnessSlider().Value())), 0, 100);
            m_isUpdatingUI = true;
            ScheduleNightBrightnessNumberBox().Value(val);
            m_isUpdatingUI = false;
            g_Settings.scheduleNightBrightness = val;
            ApplyAndSave();
            ScheduleManager::Refresh();
        });
        ScheduleNightBrightnessNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            double raw = ScheduleNightBrightnessNumberBox().Value();
            if (std::isnan(raw)) return;
            int val = (std::clamp)(static_cast<int>(std::round(raw)), 0, 100);
            m_isUpdatingUI = true;
            ScheduleNightBrightnessSlider().Value(val);
            m_isUpdatingUI = false;
            g_Settings.scheduleNightBrightness = val;
            ApplyAndSave();
            ScheduleManager::Refresh();
        });

        // Schedule Custom Time Pickers
        ScheduleDayTimePicker().TimeChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::TimePickerValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            auto totalSecs = std::chrono::duration_cast<std::chrono::seconds>(e.NewTime()).count();
            int totalMins = static_cast<int>(totalSecs / 60);
            g_Settings.scheduleDayHour = (totalMins / 60) % 24;
            g_Settings.scheduleDayMinute = totalMins % 60;
            ApplyAndSave();
            ScheduleManager::Refresh();
        });
        ScheduleNightTimePicker().TimeChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Controls::TimePickerValueChangedEventArgs const& e)
        {
            if (m_isUpdatingUI) return;
            auto totalSecs = std::chrono::duration_cast<std::chrono::seconds>(e.NewTime()).count();
            int totalMins = static_cast<int>(totalSecs / 60);
            g_Settings.scheduleNightHour = (totalMins / 60) % 24;
            g_Settings.scheduleNightMinute = totalMins % 60;
            ApplyAndSave();
            ScheduleManager::Refresh();
        });

        // Schedule Sun Source & Coordinates
        ScheduleSunSourceComboBox().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            int idx = ScheduleSunSourceComboBox().SelectedIndex();
            g_Settings.scheduleSunSource = (idx == 1) ? 1 : 0;
            UpdateScheduleVisibility();
            if (g_Settings.scheduleSunSource == 0)
            {
                DetectScheduleLocationAsync();
            }
            ApplyAndSave();
            ScheduleManager::Refresh();
        });

        ScheduleDetectLocationButton().Click([this](auto&&, auto&&)
        {
            DetectScheduleLocationAsync();
        });

        ScheduleRefreshSunButton().Click([this](auto&&, auto&&)
        {
            if (g_Settings.scheduleSunSource == 0)
            {
                DetectScheduleLocationAsync();
            }
            UpdateScheduleSunPreview();
            ScheduleManager::Refresh();
        });

        ScheduleLatNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            double raw = ScheduleLatNumberBox().Value();
            if (std::isnan(raw)) return;
            g_Settings.scheduleLatitude = (std::clamp)(raw, -90.0, 90.0);
            UpdateScheduleSunPreview();
            ApplyAndSave();
            ScheduleManager::Refresh();
        });
        ScheduleLonNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            double raw = ScheduleLonNumberBox().Value();
            if (std::isnan(raw)) return;
            g_Settings.scheduleLongitude = (std::clamp)(raw, -180.0, 180.0);
            UpdateScheduleSunPreview();
            ApplyAndSave();
            ScheduleManager::Refresh();
        });

        ScheduleSunriseOffsetNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            double raw = ScheduleSunriseOffsetNumberBox().Value();
            if (std::isnan(raw)) return;
            g_Settings.scheduleSunriseOffset = (std::clamp)(static_cast<int>(std::round(raw)), -180, 180);
            UpdateScheduleSunPreview();
            ApplyAndSave();
            ScheduleManager::Refresh();
        });
        ScheduleSunsetOffsetNumberBox().ValueChanged([this](auto&&, auto&&)
        {
            if (m_isUpdatingUI) return;
            double raw = ScheduleSunsetOffsetNumberBox().Value();
            if (std::isnan(raw)) return;
            g_Settings.scheduleSunsetOffset = (std::clamp)(static_cast<int>(std::round(raw)), -180, 180);
            UpdateScheduleSunPreview();
            ApplyAndSave();
            ScheduleManager::Refresh();
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

        FlyoutDualColorToggle().Toggled([this, updateFlyoutAccentUI](auto&&, auto&&)
        {
            bool on = FlyoutDualColorToggle().IsOn();
            FlyoutDualColorStatusText().Text(on ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
            if (m_isUpdatingUI) return;
            g_Settings.flyoutUseDualColors = on;
            updateFlyoutAccentUI(g_Settings.flyoutUseWindowsAccent);
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
        setupColorControl(OverlayDarkColorPicker(), OverlayDarkColorTextBox(), OverlayDarkColorPreview(), g_Settings.overlayAccentColorDark, []() {
            g_OverlayWindow.OnThemeChanged();
        });

        // Flyout Accent Color
        setupColorControl(FlyoutAccentColorPicker(), FlyoutAccentColorTextBox(), FlyoutAccentColorPreview(), g_Settings.flyoutAccentColor, []() {
            g_FlyoutWindow.OnThemeChanged();
        });
        setupColorControl(FlyoutDarkColorPicker(), FlyoutDarkColorTextBox(), FlyoutDarkColorPreview(), g_Settings.flyoutAccentColorDark, []() {
            g_FlyoutWindow.OnThemeChanged();
        });

        // Tray Icon Color (Light / Default)
        setupColorControl(TrayIconColorPicker(), TrayIconColorTextBox(), TrayIconColorPreview(), g_Settings.trayIconColor, []() {
            g_TrayIcon.OnThemeChanged();
        });

        // Tray Icon Dark Mode Color
        setupColorControl(TrayIconDarkColorPicker(), TrayIconDarkColorTextBox(), TrayIconDarkColorPreview(), g_Settings.trayIconColorDark, []() {
            g_TrayIcon.OnThemeChanged();
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
            g_Settings.hotkeyPresetToggleEnabled = false;
            g_Settings.hotkeyPresetToggleKey = 0x80; // VK_F17
            g_Settings.hotkeyPresetToggleMods = 0;
            g_Settings.hotkeyPresetToggleShowOsd = true;
            g_Settings.hotkeyPresetToggle1Brightness = 30;
            g_Settings.hotkeyPresetToggle2Brightness = 80;
            g_Settings.hotkeyStep = 5;
            g_Settings.hotkeyDelayEnabled = true;
            g_Settings.hotkeyDelayMs = 1000;
            g_Settings.hotkeyRepeatIntervalMs = 90;
            g_Settings.overlayTheme = 0;
            g_Settings.overlayTimeoutMs = 2000;
            g_Settings.overlayUseWindowsAccent = true;
            g_Settings.overlayAccentColor = RGB(0, 120, 212);
            g_Settings.overlayUseDualColors = false;
            g_Settings.overlayAccentColorDark = RGB(96, 205, 255);
            g_Settings.flyoutUseWindowsAccent = true;
            g_Settings.flyoutAccentColor = RGB(0, 120, 212);
            g_Settings.flyoutUseDualColors = false;
            g_Settings.flyoutAccentColorDark = RGB(96, 205, 255);
            g_Settings.flyoutScrollEnabled = true;
            g_Settings.flyoutScrollStep = 5;
            g_Settings.flyoutKeyboardEnabled = false;
            g_Settings.flyoutScrollDelayEnabled = true;
            g_Settings.flyoutScrollDelayMs = 1000;
            g_Settings.flyoutButtonDelayEnabled = false;
            g_Settings.flyoutButtonDelayMs = 500;
            g_Settings.trayPercentageMode = 0;
            g_Settings.trayIconUseDualCustomIcons = false;
            g_Settings.trayCustomIcon = L"preset:sun";
            g_Settings.trayCustomIconDark = L"preset:sun-blue";
            g_Settings.trayIconColor = RGB(0, 120, 212);
            g_Settings.trayIconUseDualColors = false;
            g_Settings.trayIconColorDark = RGB(96, 205, 255);
            g_Settings.trayFont = L"Segoe UI Variable Text";
            g_Settings.trayFontWeight = 375;
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
            g_Settings.syncExternalWithLaptopFnKeys = false;
            g_Settings.showSeparateInMultiFlyout = true;
            g_Settings.disableTurnedOffControl = false;
            g_Settings.scheduleMode = 0;
            g_Settings.scheduleSunSource = 0;
            g_Settings.scheduleDayHour = 7;
            g_Settings.scheduleDayMinute = 0;
            g_Settings.scheduleNightHour = 20;
            g_Settings.scheduleNightMinute = 0;
            g_Settings.scheduleDayBrightness = 80;
            g_Settings.scheduleNightBrightness = 30;
            g_Settings.scheduleLatitude = 47.4979;
            g_Settings.scheduleLongitude = 19.0402;
            g_Settings.scheduleSunriseOffset = 0;
            g_Settings.scheduleSunsetOffset = 0;
            g_Settings.scheduleShowOsd = true;
            for (auto& [monId, cfg] : g_Settings.monitorConfigs)
            {
                cfg.enabled = true;
                cfg.controlMode = 0;
                cfg.offset = 0;
                cfg.syncOverscrollMax = true;
                cfg.syncOverscrollMin = true;
                g_Settings.CopyCustomizationToMonitor(cfg);
                if (!g_Settings.mainMonitorId.empty() && _wcsicmp(monId.c_str(), g_Settings.mainMonitorId.c_str()) != 0)
                {
                    cfg.trayPercentageMode = 3;
                }
            }
            g_MonitorManager.UpdateFnSyncListener();
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

    void SettingsWindow::EnsureTrayTabLoaded()
    {
        if (m_trayTabLoaded) return;
        m_trayTabLoaded = true;

        if (TrayFontComboBox().Items().Size() == 0)
        {
            TrayFontComboBox().Items().Append(box_value(L"Segoe UI Variable Text"));
            TrayFontComboBox().Items().Append(box_value(L"Segoe UI Variable Small"));
            TrayFontComboBox().Items().Append(box_value(L"Segoe UI Variable Display"));
            TrayFontComboBox().Items().Append(box_value(L"Segoe UI"));
            TrayFontComboBox().Items().Append(box_value(L"Trebuchet MS"));
            TrayFontComboBox().Items().Append(box_value(L"Arial"));
            TrayFontComboBox().Items().Append(box_value(L"Consolas"));
            TrayFontComboBox().Items().Append(box_value(L"Tahoma"));
            TrayFontComboBox().Items().Append(box_value(L"Verdana"));
        }

        // Sync font selection to settings
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
    }

    void SettingsWindow::EnsureAboutTabLoaded()
    {
        if (m_aboutTabLoaded) return;
        m_aboutTabLoaded = true;

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
        bool prevUpdating = m_isUpdatingUI;
        m_isUpdatingUI = true;

        auto monitors = g_MonitorManager.GetMonitors();
        m_cachedMonitorIds.clear();
        MainMonitorComboBox().Items().Clear();
        ConfigMonitorComboBox().Items().Clear();

        if (monitors.empty())
        {
            m_selectedConfigMonitorId.clear();
            m_isUpdatingUI = prevUpdating;
            UpdateMonitorsSelectionUI();
            return;
        }

        if (g_Settings.mainMonitorId.empty())
        {
            g_Settings.mainMonitorId = monitors[0].id;
        }
        bool mainFound = false;
        for (const auto& mon : monitors)
        {
            if (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0)
            {
                mainFound = true;
                break;
            }
        }
        if (!mainFound)
        {
            g_Settings.mainMonitorId = monitors[0].id;
        }

        if (m_selectedConfigMonitorId.empty())
        {
            m_selectedConfigMonitorId = g_Settings.mainMonitorId;
        }

        int displayIndex = 1;
        int mainIdx = 0;
        int configIdx = 0;
        bool configFound = false;

        for (const auto& mon : monitors)
        {
            m_cachedMonitorIds.push_back(mon.id);
            std::wstring monName = !mon.friendlyName.empty() ? mon.friendlyName : mon.description;
            auto& cfg = g_Settings.GetOrCreateMonitorConfig(mon.id, monName);
            if (cfg.customName.empty())
            {
                cfg.customName = AppSettings::SanitizeMonitorName(monName, 20);
            }

            std::wstring dispNum = std::to_wstring(displayIndex);
            std::wstring fmt = g_Loc.Get(L"Monitors_DisplayFormat", L"Display %s: %s");
            wchar_t buf[256];
            swprintf_s(buf, fmt.c_str(), dispNum.c_str(), monName.c_str());
            std::wstring label = buf;
            if (mon.isInternal)
            {
                label += L" (" + g_Loc.Get(L"InternalBadge", L"Internal") + L")";
            }
            if (mon.isPoweredOff)
            {
                label += L" (" + g_Loc.Get(L"Common_Off", L"Off") + L")";
            }
            else if (mon.isPrimary)
            {
                label += L" (" + g_Loc.Get(L"PrimaryBadge", L"Primary") + L")";
            }

            MainMonitorComboBox().Items().Append(box_value(label));
            ConfigMonitorComboBox().Items().Append(box_value(label));

            if (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0)
            {
                mainIdx = displayIndex - 1;
            }
            if (_wcsicmp(mon.id.c_str(), m_selectedConfigMonitorId.c_str()) == 0)
            {
                configIdx = displayIndex - 1;
                configFound = true;
            }

            displayIndex++;
        }

        if (!configFound)
        {
            m_selectedConfigMonitorId = g_Settings.mainMonitorId;
            configIdx = mainIdx;
        }

        MainMonitorComboBox().SelectedIndex(mainIdx);
        ConfigMonitorComboBox().SelectedIndex(configIdx);

        m_isUpdatingUI = prevUpdating;
        UpdateMonitorsSelectionUI();
        LoadMonitorConfigToUI();
    }

    void SettingsWindow::UpdateMonitorsSelectionUI()
    {
        bool prevUpdating = m_isUpdatingUI;
        m_isUpdatingUI = true;

        bool isMainInternal = g_MonitorManager.IsMainDisplayInternal();
        SyncLaptopFnKeysCard().Visibility(isMainInternal ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        SyncLaptopFnKeysToggle().IsOn(g_Settings.syncExternalWithLaptopFnKeys);
        SyncLaptopFnKeysStatusText().Text(g_Settings.syncExternalWithLaptopFnKeys ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        ShowSeparateInMultiFlyoutToggle().IsOn(g_Settings.showSeparateInMultiFlyout);
        ShowSeparateInMultiFlyoutStatusText().Text(g_Settings.showSeparateInMultiFlyout ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        DisableTurnedOffControlToggle().IsOn(g_Settings.disableTurnedOffControl);
        DisableTurnedOffControlStatusText().Text(g_Settings.disableTurnedOffControl ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        m_isUpdatingUI = prevUpdating;
    }

    void SettingsWindow::SaveCurrentMonitorConfigFromUI()
    {
        if (m_selectedConfigMonitorId.empty()) return;
        if (!g_Settings.FindMonitorConfig(m_selectedConfigMonitorId)) return;
        auto& cfg = g_Settings.GetOrCreateMonitorConfig(m_selectedConfigMonitorId);
        g_Settings.CopyCustomizationToMonitor(cfg);
    }

    void SettingsWindow::OpenMonitorsSubpage(const std::wstring& subpageTag)
    {
        std::wstring prevSubpage = m_currentMonitorsSubpage;
        m_currentMonitorsSubpage = subpageTag;

        const MonitorControlConfig* cfg = g_Settings.FindMonitorConfig(m_selectedConfigMonitorId);
        bool isMain = (_wcsicmp(m_selectedConfigMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
        bool isSyncNonMain = (!isMain && (!cfg || cfg->controlMode == 0));

        if ((m_currentMonitorsSubpage == L"Shortcuts" || m_currentMonitorsSubpage == L"Schedule") && isSyncNonMain)
        {
            m_currentMonitorsSubpage.clear();
        }

        SetBackButtonUsable(!m_currentMonitorsSubpage.empty());

        auto hostChildren = MonitorsViewHost().Children();
        bool viewChanged = (prevSubpage.empty() != m_currentMonitorsSubpage.empty()) ||
                           (!m_currentMonitorsSubpage.empty() && prevSubpage != m_currentMonitorsSubpage) ||
                           hostChildren.Size() != 1;

        if (m_currentMonitorsSubpage.empty())
        {
            if (!prevSubpage.empty())
            {
                // Returning from a submenu: slide left-to-right
                MonitorsViewTransition().FromHorizontalOffset(-40);
                MonitorsViewTransition().FromVerticalOffset(0);
            }
            else
            {
                // Entering Monitors from top-level NavigationView: slide bottom-to-top
                MonitorsViewTransition().FromHorizontalOffset(0);
                MonitorsViewTransition().FromVerticalOffset(20);
            }
            if (viewChanged)
            {
                hostChildren.Clear();
                m_monitorsMainView.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                hostChildren.Append(m_monitorsMainView);
            }
            return;
        }

        MonitorsViewTransition().FromHorizontalOffset(40);
        MonitorsViewTransition().FromVerticalOffset(0);

        std::wstring dispLabel = (cfg && !cfg->customName.empty()) ? cfg->customName : m_selectedConfigMonitorId;
        MonitorsBreadcrumbTargetText().Text(dispLabel);

        bool showShortcuts = (m_currentMonitorsSubpage == L"Shortcuts" && !isSyncNonMain);
        bool showOsd = (m_currentMonitorsSubpage == L"OSD");
        bool showTray = (m_currentMonitorsSubpage == L"Tray");
        bool showFlyout = (m_currentMonitorsSubpage == L"Flyout");
        bool showSchedule = (m_currentMonitorsSubpage == L"Schedule" && !isSyncNonMain);

        if (showShortcuts) MonitorsBreadcrumbCurrentText().Text(g_Loc.Get(L"Nav_Shortcuts", L"Shortcuts"));
        else if (showOsd) MonitorsBreadcrumbCurrentText().Text(g_Loc.Get(L"Nav_OSD", L"On-Screen Display"));
        else if (showTray) MonitorsBreadcrumbCurrentText().Text(g_Loc.Get(L"Nav_Tray", L"System Tray"));
        else if (showFlyout) MonitorsBreadcrumbCurrentText().Text(g_Loc.Get(L"Nav_FlyoutSlider", L"Flyout Slider"));
        else if (showSchedule) MonitorsBreadcrumbCurrentText().Text(g_Loc.Get(L"Nav_Schedule", L"Schedule"));

        ShortcutsCustomSection().Visibility(showShortcuts ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        OsdAccentCustomSection().Visibility(showOsd ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TrayAppearanceCustomSection().Visibility(showTray ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TrayPctHideItem().Visibility(isMain ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);
        TrayPctHideItem().IsEnabled(!isMain);
        TrayWheelCustomSection().Visibility((showTray && !isSyncNonMain) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        FlyoutBehaviorCustomSection().Visibility((showFlyout && !isSyncNonMain) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        FlyoutAccentCustomSection().Visibility(showFlyout ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        ScheduleCustomSection().Visibility(showSchedule ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        if (viewChanged)
        {
            hostChildren.Clear();
            m_monitorsSubpageView.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            hostChildren.Append(m_monitorsSubpageView);
            SettingsScrollViewer().ChangeView(nullptr, 0.0, nullptr, true);
        }
    }

    void SettingsWindow::OpenTimingsSubpage(const std::wstring& subpageTag)
    {
        std::wstring prevSubpage = m_currentTimingsSubpage;
        m_currentTimingsSubpage = subpageTag;

        SetBackButtonUsable(!m_currentTimingsSubpage.empty());

        auto hostChildren = TimingsViewHost().Children();
        bool viewChanged = (prevSubpage.empty() != m_currentTimingsSubpage.empty()) ||
                           (!m_currentTimingsSubpage.empty() && prevSubpage != m_currentTimingsSubpage) ||
                           hostChildren.Size() != 1;

        if (m_currentTimingsSubpage.empty())
        {
            if (!prevSubpage.empty())
            {
                TimingsViewTransition().FromHorizontalOffset(-40);
                TimingsViewTransition().FromVerticalOffset(0);
            }
            else
            {
                TimingsViewTransition().FromHorizontalOffset(0);
                TimingsViewTransition().FromVerticalOffset(20);
            }
            if (viewChanged)
            {
                hostChildren.Clear();
                m_timingsMainView.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                hostChildren.Append(m_timingsMainView);
            }
            return;
        }

        TimingsViewTransition().FromHorizontalOffset(40);
        TimingsViewTransition().FromVerticalOffset(0);

        bool showShortcuts = (m_currentTimingsSubpage == L"Shortcuts");
        bool showOsd = (m_currentTimingsSubpage == L"OSD");
        bool showTray = (m_currentTimingsSubpage == L"Tray");
        bool showFlyout = (m_currentTimingsSubpage == L"Flyout");

        if (showShortcuts) TimingsBreadcrumbCurrentText().Text(g_Loc.Get(L"Nav_Shortcuts", L"Shortcuts"));
        else if (showOsd) TimingsBreadcrumbCurrentText().Text(g_Loc.Get(L"Nav_OSD", L"On-Screen Display"));
        else if (showTray) TimingsBreadcrumbCurrentText().Text(g_Loc.Get(L"Nav_Tray", L"System Tray"));
        else if (showFlyout) TimingsBreadcrumbCurrentText().Text(g_Loc.Get(L"Nav_FlyoutSlider", L"Flyout Slider"));

        TimingsShortcutsSection().Visibility(showShortcuts ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TimingsOsdSection().Visibility(showOsd ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TimingsTraySection().Visibility(showTray ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TimingsFlyoutSection().Visibility(showFlyout ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        if (viewChanged)
        {
            hostChildren.Clear();
            m_timingsSubpageView.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            hostChildren.Append(m_timingsSubpageView);
            SettingsScrollViewer().ChangeView(nullptr, 0.0, nullptr, true);
        }
    }

    void SettingsWindow::LoadMonitorConfigToUI()
    {
        bool prevUpdating = m_isUpdatingUI;
        m_isUpdatingUI = true;

        const MonitorControlConfig* cfg = g_Settings.FindMonitorConfig(m_selectedConfigMonitorId);
        if (!cfg && !g_Settings.mainMonitorId.empty())
        {
            m_selectedConfigMonitorId = g_Settings.mainMonitorId;
            cfg = g_Settings.FindMonitorConfig(m_selectedConfigMonitorId);
        }

        if (!cfg)
        {
            m_isUpdatingUI = prevUpdating;
            return;
        }

        bool isMain = (_wcsicmp(m_selectedConfigMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
        if (isMain && !cfg->enabled)
        {
            g_Settings.GetOrCreateMonitorConfig(m_selectedConfigMonitorId).enabled = true;
        }
        if (isMain && cfg->controlMode != 0)
        {
            g_Settings.GetOrCreateMonitorConfig(m_selectedConfigMonitorId).controlMode = 0;
        }
        if (isMain && cfg->trayPercentageMode == 3)
        {
            g_Settings.GetOrCreateMonitorConfig(m_selectedConfigMonitorId).trayPercentageMode = 0;
        }

        MonitorEnabledCard().Visibility(isMain ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);
        MonitorEnabledToggle().IsOn(cfg->enabled);
        MonitorEnabledStatusText().Text(cfg->enabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        MonitorCustomNameTextBox().Text(cfg->customName);

        MonitorControlModeComboBox().IsEnabled(!isMain);
        MonitorControlModeComboBox().SelectedIndex((isMain || cfg->controlMode == 0) ? 0 : 1);

        bool isSync = (isMain || cfg->controlMode == 0);
        bool isSyncNonMain = (!isMain && cfg->controlMode == 0);
        MonitorOffsetCard().Visibility(isSyncNonMain ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        SyncOverscrollMaxCard().Visibility(isSync ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        SyncOverscrollMinCard().Visibility(isSync ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        MonitorSyncNoticeBanner().Visibility(isSyncNonMain ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        MonitorsShortcutsCard().Visibility(isSyncNonMain ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);
        MonitorsScheduleCard().Visibility(isSyncNonMain ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);

        OpenMonitorsSubpage(m_currentMonitorsSubpage);

        MonitorOffsetSlider().Value(cfg->offset);
        MonitorOffsetNumberBox().Value(cfg->offset);
        SyncOverscrollMaxToggle().IsOn(cfg->syncOverscrollMax);
        SyncOverscrollMaxStatusText().Text(cfg->syncOverscrollMax ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        SyncOverscrollMinToggle().IsOn(cfg->syncOverscrollMin);
        SyncOverscrollMinStatusText().Text(cfg->syncOverscrollMin ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        g_Settings.ApplyMonitorCustomizationToLegacy(*cfg);
        m_isUpdatingUI = prevUpdating;
        LoadSettingsToUI();
    }

    void SettingsWindow::UpdateLocalizedStrings()
    {
        bool prevUpdating = m_isUpdatingUI;
        m_isUpdatingUI = true;

        if (m_appWindow)
        {
            m_appWindow.Title(g_Loc.Get(L"AppTitle", L"ClickBrightness settings"));
        }
        AppTitleBarText().Text(g_Loc.Get(L"AppTitle", L"ClickBrightness settings"));

        // Navigation
        NavItemGeneral().Content(box_value(g_Loc.Get(L"Nav_General", L"General")));
        NavItemMonitors().Content(box_value(g_Loc.Get(L"Nav_Monitors", L"Monitors")));
        NavItemTimings().Content(box_value(g_Loc.Get(L"Nav_Timings", L"Timings & Global")));
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
        ensureNavIcon(NavItemMonitors(), L"\uE7F4");
        ensureNavIcon(NavItemTimings(), L"\uE916");
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
        bool startWithWin = g_Settings.startWithWindows || AppSettings::IsStartWithWindowsEnabled();
        if (isInstalled)
        {
            StartWithWindowsDescText().Text(g_Loc.Get(L"Desc_StartWithWindows", L"Launch ClickBrightness automatically on system logon"));
            StartWithWindowsToggle().IsEnabled(true);
        }
        else
        {
            StartWithWindowsDescText().Text(g_Loc.Get(L"Desc_StartWithWindows_NotInstalled", L"Launch ClickBrightness automatically on system logon (Available when installed in Program Files)"));
            StartWithWindowsToggle().IsEnabled(false);
        }
        StartWithWindowsToggle().IsOn(startWithWin);
        StartWithWindowsStatusText().Text(startWithWin ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        PhysicalMonitorsTitleText().Text(g_Loc.Get(L"Opt_PhysicalMonitors", L"Physical Monitors"));
        RedetectMonitorsButton().Content(box_value(g_Loc.Get(L"Btn_Redetect", L"Re-detect")));

        // Monitors & Timings Headers
        MonitorsHeaderTitle().Text(g_Loc.Get(L"Monitors_Title", L"Monitors & Displays"));
        MonitorsHeaderSubtitle().Text(g_Loc.Get(L"Monitors_Subtitle", L"Configure main display, sync/separate modes, offsets, and per-monitor controls"));
        MonitorsBreadcrumbRootText().Text(g_Loc.Get(L"Nav_Monitors", L"Monitors"));
        MonitorSubmenusSectionTitle().Text(g_Loc.Get(L"Sec_MonitorSubmenus", L"Customization & Controls"));
        SubmenuShortcutsTitleText().Text(g_Loc.Get(L"Shortcuts_Title", L"Keyboard Shortcuts"));
        SubmenuShortcutsDescText().Text(g_Loc.Get(L"Shortcuts_Subtitle", L"Global hotkeys for brightness stepping and preset jumping"));
        SubmenuOsdTitleText().Text(g_Loc.Get(L"OSD_Title", L"On-Screen Display (OSD)"));
        SubmenuOsdDescText().Text(g_Loc.Get(L"Desc_SubmenuOsd", L"Overlay accent color for this display"));
        SubmenuTrayTitleText().Text(g_Loc.Get(L"Tray_Title", L"System Tray Icon"));
        SubmenuTrayDescText().Text(g_Loc.Get(L"Tray_Subtitle", L"Taskbar indicator, live percentage typography, and mouse actions"));
        SubmenuFlyoutTitleText().Text(g_Loc.Get(L"Flyout_Title", L"Flyout Slider"));
        SubmenuFlyoutDescText().Text(g_Loc.Get(L"Desc_SubmenuFlyout", L"Flyout scrolling and slider accent color"));
        MainMonitorTitleText().Text(g_Loc.Get(L"Opt_MainMonitor", L"Main Display"));
        MainMonitorDescText().Text(g_Loc.Get(L"Desc_MainMonitor", L"Primary reference display used for brightness offsets and Sync mode"));
        SyncLaptopFnKeysTitleText().Text(g_Loc.Get(L"Opt_SyncLaptopFnKeys", L"Sync External Monitors with Laptop Fn Keys"));
        SyncLaptopFnKeysDescText().Text(g_Loc.Get(L"Desc_SyncLaptopFnKeys", L"Automatically sync external Sync-mode monitors when adjusting internal display brightness via laptop Fn keys"));
        ShowSeparateInMultiFlyoutTitleText().Text(g_Loc.Get(L"Opt_ShowSeparateInMultiFlyout", L"Show Separate Monitors in Multi-Monitor Flyout"));
        ShowSeparateInMultiFlyoutDescText().Text(g_Loc.Get(L"Desc_ShowSeparateInMultiFlyout", L"Include Separate-mode displays with their individual preset buttons in the main multi-monitor flyout"));
        DisableTurnedOffControlTitleText().Text(g_Loc.Get(L"Opt_DisableTurnedOffControl", L"Disable Control for Turned-Off Monitors"));
        DisableTurnedOffControlDescText().Text(g_Loc.Get(L"Desc_DisableTurnedOffControl", L"Prevent brightness changes and OSD popups for displays detected as switched off"));
        MonitorConfigSectionTitle().Text(g_Loc.Get(L"Sec_MonitorConfig", L"Display Configuration"));
        ConfigMonitorSelectTitleText().Text(g_Loc.Get(L"Opt_ConfigMonitor", L"Selected Display"));
        ConfigMonitorSelectDescText().Text(g_Loc.Get(L"Desc_ConfigMonitor", L"Choose a detected display to configure its mode, name, and controls"));
        MonitorEnabledTitleText().Text(g_Loc.Get(L"Opt_MonitorEnabled", L"Brightness Control"));
        MonitorEnabledDescText().Text(g_Loc.Get(L"Desc_MonitorEnabled", L"Enable or disable brightness adjustments for this display"));
        MonitorCustomNameTitleText().Text(g_Loc.Get(L"Opt_MonitorCustomName", L"Monitor Name"));
        MonitorCustomNameDescText().Text(g_Loc.Get(L"Desc_MonitorCustomName", L"Display label shown in the OSD banner and flyout slider (max 20 characters)"));
        MonitorControlModeTitleText().Text(g_Loc.Get(L"Opt_MonitorControlMode", L"Control Mode"));
        MonitorControlModeDescText().Text(g_Loc.Get(L"Desc_MonitorControlMode", L"Sync follows the Main Display with an offset; Separate uses independent controls"));
        MonitorModeItemSync().Content(box_value(g_Loc.Get(L"MonitorMode_Sync", L"Sync")));
        MonitorModeItemSeparate().Content(box_value(g_Loc.Get(L"MonitorMode_Separate", L"Separate")));
        MonitorOffsetTitleText().Text(g_Loc.Get(L"Opt_MonitorOffset", L"Brightness Offset (%)"));
        MonitorOffsetDescText().Text(g_Loc.Get(L"Desc_MonitorOffset", L"Relative brightness offset applied compared to the Main Display"));
        SyncOverscrollMaxTitleText().Text(g_Loc.Get(L"Opt_SyncOverscrollMax", L"Overscroll when a display is at 100%"));
        SyncOverscrollMaxDescText().Text(g_Loc.Get(L"Desc_SyncOverscrollMax", L"Continue increasing other synced displays even when at least one synced display is already at 100%"));
        SyncOverscrollMinTitleText().Text(g_Loc.Get(L"Opt_SyncOverscrollMin", L"Overscroll when a display is at 0%"));
        SyncOverscrollMinDescText().Text(g_Loc.Get(L"Desc_SyncOverscrollMin", L"Continue decreasing other synced displays even when at least one synced display is already at 0%"));
        MonitorSyncNoticeText().Text(g_Loc.Get(L"Monitor_SyncNotice", L"This display is in Sync mode and uses the Main Display's shortcuts and wheel controls. You can customize its OSD color, Flyout color, and optional tray icon below."));

        TimingsHeaderTitle().Text(g_Loc.Get(L"Timings_Title", L"Timings & Global"));
        TimingsHeaderSubtitle().Text(g_Loc.Get(L"Timings_Subtitle", L"Global hardware delays, OSD appearance, and flyout keyboard navigation"));
        TimingsBreadcrumbRootText().Text(g_Loc.Get(L"Nav_Timings", L"Timings & Global"));
        GlobalOsdFlyoutSectionTitle().Text(g_Loc.Get(L"Nav_OSD", L"On-Screen Display"));
        TimingsSubmenusSectionTitle().Text(g_Loc.Get(L"Sec_TimingsSubmenus", L"Feature Settings & Delays"));
        SubmenuTimingsShortcutsTitleText().Text(g_Loc.Get(L"Nav_Shortcuts", L"Shortcuts"));
        SubmenuTimingsShortcutsDescText().Text(g_Loc.Get(L"Desc_SubmenuTimingsShortcuts", L"Hotkey debounce delay and key hold repeat interval"));
        SubmenuTimingsFlyoutTitleText().Text(g_Loc.Get(L"Nav_FlyoutSlider", L"Flyout Slider"));
        SubmenuTimingsFlyoutDescText().Text(g_Loc.Get(L"Desc_SubmenuTimingsFlyout", L"Keyboard controls, scrolling delay, and button debounce delay"));
        SubmenuTimingsOsdTitleText().Text(g_Loc.Get(L"Nav_OSD", L"On-Screen Display"));
        SubmenuTimingsOsdDescText().Text(g_Loc.Get(L"Desc_SubmenuTimingsOsd", L"OSD banner theme and auto-hide duration"));
        SubmenuTimingsTrayTitleText().Text(g_Loc.Get(L"Nav_Tray", L"System Tray"));
        SubmenuTimingsTrayDescText().Text(g_Loc.Get(L"Desc_SubmenuTimingsTray", L"Mouse wheel scrolling debounce delay on the tray icon"));

        // Shortcuts
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

        // Preset Toggle Shortcut
        ShortcutsPresetToggleSectionTitle().Text(g_Loc.Get(L"Sec_ShortcutsPresetToggle", L"Preset Toggle Shortcut"));
        HotkeyPresetToggleTitleText().Text(g_Loc.Get(L"Opt_HotkeyPresetToggle", L"Preset Toggle Shortcut"));
        HotkeyPresetToggleDescText().Text(g_Loc.Get(L"Desc_HotkeyPresetToggle", L"Toggle between two brightness presets using a single shortcut"));
        HotkeyPresetToggleStatusText().Text(HotkeyPresetToggleToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        PresetToggleKeyTitleText().Text(g_Loc.Get(L"Opt_KeyPresetToggle", L"Toggle Shortcut Key"));
        PresetToggleKeyDescText().Text(g_Loc.Get(L"Desc_KeyPresetToggle", L"Press shortcut to toggle between Preset 1 and Preset 2"));
        PresetToggle1BrightnessTitleText().Text(g_Loc.Get(L"Opt_PresetToggle1Brightness", L"Preset 1 Brightness (%)"));
        PresetToggle1BrightnessDescText().Text(g_Loc.Get(L"Desc_PresetToggle1Brightness", L"First target brightness level toggled by the shortcut"));
        PresetToggle2BrightnessTitleText().Text(g_Loc.Get(L"Opt_PresetToggle2Brightness", L"Preset 2 Brightness (%)"));
        PresetToggle2BrightnessDescText().Text(g_Loc.Get(L"Desc_PresetToggle2Brightness", L"Second target brightness level toggled by the shortcut"));
        HotkeyPresetToggleShowOsdTitleText().Text(g_Loc.Get(L"Opt_HotkeyPresetToggleShowOsd", L"Show OSD Banner"));
        HotkeyPresetToggleShowOsdDescText().Text(g_Loc.Get(L"Desc_HotkeyPresetToggleShowOsd", L"Display the on-screen brightness banner when toggling presets"));
        HotkeyPresetToggleShowOsdStatusText().Text(HotkeyPresetToggleShowOsdToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        ResetShortcutsTitleText().Text(g_Loc.Get(L"Opt_ResetShortcuts", L"Reset Shortcuts"));
        ResetShortcutsDescText().Text(g_Loc.Get(L"Desc_ResetShortcuts", L"Restore hotkeys to defaults (F14, F13, F15, F16, F17)"));
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

        OverlayTimeoutTitleText().Text(g_Loc.Get(L"Opt_OverlayTimeout", L"Hide Time (ms)"));
        OverlayTimeoutDescText().Text(g_Loc.Get(L"Desc_OverlayTimeout", L"Auto-hide timeout for banner overlay"));

        // System Tray
        TrayAppearanceSectionTitle().Text(g_Loc.Get(L"Sec_TrayAppearance", L"System Tray Icon"));
        TrayShowPercentageTitleText().Text(g_Loc.Get(L"Opt_TrayShowPercentage", L"Tray Icon"));
        TrayShowPercentageDescText().Text(g_Loc.Get(L"Desc_TrayShowPercentage", L"Choose how the brightness percentage is displayed on the system tray icon"));
        TrayPctFollowWindowsItem().Content(box_value(g_Loc.Get(L"Opt_TrayPctFollowWindows", L"Follow Windows")));
        TrayPctColoredItem().Content(box_value(g_Loc.Get(L"Opt_TrayPctColored", L"Colored")));
        TrayPctDisabledItem().Content(box_value(g_Loc.Get(L"Opt_TrayPctDisabled", L"Disabled")));
        TrayPctHideItem().Content(box_value(g_Loc.Get(L"Opt_TrayPctHide", L"Hide")));
        TrayPctCustomItem().Content(box_value(g_Loc.Get(L"Opt_TrayPctCustom", L"Custom")));
        TrayCustomIconTitleText().Text(TrayIconDualCustomIconToggle().IsOn() ? g_Loc.Get(L"Opt_TrayCustomIconLight", L"Light Mode Icon") : g_Loc.Get(L"Opt_TrayCustomIcon", L"Custom Icon"));
        TrayCustomIconDescText().Text(g_Loc.Get(L"Desc_TrayCustomIcon", L"Choose a preset sun icon or browse for a custom .ico file"));
        TrayCustomIconBrowseButton().Content(box_value(g_Loc.Get(L"Common_Browse", L"Browse...")));
        TrayIconDualCustomIconTitleText().Text(g_Loc.Get(L"Opt_TrayIconDualCustomIcons", L"Separate Dark & Light Icons"));
        TrayIconDualCustomIconDescText().Text(g_Loc.Get(L"Desc_TrayIconDualCustomIcons", L"Use a different custom tray icon when Windows is in dark mode"));
        TrayIconDualCustomIconStatusText().Text(TrayIconDualCustomIconToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayCustomIconDarkTitleText().Text(g_Loc.Get(L"Opt_TrayCustomIconDark", L"Dark Mode Icon"));
        TrayCustomIconDarkDescText().Text(g_Loc.Get(L"Desc_TrayCustomIconDark", L"Choose a preset sun icon or browse for a custom .ico file in dark mode"));
        TrayCustomIconDarkBrowseButton().Content(box_value(g_Loc.Get(L"Common_Browse", L"Browse...")));
        TrayIconColorTitleText().Text(TrayIconDualColorToggle().IsOn() ? g_Loc.Get(L"Opt_TrayIconColorLight", L"Light Mode Color") : g_Loc.Get(L"Opt_TrayIconColor", L"Icon Color"));
        TrayIconColorDescText().Text(g_Loc.Get(L"Desc_TrayIconColor", L"Custom color for the system tray percentage text"));
        TrayIconDualColorTitleText().Text(g_Loc.Get(L"Opt_TrayIconDualColor", L"Separate Dark & Light Colors"));
        TrayIconDualColorDescText().Text(g_Loc.Get(L"Desc_TrayIconDualColor", L"Use a different tray icon color when Windows is in dark mode"));
        TrayIconDualColorStatusText().Text(TrayIconDualColorToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayIconDarkColorTitleText().Text(g_Loc.Get(L"Opt_TrayIconColorDark", L"Dark Mode Color"));
        TrayIconDarkColorDescText().Text(g_Loc.Get(L"Desc_TrayIconColorDark", L"Custom color for the system tray percentage text in dark mode"));
        TrayTypographySectionTitle().Text(g_Loc.Get(L"Sec_TrayTypography", L"System Tray Icon Typography"));
        TrayFontTitleText().Text(g_Loc.Get(L"Opt_TrayFont", L"Number Font"));
        TrayFontDescText().Text(g_Loc.Get(L"Desc_TrayFont", L"DirectWrite rendered font for tray icon"));
        TrayFontWeightTitleText().Text(g_Loc.Get(L"Opt_TrayFontWeight", L"Font Weight"));
        TrayFontWeightDescText().Text(g_Loc.Get(L"Desc_TrayFontWeight", L"Stroke thickness from 100 to 900 (e.g. 300 Light, 350 Semilight, 400 Regular)"));
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
        FlyoutSliderSectionTitle().Text(g_Loc.Get(L"Flyout_Title", L"Flyout Slider"));
        FlyoutScrollEnabledTitleText().Text(g_Loc.Get(L"Opt_FlyoutScrollEnabled", L"Slider Scrolling"));
        FlyoutScrollEnabledDescText().Text(g_Loc.Get(L"Desc_FlyoutScrollEnabled", L"Adjust brightness by scrolling the mouse wheel over the flyout slider"));
        FlyoutScrollEnabledStatusText().Text(FlyoutScrollEnabledToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        FlyoutScrollStepTitleText().Text(g_Loc.Get(L"Opt_FlyoutScrollStep", L"Scrolling Brightness Step"));
        FlyoutScrollStepDescText().Text(g_Loc.Get(L"Desc_FlyoutScrollStep", L"Percentage adjustment per mouse wheel notch on the flyout slider"));
        FlyoutKeyboardEnabledTitleText().Text(g_Loc.Get(L"Opt_FlyoutKeyboardEnabled", L"Keyboard Controls"));
        FlyoutKeyboardEnabledDescText().Text(g_Loc.Get(L"Desc_FlyoutKeyboardEnabled", L"Adjust brightness using keyboard arrow keys, Page Up/Down, and Home/End"));
        FlyoutKeyboardEnabledStatusText().Text(FlyoutKeyboardEnabledToggle().IsOn() ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
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
        AboutVersionText().Text(g_Loc.Get(L"About_Version", L"Version 2.0.0 (x64 Release)"));
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
            KeyPresetToggleButton().Content(box_value(HotkeyManager::FormatHotkey(m_curPresetToggleMods, m_curPresetToggleKey)));
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
        else if (m_captureTarget == 5) KeyPresetToggleButton().Content(box_value(hint));
    }

    void SettingsWindow::HandleCapturedKey(UINT vkCode, UINT mods)
    {
        if (m_captureTarget == 0) return;

        if (vkCode == VK_ESCAPE)
        {
            StopCapture(false);
            return;
        }

        // Ensure one hotkey is not used multiple times at once (same monitor or across monitors)
        if (vkCode != 0)
        {
            if (m_captureTarget != 1 && m_curUpKey == vkCode && m_curUpMods == mods)
            {
                m_curUpKey = 0; m_curUpMods = 0;
                g_Settings.hotkeyUpKey = 0; g_Settings.hotkeyUpMods = 0;
            }
            if (m_captureTarget != 2 && m_curDownKey == vkCode && m_curDownMods == mods)
            {
                m_curDownKey = 0; m_curDownMods = 0;
                g_Settings.hotkeyDownKey = 0; g_Settings.hotkeyDownMods = 0;
            }
            if (m_captureTarget != 3 && m_curPreset1Key == vkCode && m_curPreset1Mods == mods)
            {
                m_curPreset1Key = 0; m_curPreset1Mods = 0;
                g_Settings.hotkeyPreset1Key = 0; g_Settings.hotkeyPreset1Mods = 0;
            }
            if (m_captureTarget != 4 && m_curPreset2Key == vkCode && m_curPreset2Mods == mods)
            {
                m_curPreset2Key = 0; m_curPreset2Mods = 0;
                g_Settings.hotkeyPreset2Key = 0; g_Settings.hotkeyPreset2Mods = 0;
            }
            if (m_captureTarget != 5 && m_curPresetToggleKey == vkCode && m_curPresetToggleMods == mods)
            {
                m_curPresetToggleKey = 0; m_curPresetToggleMods = 0;
                g_Settings.hotkeyPresetToggleKey = 0; g_Settings.hotkeyPresetToggleMods = 0;
            }
            for (auto& kv : g_Settings.monitorConfigs)
            {
                if (_wcsicmp(kv.first.c_str(), m_selectedConfigMonitorId.c_str()) == 0) continue;
                AppSettings::ClearMatchingHotkey(kv.second, vkCode, mods);
            }
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
        else if (m_captureTarget == 5)
        {
            m_curPresetToggleKey = vkCode;
            m_curPresetToggleMods = mods;
            g_Settings.hotkeyPresetToggleKey = m_curPresetToggleKey;
            g_Settings.hotkeyPresetToggleMods = m_curPresetToggleMods;
        }

        StopCapture(true);
        LoadSettingsToUI();
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
        else if (uMsg == WM_TIMER && wParam == 102)
        {
            pThis->FlushPendingSave();
            return 0;
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

        // Hotkey Preset Toggle & Cards
        HotkeyPresetToggleToggle().IsOn(g_Settings.hotkeyPresetToggleEnabled);
        HotkeyPresetToggleStatusText().Text(g_Settings.hotkeyPresetToggleEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        PresetToggleKeyCard().Visibility(g_Settings.hotkeyPresetToggleEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        PresetToggle1BrightnessCard().Visibility(g_Settings.hotkeyPresetToggleEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        PresetToggle2BrightnessCard().Visibility(g_Settings.hotkeyPresetToggleEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        HotkeyPresetToggleShowOsdCard().Visibility(g_Settings.hotkeyPresetToggleEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        HotkeyPresetToggleShowOsdToggle().IsOn(g_Settings.hotkeyPresetToggleShowOsd);
        HotkeyPresetToggleShowOsdStatusText().Text(g_Settings.hotkeyPresetToggleShowOsd ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

        m_curPresetToggleKey = g_Settings.hotkeyPresetToggleKey;
        m_curPresetToggleMods = g_Settings.hotkeyPresetToggleMods;
        KeyPresetToggleButton().Content(box_value(HotkeyManager::FormatHotkey(m_curPresetToggleMods, m_curPresetToggleKey)));
        PresetToggle1BrightnessSlider().Value(g_Settings.hotkeyPresetToggle1Brightness);
        PresetToggle1BrightnessNumberBox().Value(g_Settings.hotkeyPresetToggle1Brightness);
        PresetToggle2BrightnessSlider().Value(g_Settings.hotkeyPresetToggle2Brightness);
        PresetToggle2BrightnessNumberBox().Value(g_Settings.hotkeyPresetToggle2Brightness);

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
        OverlayDualColorToggle().IsOn(g_Settings.overlayUseDualColors);
        OverlayDualColorStatusText().Text(g_Settings.overlayUseDualColors ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        OverlayAccentColorCard().Visibility(g_Settings.overlayUseWindowsAccent ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);
        OverlayDualColorCard().Visibility(g_Settings.overlayUseWindowsAccent ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);
        OverlayDarkColorCard().Visibility((!g_Settings.overlayUseWindowsAccent && g_Settings.overlayUseDualColors) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        OverlayCustomAccentTitleText().Text(g_Settings.overlayUseDualColors
            ? g_Loc.Get(L"Opt_TrayIconColorLight", L"Light Mode Color")
            : g_Loc.Get(L"Opt_OverlayCustomAccent", L"Custom Accent Color"));

        // Tray Percentage Icon Mode
        bool isMain = (_wcsicmp(m_selectedConfigMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
        int pctMode = (g_Settings.trayPercentageMode >= 0 && g_Settings.trayPercentageMode <= 4) ? g_Settings.trayPercentageMode : 0;
        if (isMain && pctMode == 3) pctMode = 0;
        TrayPctHideItem().Visibility(isMain ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);
        TrayPctHideItem().IsEnabled(!isMain);
        TrayPercentageModeComboBox().SelectedIndex(pctMode);
        TrayIconDualColorToggle().IsOn(g_Settings.trayIconUseDualColors);
        TrayIconDualColorStatusText().Text(g_Settings.trayIconUseDualColors ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayIconColorTitleText().Text(g_Settings.trayIconUseDualColors ? g_Loc.Get(L"Opt_TrayIconColorLight", L"Light Mode Color") : g_Loc.Get(L"Opt_TrayIconColor", L"Icon Color"));
        TrayIconColorCard().Visibility(pctMode == 1 ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TrayIconDualColorCard().Visibility(pctMode == 1 ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TrayIconDarkColorCard().Visibility((pctMode == 1 && g_Settings.trayIconUseDualColors) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TrayIconDualCustomIconToggle().IsOn(g_Settings.trayIconUseDualCustomIcons);
        TrayIconDualCustomIconStatusText().Text(g_Settings.trayIconUseDualCustomIcons ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        TrayCustomIconTitleText().Text(g_Settings.trayIconUseDualCustomIcons ? g_Loc.Get(L"Opt_TrayCustomIconLight", L"Light Mode Icon") : g_Loc.Get(L"Opt_TrayCustomIcon", L"Custom Icon"));
        TrayCustomIconCard().Visibility(pctMode == 4 ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TrayIconDualCustomIconCard().Visibility(pctMode == 4 ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        TrayCustomIconDarkCard().Visibility((pctMode == 4 && g_Settings.trayIconUseDualCustomIcons) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        SelectSunPresetInComboBox(TrayCustomIconPresetComboBox(), g_Settings.trayCustomIcon);
        SelectSunPresetInComboBox(TrayCustomIconDarkPresetComboBox(), g_Settings.trayCustomIconDark);
        UpdateCustomIconPreview(TrayCustomIconPreview(), g_Settings.trayCustomIcon);
        UpdateCustomIconPreview(TrayCustomIconDarkPreview(), g_Settings.trayCustomIconDark);
        TrayTypographyPanel().Visibility((pctMode == 0 || pctMode == 1) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        // Tray Font Weight
        int weightVal = (std::clamp)(g_Settings.trayFontWeight, 100, 900);
        TrayFontWeightSlider().Value(weightVal);
        TrayFontWeightNumberBox().Value(weightVal);

        // Tray Font
        if (m_trayTabLoaded)
        {
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

        // Schedule
        ScheduleModeComboBox().SelectedIndex((std::clamp)(g_Settings.scheduleMode, 0, 2));
        ScheduleShowOsdToggle().IsOn(g_Settings.scheduleShowOsd);
        ScheduleShowOsdStatusText().Text(g_Settings.scheduleShowOsd ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        ScheduleDayBrightnessSlider().Value(g_Settings.scheduleDayBrightness);
        ScheduleDayBrightnessNumberBox().Value(g_Settings.scheduleDayBrightness);
        ScheduleNightBrightnessSlider().Value(g_Settings.scheduleNightBrightness);
        ScheduleNightBrightnessNumberBox().Value(g_Settings.scheduleNightBrightness);
        ScheduleDayTimePicker().Time(std::chrono::hours(g_Settings.scheduleDayHour) + std::chrono::minutes(g_Settings.scheduleDayMinute));
        ScheduleNightTimePicker().Time(std::chrono::hours(g_Settings.scheduleNightHour) + std::chrono::minutes(g_Settings.scheduleNightMinute));
        ScheduleSunSourceComboBox().SelectedIndex(g_Settings.scheduleSunSource == 1 ? 1 : 0);
        ScheduleLatNumberBox().Value(g_Settings.scheduleLatitude);
        ScheduleLonNumberBox().Value(g_Settings.scheduleLongitude);
        ScheduleSunriseOffsetNumberBox().Value(g_Settings.scheduleSunriseOffset);
        ScheduleSunsetOffsetNumberBox().Value(g_Settings.scheduleSunsetOffset);
        UpdateScheduleVisibility();
        UpdateScheduleSunPreview();

        // Start with Windows
        bool isInstalled = AppSettings::IsInstalledInProgramFiles();
        bool startWithWin = g_Settings.startWithWindows || AppSettings::IsStartWithWindowsEnabled();
        StartWithWindowsToggle().IsEnabled(isInstalled);
        StartWithWindowsToggle().IsOn(startWithWin);
        StartWithWindowsStatusText().Text(startWithWin ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

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
        loadColorToUI(OverlayDarkColorPicker(), OverlayDarkColorTextBox(), OverlayDarkColorPreview(), g_Settings.overlayAccentColorDark);
        loadColorToUI(TrayIconColorPicker(), TrayIconColorTextBox(), TrayIconColorPreview(), g_Settings.trayIconColor);
        loadColorToUI(TrayIconDarkColorPicker(), TrayIconDarkColorTextBox(), TrayIconDarkColorPreview(), g_Settings.trayIconColorDark);

        FlyoutUseWindowsAccentToggle().IsOn(g_Settings.flyoutUseWindowsAccent);
        FlyoutUseWindowsAccentStatusText().Text(g_Settings.flyoutUseWindowsAccent ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        FlyoutDualColorToggle().IsOn(g_Settings.flyoutUseDualColors);
        FlyoutDualColorStatusText().Text(g_Settings.flyoutUseDualColors ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        FlyoutAccentColorCard().Visibility(g_Settings.flyoutUseWindowsAccent ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);
        FlyoutDualColorCard().Visibility(g_Settings.flyoutUseWindowsAccent ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible);
        FlyoutDarkColorCard().Visibility((!g_Settings.flyoutUseWindowsAccent && g_Settings.flyoutUseDualColors) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        FlyoutCustomAccentTitleText().Text(g_Settings.flyoutUseDualColors
            ? g_Loc.Get(L"Opt_TrayIconColorLight", L"Light Mode Color")
            : g_Loc.Get(L"Opt_FlyoutCustomAccent", L"Custom Accent Color"));
        loadColorToUI(FlyoutAccentColorPicker(), FlyoutAccentColorTextBox(), FlyoutAccentColorPreview(), g_Settings.flyoutAccentColor);
        loadColorToUI(FlyoutDarkColorPicker(), FlyoutDarkColorTextBox(), FlyoutDarkColorPreview(), g_Settings.flyoutAccentColorDark);

        FlyoutScrollEnabledToggle().IsOn(g_Settings.flyoutScrollEnabled);
        FlyoutScrollEnabledStatusText().Text(g_Settings.flyoutScrollEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));
        FlyoutScrollStepCard().Visibility(g_Settings.flyoutScrollEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        FlyoutScrollDelayCard().Visibility(g_Settings.flyoutScrollEnabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        FlyoutScrollStepComboBox().SelectedIndex(stepToIndex(g_Settings.flyoutScrollStep));

        FlyoutKeyboardEnabledToggle().IsOn(g_Settings.flyoutKeyboardEnabled);
        FlyoutKeyboardEnabledStatusText().Text(g_Settings.flyoutKeyboardEnabled ? g_Loc.Get(L"Common_On", L"On") : g_Loc.Get(L"Common_Off", L"Off"));

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

    void SettingsWindow::UpdateScheduleVisibility()
    {
        bool enabled = (g_Settings.scheduleMode != 0);
        bool isCustom = (g_Settings.scheduleMode == 1);
        bool isSun = (g_Settings.scheduleMode == 2);
        bool isLocService = (g_Settings.scheduleSunSource == 0);

        ScheduleShowOsdCard().Visibility(enabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        ScheduleBrightnessSection().Visibility(enabled ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        CustomScheduleSection().Visibility(isCustom ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        SunScheduleSection().Visibility(isSun ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        ScheduleLocationStatusCard().Visibility(isLocService ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        ScheduleLatCard().Visibility(!isLocService ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        ScheduleLonCard().Visibility(!isLocService ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    }

    void SettingsWindow::UpdateScheduleSunPreview()
    {
        wchar_t locBuf[96];
        swprintf_s(locBuf, L"%.4f, %.4f", g_Settings.scheduleLatitude, g_Settings.scheduleLongitude);
        ScheduleLocationText().Text(locBuf);

        auto info = ScheduleManager::GetTodaySunInfo(
            g_Settings.scheduleLatitude,
            g_Settings.scheduleLongitude,
            g_Settings.scheduleSunriseOffset,
            g_Settings.scheduleSunsetOffset);
        if (info.valid)
        {
            wchar_t sunBuf[160];
            if (g_Settings.scheduleSunriseOffset != 0 || g_Settings.scheduleSunsetOffset != 0)
            {
                swprintf_s(sunBuf, L"Sunrise: %02d:%02d  |  Sunset: %02d:%02d  (Effective: %02d:%02d  |  %02d:%02d)",
                    info.riseHour, info.riseMin, info.setHour, info.setMin,
                    info.effectiveRiseHour, info.effectiveRiseMin, info.effectiveSetHour, info.effectiveSetMin);
            }
            else
            {
                swprintf_s(sunBuf, L"Sunrise: %02d:%02d  |  Sunset: %02d:%02d",
                    info.riseHour, info.riseMin, info.setHour, info.setMin);
            }
            ScheduleSunTimesText().Text(sunBuf);
        }
        else
        {
            ScheduleSunTimesText().Text(L"Polar day / night (no sunrise/sunset today)");
        }
    }

    winrt::fire_and_forget SettingsWindow::DetectScheduleLocationAsync()
    {
        auto weak = get_weak();
        try
        {
            auto access = co_await winrt::Windows::Devices::Geolocation::Geolocator::RequestAccessAsync();
            if (access == winrt::Windows::Devices::Geolocation::GeolocationAccessStatus::Allowed)
            {
                winrt::Windows::Devices::Geolocation::Geolocator geolocator;
                auto pos = co_await geolocator.GetGeopositionAsync();
                auto self = weak.get();
                if (!self) co_return;
                double lat = pos.Coordinate().Point().Position().Latitude;
                double lon = pos.Coordinate().Point().Position().Longitude;
                g_Settings.scheduleLatitude = lat;
                g_Settings.scheduleLongitude = lon;
                bool prev = m_isUpdatingUI;
                m_isUpdatingUI = true;
                ScheduleLatNumberBox().Value(lat);
                ScheduleLonNumberBox().Value(lon);
                m_isUpdatingUI = prev;
                UpdateScheduleSunPreview();
                ApplyAndSave();
                ScheduleManager::Refresh();
            }
            else
            {
                auto self = weak.get();
                if (self)
                {
                    ScheduleLocationText().Text(L"Location permission denied (using saved coordinates)");
                }
            }
        }
        catch (...)
        {
            auto self = weak.get();
            if (self)
            {
                ScheduleLocationText().Text(L"Could not detect location (using saved coordinates)");
            }
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
        g_MonitorManager.RefreshExternalPowerStatesAsync();
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
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    }).detach();
}
