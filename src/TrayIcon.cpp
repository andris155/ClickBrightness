#include "TrayIcon.h"
#include "Settings.h"
#include "LocalizationManager.h"
#include "ThemeHelper.h"
#include "App.xaml.h"
#include "resource.h"
#include "DpiHelper.h"
#include <strsafe.h>
#include <memory>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <dwmapi.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

TrayIcon g_TrayIcon;

static winrt::Microsoft::UI::Xaml::Window s_trayMenuWindow{ nullptr };
static winrt::Microsoft::UI::Xaml::Controls::Grid s_trayMenuRoot{ nullptr };
static winrt::Microsoft::UI::Xaml::Controls::MenuFlyout s_trayMenuFlyout{ nullptr };
static HWND s_hTrayMenuWnd = NULL;
static std::atomic<bool> s_isMenuOpen{ false };
static std::atomic<ULONGLONG> s_lastMenuCloseTick{ 0 };
static HHOOK s_hTrayWheelHook = NULL;
static void InitTrayMenu();

static LRESULT CALLBACK TrayWheelHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL)
    {
        if (g_Settings.trayWheelEnabled)
        {
            PMSLLHOOKSTRUCT p = reinterpret_cast<PMSLLHOOKSTRUCT>(lParam);
            if (p)
            {
                HWND hOwner = g_TrayIcon.GetOwnerWnd();
                if (hOwner)
                {
                    NOTIFYICONIDENTIFIER nid = { sizeof(NOTIFYICONIDENTIFIER) };
                    nid.hWnd = hOwner;
                    nid.uID = g_TrayIcon.GetId();
                    RECT rcIcon = {};
                    if (SUCCEEDED(Shell_NotifyIconGetRect(&nid, &rcIcon)))
                    {
                        if (PtInRect(&rcIcon, p->pt))
                        {
                            short delta = static_cast<short>(HIWORD(p->mouseData));
                            if (delta != 0)
                            {
                                int step = (delta > 0) ? g_Settings.trayWheelStep : -g_Settings.trayWheelStep;
                                PostMessageW(hOwner, WM_APP_TRAY_WHEEL, static_cast<WPARAM>(step), 0);
                                return 1;
                            }
                        }
                    }
                }
            }
        }
    }
    return CallNextHookEx(s_hTrayWheelHook, nCode, wParam, lParam);
}

TrayIcon::TrayIcon()
    : m_hOwnerWnd(NULL), m_uId(1), m_uCallbackMsg(0),
      m_hBaseSunIcon(NULL),
      m_lastBrightness(-1), m_created(false),
      m_lastTrayWasLight(false) {
    memset(m_iconCache, 0, sizeof(m_iconCache));
}

TrayIcon::~TrayIcon() {
    Destroy();
}

void TrayIcon::InitD2D() {
    if (!m_pD2DFactory) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_pD2DFactory.GetAddressOf());
    }
    if (!m_pDWriteFactory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)m_pDWriteFactory.GetAddressOf());
    }
    if (!m_pRenderingParams && m_pDWriteFactory) {
        m_pDWriteFactory->CreateCustomRenderingParams(
            1.8f, // Gamma matching Windows 11 XAML/clock rendering
            0.5f, // Enhanced contrast matching Windows 11 default
            0.0f, // ClearType level
            DWRITE_PIXEL_GEOMETRY_FLAT,
            DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
            m_pRenderingParams.GetAddressOf()
        );
    }
    if (!m_pDCRenderTarget && m_pD2DFactory) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0, 0,
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT
        );
        m_pD2DFactory->CreateDCRenderTarget(&props, m_pDCRenderTarget.GetAddressOf());
        if (m_pDCRenderTarget && m_pRenderingParams) {
            m_pDCRenderTarget->SetTextRenderingParams(m_pRenderingParams.Get());
        }
    }
    if (m_pDCRenderTarget && !m_pBrush) {
        m_pDCRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), m_pBrush.GetAddressOf());
    }
}

void TrayIcon::CleanupD2D() {
    m_pBrush.Reset();
    m_pRenderingParams.Reset();
    m_pDCRenderTarget.Reset();
    m_pDWriteFactory.Reset();
    m_pD2DFactory.Reset();
}

