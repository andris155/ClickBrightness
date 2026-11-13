#include "pch.h"
#include "OverlayWindow.h"
#include "OverlayWindow.xaml.h"

OverlayWindow g_OverlayWindow;

static winrt::BrightnessApp::OverlayWindow s_overlayWindow{ nullptr };

static winrt::BrightnessApp::implementation::OverlayWindow* GetImpl()
{
    if (!s_overlayWindow)
    {
        try
        {
            s_overlayWindow = winrt::make<winrt::BrightnessApp::implementation::OverlayWindow>();
        }
        catch (...)
        {
        }
    }
    if (s_overlayWindow)
    {
        return winrt::get_self<winrt::BrightnessApp::implementation::OverlayWindow>(s_overlayWindow);
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
    auto p = GetImpl();
    if (p)
    {
        p->TriggerBrightnessDelta(delta, showOsd);
    }
}

void OverlayWindow::TriggerTrayWheel(int delta)
{
    auto p = GetImpl();
    if (p)
    {
        p->TriggerTrayWheel(delta);
    }
}

void OverlayWindow::TriggerBrightnessSet(int target, bool showOsd)
{
    auto p = GetImpl();
    if (p)
    {
        p->TriggerBrightnessSet(target, showOsd);
    }
}

void OverlayWindow::ApplyPendingBrightness()
{
    if (s_overlayWindow)
    {
        auto p = winrt::get_self<winrt::BrightnessApp::implementation::OverlayWindow>(s_overlayWindow);
        if (p) p->ApplyPendingBrightness();
    }
}

void OverlayWindow::OnThemeChanged()
{
    if (s_overlayWindow)
    {
        auto p = winrt::get_self<winrt::BrightnessApp::implementation::OverlayWindow>(s_overlayWindow);
        if (p) p->UpdateTheme();
    }
}

void OverlayWindow::Hide()
{
    if (s_overlayWindow)
    {
        auto p = winrt::get_self<winrt::BrightnessApp::implementation::OverlayWindow>(s_overlayWindow);
        if (p) p->HideWindow();
    }
}

void OverlayWindow::Destroy()
{
    if (s_overlayWindow)
    {
        auto p = winrt::get_self<winrt::BrightnessApp::implementation::OverlayWindow>(s_overlayWindow);
        if (p) p->DestroyWindow();
        s_overlayWindow = nullptr;
    }
}

void OverlayWindow::ResetGraphicsResources()
{
    Destroy();
    Create();
}
