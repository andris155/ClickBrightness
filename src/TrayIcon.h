#pragma once

#include <windows.h>
#include <shellapi.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    bool Create(HWND hOwnerWnd, UINT uCallbackMsg, UINT uId);
    void Destroy();

    bool UpdateBrightness(int brightness);
    void ShowContextMenu();

    HICON CreateBrightnessIcon(int brightness);
    void InvalidateIconCache();
    void OnThemeChanged();
    void OnLanguageChanged();
    void ResetGraphicsResources();
    void UpdateWheelHook();

    HWND GetOwnerWnd() const { return m_hOwnerWnd; }
    UINT GetId() const { return m_uId; }

    static bool IsTrayColorLight();

private:
    void InitD2D();
    void CleanupD2D();

    HWND m_hOwnerWnd;
    UINT m_uId;
    UINT m_uCallbackMsg;
    HICON m_hBaseSunIcon;
    HICON m_iconCache[101];
    int m_lastBrightness;
    bool m_created;
    bool m_lastTrayWasLight;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_pD2DFactory;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_pDWriteFactory;
    Microsoft::WRL::ComPtr<IDWriteRenderingParams> m_pRenderingParams;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_pDCRenderTarget;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pBrush;
};

extern TrayIcon g_TrayIcon;
void CloseTrayMenu();