bool TrayIcon::IsTrayColorLight() {
    DWORD data = 0;
    DWORD dataSize = sizeof(data);
    LONG res = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme",
        RRF_RT_REG_DWORD,
        NULL,
        &data,
        &dataSize
    );
    if (res == ERROR_SUCCESS) {
        return (data != 0);
    }
    return false;
}

void TrayIcon::InvalidateIconCache() {
    for (int i = 0; i <= 100; ++i) {
        if (m_iconCache[i]) {
            DestroyIcon(m_iconCache[i]);
            m_iconCache[i] = NULL;
        }
    }
}

void TrayIcon::ResetGraphicsResources() {
    InvalidateIconCache();
    CleanupD2D();
    CloseTrayMenu();
}

void TrayIcon::OnThemeChanged() {
    InvalidateIconCache();
    m_lastTrayWasLight = IsTrayColorLight();
    if (m_created) {
        int b = (m_lastBrightness >= 0) ? m_lastBrightness : g_Settings.cachedBrightness;
        UpdateBrightness(b);
    }
    if (s_trayMenuRoot && s_trayMenuFlyout) {
        bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
        auto theme = isDark ? winrt::Microsoft::UI::Xaml::ElementTheme::Dark : winrt::Microsoft::UI::Xaml::ElementTheme::Light;
        s_trayMenuRoot.RequestedTheme(theme);
        for (auto item : s_trayMenuFlyout.Items()) {
            if (auto fe = item.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>()) {
                fe.RequestedTheme(theme);
            }
        }
    }
}

void TrayIcon::OnLanguageChanged() {
    if (m_created) {
        int b = (m_lastBrightness >= 0) ? m_lastBrightness : g_Settings.cachedBrightness;
        UpdateBrightness(b);
    }
    if (s_trayMenuFlyout) {
        auto items = s_trayMenuFlyout.Items();
        if (items.Size() >= 4) {
            if (auto optItem = items.GetAt(0).try_as<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem>()) {
                optItem.Text(g_Loc.Get(L"Tray_Options", L"Options..."));
            }
            if (auto aboutItem = items.GetAt(2).try_as<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem>()) {
                aboutItem.Text(g_Loc.Get(L"Tray_About", L"ClickBrightness v1.0.8"));
            }
            if (auto exitItem = items.GetAt(3).try_as<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem>()) {
                exitItem.Text(g_Loc.Get(L"Tray_Exit", L"Exit"));
            }
        }
    }
}

bool TrayIcon::Create(HWND hOwnerWnd, UINT uCallbackMsg, UINT uId) {
    m_hOwnerWnd = hOwnerWnd;
    m_uCallbackMsg = uCallbackMsg;
    m_uId = uId;
    m_lastTrayWasLight = IsTrayColorLight();

    // Load base sun icon from resources
    if (m_hBaseSunIcon) {
        DestroyIcon(m_hBaseSunIcon);
        m_hBaseSunIcon = NULL;
    }
    HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
    UINT dpi = hTaskbar ? GetDpiForWindow(hTaskbar) : GetDpiForSystem();
    int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    if (iconSize <= 0) iconSize = 16;
    m_hBaseSunIcon = (HICON)LoadImageW(
        GetModuleHandleW(NULL),
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        iconSize,
        iconSize,
        LR_DEFAULTCOLOR
    );

    NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW) };
    nid.hWnd = m_hOwnerWnd;
    nid.uID = m_uId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = m_uCallbackMsg;
    nid.hIcon = m_hBaseSunIcon ? m_hBaseSunIcon : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    std::wstring tipFmt = g_Loc.Get(L"Tray_Tooltip", L"Brightness: %d%%");
    StringCchPrintfW(nid.szTip, ARRAYSIZE(nid.szTip), tipFmt.c_str(), m_lastBrightness >= 0 ? m_lastBrightness : g_Settings.cachedBrightness);

    m_created = (Shell_NotifyIconW(NIM_ADD, &nid) == TRUE);
    if (m_created) {
        // Request version 4 behavior
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        InitTrayMenu();
        UpdateWheelHook();
        return true;
    }
    return false;
}

