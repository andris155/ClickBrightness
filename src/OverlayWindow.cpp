#include "pch.h"
#include "OverlayWindow.h"
#include "OverlayWindow.xaml.h"

OverlayWindow g_OverlayWindow;

static winrt::ClickBrightness::OverlayWindow s_overlayWindow{ nullptr };

static winrt::ClickBrightness::implementation::OverlayWindow* GetImpl()
{
    if (!s_overlayWindow)
    {
        try
        {
            s_overlayWindow = winrt::make<winrt::ClickBrightness::implementation::OverlayWindow>();
        }
        catch (...)
        {
        }
    }
    if (s_overlayWindow)
    {
        return winrt::get_self<winrt::ClickBrightness::implementation::OverlayWindow>(s_overlayWindow);
    }
    return nullptr;
}

OverlayWindow::~OverlayWindow()
{
    Destroy();
}

bool OverlayWindow::Create()
{
    return (GetImpl() != nullptr);
}

void OverlayWindow::TriggerBrightnessDelta(int delta, bool showOsd)
{
    TriggerBrightnessDeltaForTarget(L"", delta, showOsd);
}

void OverlayWindow::TriggerTrayWheel(int delta)
{
    TriggerTrayWheelForTarget(L"", delta);
}

void OverlayWindow::TriggerBrightnessSet(int target, bool showOsd)
{
    TriggerBrightnessSetForTarget(L"", target, showOsd);
}

void OverlayWindow::TriggerBrightnessDeltaForTarget(const std::wstring& targetMonitorId, int delta, bool showOsd)
{
    auto p = GetImpl();
    if (p)
    {
        p->TriggerBrightnessDeltaForTarget(targetMonitorId, delta, showOsd);
    }
}

void OverlayWindow::TriggerTrayWheelForTarget(const std::wstring& targetMonitorId, int delta)
{
    auto p = GetImpl();
    if (p)
    {
        p->TriggerTrayWheelForTarget(targetMonitorId, delta);
    }
}

void OverlayWindow::TriggerBrightnessSetForTarget(const std::wstring& targetMonitorId, int target, bool showOsd)
{
    auto p = GetImpl();
    if (p)
    {
        p->TriggerBrightnessSetForTarget(targetMonitorId, target, showOsd);
    }
}

void OverlayWindow::ApplyPendingBrightness()
{
    if (s_overlayWindow)
    {
        auto p = winrt::get_self<winrt::ClickBrightness::implementation::OverlayWindow>(s_overlayWindow);
        if (p) p->ApplyPendingBrightness();
    }
}

void OverlayWindow::OnThemeChanged()
{
    if (s_overlayWindow)
    {
        auto p = winrt::get_self<winrt::ClickBrightness::implementation::OverlayWindow>(s_overlayWindow);
        if (p) p->UpdateTheme();
    }
}

void OverlayWindow::Hide()
{
    if (s_overlayWindow)
    {
        auto p = winrt::get_self<winrt::ClickBrightness::implementation::OverlayWindow>(s_overlayWindow);
        if (p) p->HideWindow();
    }
}

void OverlayWindow::Destroy()
{
    if (s_overlayWindow)
    {
        auto p = winrt::get_self<winrt::ClickBrightness::implementation::OverlayWindow>(s_overlayWindow);
        if (p) p->DestroyWindow();
        s_overlayWindow = nullptr;
    }
}

void OverlayWindow::ResetGraphicsResources()
{
    Destroy();
    Create();
}

void OverlayWindow::RefreshOnDisplayChange()
{
    if (s_overlayWindow)
    {
        auto p = winrt::get_self<winrt::ClickBrightness::implementation::OverlayWindow>(s_overlayWindow);
        if (p) p->RefreshOnDisplayChange();
    }
}
