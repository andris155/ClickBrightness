#pragma once
#include <windows.h>

class OverlayWindow
{
public:
    OverlayWindow() = default;
    ~OverlayWindow();

    bool Create();
    void TriggerBrightnessDelta(int delta, bool showOsd = true);
    void TriggerTrayWheel(int delta);
    void TriggerBrightnessSet(int target, bool showOsd = true);
    void ApplyPendingBrightness();
    void OnThemeChanged();
    void Hide();
    void Destroy();
    void ResetGraphicsResources();
};

extern OverlayWindow g_OverlayWindow;