void TrayIcon::Destroy() {
    if (s_hTrayWheelHook) {
        UnhookWindowsHookEx(s_hTrayWheelHook);
        s_hTrayWheelHook = NULL;
    }
    if (m_created) {
        NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW) };
        nid.hWnd = m_hOwnerWnd;
        nid.uID = m_uId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        m_created = false;
    }
    InvalidateIconCache();
    if (m_hBaseSunIcon) {
        DestroyIcon(m_hBaseSunIcon);
        m_hBaseSunIcon = NULL;
    }
    CleanupD2D();
    CloseTrayMenu();
}

void TrayIcon::UpdateWheelHook() {
    if (g_Settings.trayWheelEnabled && m_created) {
        if (!s_hTrayWheelHook) {
            s_hTrayWheelHook = SetWindowsHookExW(WH_MOUSE_LL, TrayWheelHookProc, GetModuleHandleW(NULL), 0);
        }
    } else {
        if (s_hTrayWheelHook) {
            UnhookWindowsHookEx(s_hTrayWheelHook);
            s_hTrayWheelHook = NULL;
        }
    }
}

// Render brightness number onto native-resolution icon using DirectWrite for Start Menu sharpness
HICON TrayIcon::CreateBrightnessIcon(int brightness) {
    InitD2D();
    if (!m_pDCRenderTarget || !m_pDWriteFactory) return NULL;

    HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
    UINT dpi = hTaskbar ? GetDpiForWindow(hTaskbar) : GetDpiForSystem();
    int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    if (size <= 0) size = 16;

    wchar_t text[16];
    swprintf_s(text, L"%d", brightness);
    int textLen = (int)wcslen(text);

    // Create 32-bit DIB section
    BITMAPINFO bmi = { sizeof(BITMAPINFOHEADER) };
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    HDC hdcMem = CreateCompatibleDC(NULL);
    if (!hdcMem) return NULL;

    HBITMAP hBmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hBmp) {
        DeleteDC(hdcMem);
        return NULL;
    }
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBmp);

    RECT rc = { 0, 0, size, size };
    m_pDCRenderTarget->BindDC(hdcMem, &rc);

    std::wstring fontName = !g_Settings.trayFont.empty() ? g_Settings.trayFont : L"Segoe UI Variable Text";

    int w = (g_Settings.trayFontWeight >= 100 && g_Settings.trayFontWeight <= 900) ? g_Settings.trayFontWeight : 375;
    DWRITE_FONT_WEIGHT fontWeight = static_cast<DWRITE_FONT_WEIGHT>(w);

    // Fallback: if a legacy font name still has a weight suffix, strip it and apply weight
    auto stripWeightSuffix = [&fontName](const wchar_t* suffix) {
        size_t pos = fontName.rfind(suffix);
        if (pos != std::wstring::npos && pos + wcslen(suffix) == fontName.length()) {
            fontName.erase(pos);
            return true;
        }
        return false;
    };
    if (stripWeightSuffix(L" Semilight") || stripWeightSuffix(L" SemiLight")) fontWeight = DWRITE_FONT_WEIGHT_SEMI_LIGHT;
    else if (stripWeightSuffix(L" Light")) fontWeight = DWRITE_FONT_WEIGHT_LIGHT;
    else if (stripWeightSuffix(L" Semibold") || stripWeightSuffix(L" SemiBold")) fontWeight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    else if (stripWeightSuffix(L" Medium")) fontWeight = DWRITE_FONT_WEIGHT_MEDIUM;
    else if (stripWeightSuffix(L" Bold")) fontWeight = DWRITE_FONT_WEIGHT_BOLD;

    // Font size calibrated for authentic Windows 11 taskbar typography:
    float scale = (g_Settings.trayFontSize >= 40 && g_Settings.trayFontSize <= 150)
        ? (g_Settings.trayFontSize / 100.0f) : 0.88f;
    float fontSize = size * scale;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> pFormat;
    HRESULT hr = m_pDWriteFactory->CreateTextFormat(
        fontName.c_str(),
        NULL,
        fontWeight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontSize,
        L"en-US",
        pFormat.GetAddressOf()
    );

    if (FAILED(hr) || !pFormat) {
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text",
            NULL,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            fontSize,
            L"en-US",
            pFormat.GetAddressOf()
        );
    }

    if (pFormat) {
        Microsoft::WRL::ComPtr<IDWriteTextFormat3> pFormat3;
        if (SUCCEEDED(pFormat.As(&pFormat3)) && pFormat3) {
            DWRITE_FONT_AXIS_VALUE axisValue = {};
            axisValue.axisTag = DWRITE_FONT_AXIS_TAG_WEIGHT;
            axisValue.value = static_cast<float>(fontWeight);
            pFormat3->SetFontAxisValues(&axisValue, 1);
        }
        pFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        pFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        pFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    D2D1_COLOR_F textColor;
    if (g_Settings.trayPercentageMode == 1) {
        COLORREF col = (g_Settings.trayIconUseDualColors && !m_lastTrayWasLight)
            ? g_Settings.trayIconColorDark
            : g_Settings.trayIconColor;
        textColor = D2D1::ColorF(
            GetRValue(col) / 255.0f,
            GetGValue(col) / 255.0f,
            GetBValue(col) / 255.0f,
            1.0f
        );
    } else {
        bool isLight = m_lastTrayWasLight;
        textColor = isLight
            ? D2D1::ColorF(0.08f, 0.08f, 0.08f, 1.0f)   // Fluent dark charcoal text for light taskbar
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);     // Crisp white text for dark taskbar
    }

    if (m_pBrush) {
        m_pBrush->SetColor(textColor);
    }

    m_pDCRenderTarget->BeginDraw();
    m_pDCRenderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // Pure Grayscale antialiasing matching the Windows 11 Start Menu and system glyphs
    m_pDCRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    if (pFormat && m_pBrush) {
        float yOffset = (float)g_Settings.trayYOffset * (size / 16.0f);
        if (textLen >= 3) {
            // Restore stretched condensed look for 100%: full height, horizontally condensed
            float scaleX = (g_Settings.trayFontSize100 >= 30 && g_Settings.trayFontSize100 <= 130)
                ? (g_Settings.trayFontSize100 / 100.0f) : 0.65f;
            float virtualWidth = (float)size / scaleX;
            m_pDCRenderTarget->SetTransform(D2D1::Matrix3x2F::Scale(scaleX, 1.0f, D2D1::Point2F(0.0f, 0.0f)));
            D2D1_RECT_F layoutRect = D2D1::RectF(0.0f, yOffset, virtualWidth, (float)size + yOffset);
            m_pDCRenderTarget->DrawText(text, textLen, pFormat.Get(), layoutRect, m_pBrush.Get());
            m_pDCRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
        } else {
            D2D1_RECT_F layoutRect = D2D1::RectF(0.0f, yOffset, (float)size, (float)size + yOffset);
            m_pDCRenderTarget->DrawText(text, textLen, pFormat.Get(), layoutRect, m_pBrush.Get());
        }
    }

    hr = m_pDCRenderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        m_pDCRenderTarget.Reset();
        m_pBrush.Reset();
    }

    SelectObject(hdcMem, hOldBmp);

    // Un-premultiply BGRA pixels: Direct2D DCRenderTarget outputs premultiplied alpha,
    // while the Windows 11 taskbar HICON pipeline treats 32bpp DIBs as straight alpha
    // (otherwise bright colors get double-premultiplied at edges, creating a dark outline).
    if (pBits) {
        BYTE* px = static_cast<BYTE*>(pBits);
        int totalPixels = size * size;
        for (int i = 0; i < totalPixels; ++i) {
            BYTE a = px[3];
            if (a > 0 && a < 255) {
                for (int c = 0; c < 3; ++c) {
                    int val = (px[c] * 255 + a / 2) / a;
                    px[c] = static_cast<BYTE>(val > 255 ? 255 : val);
                }
            }
            px += 4;
        }
    }

    int maskBytes = (size * size + 7) / 8;
    BYTE maskStack[512] = { 0 };
    BYTE* pMaskBits = maskStack;
    std::unique_ptr<BYTE[]> heapMask;
    if (maskBytes > (int)sizeof(maskStack)) {
        heapMask = std::make_unique<BYTE[]>(maskBytes);
        pMaskBits = heapMask.get();
        memset(pMaskBits, 0, maskBytes);
    }
    HBITMAP hMask = CreateBitmap(size, size, 1, 1, pMaskBits);

    ICONINFO ii = { 0 };
    ii.fIcon = TRUE;
    ii.hbmColor = hBmp;
    ii.hbmMask = hMask;
    HICON hIcon = CreateIconIndirect(&ii);

    if (hMask) DeleteObject(hMask);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);

    return hIcon;
}

