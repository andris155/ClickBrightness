#pragma once

#include <windows.h>
#include <string>

class FlyoutWindow {
public:
    FlyoutWindow() = default;
    ~FlyoutWindow();

    bool Create();
    void Show(const POINT* pTrayPos = nullptr, const std::wstring& targetMonitorId = L"");
    void Hide();
    void Destroy();
    bool IsVisible() const;
    ULONGLONG GetHideTick() const;

    void SetBrightness(int brightness, bool notifyHardware);
    void SetBrightnessForTarget(const std::wstring& targetMonitorId, int brightness, bool notifyHardware);
    void ApplyPendingBrightness();
    void OnThemeChanged();
    void ResetGraphicsResources();
    void RefreshOnDisplayChange();
};

extern FlyoutWindow g_FlyoutWindow;
