#pragma once

#include <windows.h>
#include <shellapi.h>

#include <d2d1.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <string>
#include <vector>

struct MonitorControlConfig;

struct ExtraTraySlot {
    UINT uId = 0;
    std::wstring monitorId;
    HICON iconCache[101] = { nullptr };
    int lastBrightness = -1;
    bool created = false;
    bool isSyncSlot = false;
};

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    bool Create(HWND hOwnerWnd, UINT uCallbackMsg, UINT uId);
    void Destroy();

    bool UpdateBrightness(int brightness);
    bool UpdateBrightnessForTarget(const std::wstring& targetMonitorId, int brightness);
    void SyncExtraTrayIcons();
    std::wstring GetTargetMonitorByTrayId(UINT uId) const;
    const std::vector<ExtraTraySlot>& GetExtraSlots() const { return m_extraSlots; }
    void ShowContextMenu(UINT uId = 1);

    HICON CreateBrightnessIcon(int brightness);
    HICON CreateBrightnessIconForConfig(int brightness, const MonitorControlConfig& cfg);
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
    std::vector<ExtraTraySlot> m_extraSlots;
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