bool TrayIcon::UpdateBrightness(int brightness) {
    if (!m_created) return false;

    if (brightness < 0) brightness = 0;
    if (brightness > 100) brightness = 100;
    m_lastBrightness = brightness;

    HICON hNewIcon = NULL;
    if (g_Settings.trayPercentageMode != 2) {
        hNewIcon = m_iconCache[brightness];
        if (!hNewIcon) {
            hNewIcon = CreateBrightnessIcon(brightness);
            m_iconCache[brightness] = hNewIcon;
        }
    } else {
        if (!m_hBaseSunIcon) {
            HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
            UINT dpi = hTaskbar ? GetDpiForWindow(hTaskbar) : GetDpiForSystem();
            int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
            if (iconSize <= 0) iconSize = 16;
            m_hBaseSunIcon = (HICON)LoadImageW(
                GetModuleHandleW(NULL),
                MAKEINTRESOURCEW(IDI_APP_ICON),
                IMAGE_ICON,
                iconSize,
                iconSize,
                LR_DEFAULTCOLOR
            );
        }
        hNewIcon = m_hBaseSunIcon ? m_hBaseSunIcon : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    }
    if (!hNewIcon) return false;

    NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW) };
    nid.hWnd = m_hOwnerWnd;
    nid.uID = m_uId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = hNewIcon;
    std::wstring tipFmt = g_Loc.Get(L"Tray_Tooltip", L"Brightness: %d%%");
    StringCchPrintfW(nid.szTip, ARRAYSIZE(nid.szTip), tipFmt.c_str(), brightness);

    bool ok = (Shell_NotifyIconW(NIM_MODIFY, &nid) == TRUE);

    return ok;
}

