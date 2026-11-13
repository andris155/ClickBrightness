#pragma once

#include <windows.h>

class FlyoutWindow {
public:
    FlyoutWindow() = default;
    ~FlyoutWindow();

    bool Create();
    void Show(const POINT* pTrayPos = nullptr);
    void Hide();
    void Destroy();
    bool IsVisible() const;
    ULONGLONG GetHideTick() const;

    void SetBrightness(int brightness, bool notifyHardware);
    void ApplyPendingBrightness();
    void OnThemeChanged();
    void ResetGraphicsResources();
};

extern FlyoutWindow g_FlyoutWindow;