static void InitTrayMenu()
{
    if (s_trayMenuWindow) return;

    try
    {
        s_trayMenuWindow = winrt::Microsoft::UI::Xaml::Window();
        auto windowNative = s_trayMenuWindow.try_as<::IWindowNative>();
        if (windowNative)
        {
            windowNative->get_WindowHandle(&s_hTrayMenuWnd);
        }

        if (s_hTrayMenuWnd)
        {
            auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(s_hTrayMenuWnd);
            auto appWnd = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);
            if (appWnd)
            {
                auto p = appWnd.Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>();
                if (p)
                {
                    p.SetBorderAndTitleBar(false, false);
                    p.IsAlwaysOnTop(true);
                    p.IsResizable(false);
                    p.IsMinimizable(false);
                    p.IsMaximizable(false);
                }
            }

            // Strip window decorations to completely eliminate frame / circle artifacts
            LONG_PTR style = GetWindowLongPtrW(s_hTrayMenuWnd, GWL_STYLE);
            style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
            style |= WS_POPUP;
            SetWindowLongPtrW(s_hTrayMenuWnd, GWL_STYLE, style);

            LONG_PTR exStyle = GetWindowLongPtrW(s_hTrayMenuWnd, GWL_EXSTYLE);
            exStyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME);
            exStyle |= WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED;
            SetWindowLongPtrW(s_hTrayMenuWnd, GWL_EXSTYLE, exStyle);
            SetLayeredWindowAttributes(s_hTrayMenuWnd, 0, 0, LWA_ALPHA);

            COLORREF borderColor = DWMWA_COLOR_NONE;
            DwmSetWindowAttribute(s_hTrayMenuWnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));

            DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_DONOTROUND;
            DwmSetWindowAttribute(s_hTrayMenuWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

            int ncPolicy = DWMNCRP_DISABLED;
            DwmSetWindowAttribute(s_hTrayMenuWnd, DWMWA_NCRENDERING_POLICY, &ncPolicy, sizeof(ncPolicy));

            BOOL forceDisableTransitions = TRUE;
            DwmSetWindowAttribute(s_hTrayMenuWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &forceDisableTransitions, sizeof(forceDisableTransitions));

            HRGN hEmptyRgn = CreateRectRgn(0, 0, 0, 0);
            SetWindowRgn(s_hTrayMenuWnd, hEmptyRgn, TRUE);
        }

        s_trayMenuRoot = winrt::Microsoft::UI::Xaml::Controls::Grid();
        s_trayMenuRoot.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Transparent()));
        s_trayMenuRoot.Opacity(0.0);
        s_trayMenuWindow.Content(s_trayMenuRoot);

        s_trayMenuFlyout = winrt::Microsoft::UI::Xaml::Controls::MenuFlyout();

        // 1. Options...
        auto optionsItem = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem();
        optionsItem.Text(g_Loc.Get(L"Tray_Options", L"Options..."));
        optionsItem.Height(32.0);
        optionsItem.Padding({ 11.0, 4.0, 11.0, 5.0 });
        auto iconSettings = winrt::Microsoft::UI::Xaml::Controls::FontIcon();
        iconSettings.Glyph(L"\xE713");
        optionsItem.Icon(iconSettings);
        optionsItem.Click([](auto&&, auto&&)
        {
            ShowSettingsWindow();
        });
        s_trayMenuFlyout.Items().Append(optionsItem);

        // Separator
        s_trayMenuFlyout.Items().Append(winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator());

        // 2. ClickBrightness v1.0.8 (About info)
        auto aboutItem = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem();
        aboutItem.Text(g_Loc.Get(L"Tray_About", L"ClickBrightness v1.0.8"));
        aboutItem.Height(32.0);
        aboutItem.Padding({ 11.0, 4.0, 11.0, 5.0 });
        auto iconInfo = winrt::Microsoft::UI::Xaml::Controls::FontIcon();
        iconInfo.Glyph(L"\xE946");
        aboutItem.Icon(iconInfo);
        aboutItem.IsEnabled(false);
        s_trayMenuFlyout.Items().Append(aboutItem);

        // 3. Exit
        auto exitItem = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem();
        exitItem.Text(g_Loc.Get(L"Tray_Exit", L"Exit"));
        exitItem.Height(32.0);
        exitItem.Padding({ 11.0, 4.0, 11.0, 5.0 });
        auto iconExit = winrt::Microsoft::UI::Xaml::Controls::FontIcon();
        iconExit.Glyph(L"\xE7E8");
        exitItem.Icon(iconExit);
        exitItem.Click([](auto&&, auto&&)
        {
            if (s_trayMenuFlyout)
            {
                s_trayMenuFlyout.Hide();
            }
            HWND hMain = FindWindowW(L"ClickBrightness_MessageWndClass", L"ClickBrightness_HiddenMsgWindow");
            if (!hMain)
            {
                hMain = FindWindowW(L"ClickBrightness_MessageWndClass", NULL);
            }
            if (hMain)
            {
                PostMessageW(hMain, WM_COMMAND, MAKEWPARAM(IDM_TRAY_EXIT, 0), 0);
            }
            else
            {
                ExitApp();
            }
        });
        s_trayMenuFlyout.Items().Append(exitItem);

        bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
        auto theme = isDark ? winrt::Microsoft::UI::Xaml::ElementTheme::Dark : winrt::Microsoft::UI::Xaml::ElementTheme::Light;
        s_trayMenuRoot.RequestedTheme(theme);
        for (auto item : s_trayMenuFlyout.Items())
        {
            if (auto fe = item.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
            {
                fe.RequestedTheme(theme);
            }
        }

        if (g_Settings.transparencyEnabled)
        {
            s_trayMenuFlyout.MenuFlyoutPresenterStyle(nullptr);
        }
        else
        {
            winrt::Microsoft::UI::Xaml::Style presenterStyle(winrt::xaml_typename<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutPresenter>());
            winrt::Windows::UI::Color solidBg = isDark
                ? winrt::Microsoft::UI::ColorHelper::FromArgb(255, 44, 44, 44)
                : winrt::Microsoft::UI::ColorHelper::FromArgb(255, 243, 243, 243);
            presenterStyle.Setters().Append(winrt::Microsoft::UI::Xaml::Setter(
                winrt::Microsoft::UI::Xaml::Controls::Control::BackgroundProperty(),
                winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(solidBg)
            ));
            s_trayMenuFlyout.MenuFlyoutPresenterStyle(presenterStyle);
        }

        s_trayMenuFlyout.Opened([](auto&&, auto&&)
        {
            s_isMenuOpen = true;
        });

        s_trayMenuFlyout.Closed([](auto&&, auto&&)
        {
            s_isMenuOpen = false;
            s_lastMenuCloseTick = GetTickCount64();
            if (s_hTrayMenuWnd)
            {
                ShowWindow(s_hTrayMenuWnd, SW_HIDE);
            }
        });

        // Position window offscreen first so pre-activation never flickers or steals focus
        if (s_hTrayMenuWnd)
        {
            SetWindowPos(s_hTrayMenuWnd, HWND_TOPMOST, -32000, -32000, 1, 1, SWP_NOACTIVATE | SWP_HIDEWINDOW);
        }

        HWND hPrevFocus = GetForegroundWindow();
        s_trayMenuWindow.Activate();
        if (s_hTrayMenuWnd)
        {
            ShowWindow(s_hTrayMenuWnd, SW_HIDE);
        }
        if (hPrevFocus && hPrevFocus != s_hTrayMenuWnd)
        {
            SetForegroundWindow(hPrevFocus);
        }
    }
    catch (...)
    {
    }
}

void TrayIcon::ShowContextMenu() {
    try
    {
        InitTrayMenu();
        if (!s_hTrayMenuWnd || !s_trayMenuFlyout || !s_trayMenuRoot) return;

        // 1. If menu is already open, right-clicking toggles it closed cleanly
        if (s_isMenuOpen.load() || s_trayMenuFlyout.IsOpen())
        {
            try
            {
                s_trayMenuFlyout.Hide();
            }
            catch (...) {}
            s_isMenuOpen = false;
            return;
        }

        // 2. Debounce: if the menu closed very recently, avoid re-opening during exit animation
        ULONGLONG now = GetTickCount64();
        if (now - s_lastMenuCloseTick.load() < 250)
        {
            return;
        }

        RECT rcIcon = { 0 };
        NOTIFYICONIDENTIFIER nii = { sizeof(NOTIFYICONIDENTIFIER) };
        nii.hWnd = m_hOwnerWnd;
        nii.uID = m_uId;
        HRESULT hr = Shell_NotifyIconGetRect(&nii, &rcIcon);

        int iconX = 0, iconY = 0, iconW = 32, iconH = 32;
        if (SUCCEEDED(hr) && (rcIcon.right > rcIcon.left) && (rcIcon.bottom > rcIcon.top))
        {
            iconX = rcIcon.left;
            iconY = rcIcon.top;
            iconW = rcIcon.right - rcIcon.left;
            iconH = rcIcon.bottom - rcIcon.top;
        }
        else
        {
            POINT pt;
            GetCursorPos(&pt);
            iconX = pt.x - 16;
            iconY = pt.y - 16;
            iconW = 32;
            iconH = 32;
        }

        int iconCenterX = iconX + iconW / 2;

        s_trayMenuRoot.Width(1);
        s_trayMenuRoot.Height(1);

        bool isDark = ThemeHelper::IsOverlayInDarkMode(g_Settings.overlayTheme);
        auto theme = isDark ? winrt::Microsoft::UI::Xaml::ElementTheme::Dark : winrt::Microsoft::UI::Xaml::ElementTheme::Light;
        s_trayMenuRoot.RequestedTheme(theme);
        for (auto item : s_trayMenuFlyout.Items())
        {
            if (auto fe = item.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
            {
                fe.RequestedTheme(theme);
            }
        }

        if (g_Settings.transparencyEnabled)
        {
            s_trayMenuFlyout.MenuFlyoutPresenterStyle(nullptr);
        }
        else
        {
            winrt::Microsoft::UI::Xaml::Style presenterStyle(winrt::xaml_typename<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutPresenter>());
            winrt::Windows::UI::Color solidBg = isDark
                ? winrt::Microsoft::UI::ColorHelper::FromArgb(255, 44, 44, 44)
                : winrt::Microsoft::UI::ColorHelper::FromArgb(255, 243, 243, 243);
            presenterStyle.Setters().Append(winrt::Microsoft::UI::Xaml::Setter(
                winrt::Microsoft::UI::Xaml::Controls::Control::BackgroundProperty(),
                winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(solidBg)
            ));
            s_trayMenuFlyout.MenuFlyoutPresenterStyle(presenterStyle);
        }

        // Detect taskbar docking edge (supporting Left, Right, Top, Bottom across multi-monitors)
        enum class TaskbarEdge { Bottom, Top, Left, Right };
        TaskbarEdge edge = TaskbarEdge::Bottom;

        POINT ptIcon = { iconCenterX, iconY };
        HMONITOR hMon = MonitorFromPoint(ptIcon, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi))
        {
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
                int distLeft = abs(iconCenterX - mi.rcMonitor.left);
                int distRight = abs(mi.rcMonitor.right - iconCenterX);
                int distTop = abs(iconY - mi.rcMonitor.top);
                int distBottom = abs(mi.rcMonitor.bottom - iconY);

                int minDist = distBottom;
                edge = TaskbarEdge::Bottom;
                if (distTop < minDist) { minDist = distTop; edge = TaskbarEdge::Top; }
                if (distLeft < minDist) { minDist = distLeft; edge = TaskbarEdge::Left; }
                if (distRight < minDist) { minDist = distRight; edge = TaskbarEdge::Right; }
            }
        }

        // Position 1x1 anchor window just outside the tray icon boundary so it never covers the icon
        int anchorX = iconCenterX;
        int anchorY = iconY - 1;
        winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode placement =
            winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Top;

        switch (edge)
        {
        case TaskbarEdge::Left:
            anchorX = iconX + iconW + 1;
            anchorY = iconY + iconH / 2;
            placement = winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Right;
            break;
        case TaskbarEdge::Right:
            anchorX = iconX - 1;
            anchorY = iconY + iconH / 2;
            placement = winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Left;
            break;
        case TaskbarEdge::Top:
            anchorX = iconCenterX;
            anchorY = iconY + iconH + 1;
            placement = winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Bottom;
            break;
        case TaskbarEdge::Bottom:
        default:
            anchorX = iconCenterX;
            anchorY = iconY;
            placement = winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Top;
            break;
        }

        SetWindowPos(s_hTrayMenuWnd, HWND_TOPMOST, anchorX, anchorY, 1, 1, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        ShowWindow(s_hTrayMenuWnd, SW_SHOWNOACTIVATE);

        // Ensure anchor window remains 100% invisible without rendering a pixel/dot
        HRGN hRgn = CreateRectRgn(0, 0, 0, 0);
        SetWindowRgn(s_hTrayMenuWnd, hRgn, TRUE);
        SetLayeredWindowAttributes(s_hTrayMenuWnd, 0, 0, LWA_ALPHA);

        SetForegroundWindow(s_hTrayMenuWnd);

        // Dynamically refresh tray menu items with current language
        auto menuItems = s_trayMenuFlyout.Items();
        if (menuItems.Size() >= 4)
        {
            if (auto optItem = menuItems.GetAt(0).try_as<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem>())
            {
                optItem.Text(g_Loc.Get(L"Tray_Options", L"Options..."));
            }
            if (auto aboutItem = menuItems.GetAt(2).try_as<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem>())
            {
                aboutItem.Text(g_Loc.Get(L"Tray_About", L"ClickBrightness v1.0.8"));
            }
            if (auto exitItem = menuItems.GetAt(3).try_as<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem>())
            {
                exitItem.Text(g_Loc.Get(L"Tray_Exit", L"Exit"));
            }
        }

        winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutShowOptions options;
        options.Placement(placement);
        options.ShowMode(winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutShowMode::Standard);
        s_trayMenuFlyout.ShowAt(s_trayMenuRoot, options);
        s_isMenuOpen = true;
    }
    catch (winrt::hresult_error const&)
    {
        s_isMenuOpen = false;
        if (s_hTrayMenuWnd)
        {
            ShowWindow(s_hTrayMenuWnd, SW_HIDE);
        }
    }
    catch (...)
    {
        s_isMenuOpen = false;
        if (s_hTrayMenuWnd)
        {
            ShowWindow(s_hTrayMenuWnd, SW_HIDE);
        }
    }
}

void CloseTrayMenu()
{
    try
    {
        s_isMenuOpen = false;
        if (s_trayMenuFlyout)
        {
            s_trayMenuFlyout.Hide();
            s_trayMenuFlyout = nullptr;
        }
        if (s_trayMenuRoot)
        {
            s_trayMenuRoot = nullptr;
        }
        if (s_trayMenuWindow)
        {
            s_trayMenuWindow.Close();
            s_trayMenuWindow = nullptr;
        }
        s_hTrayMenuWnd = NULL;
    }
    catch (...)
    {
    }
}
