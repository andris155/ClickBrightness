#include "pch.h"
#include "FlyoutWindow.h"
#include "FlyoutWindow.xaml.h"

FlyoutWindow g_FlyoutWindow;

static winrt::BrightnessApp::FlyoutWindow s_flyoutWindow{ nullptr };

static winrt::BrightnessApp::implementation::FlyoutWindow* GetImpl()
{
    if (!s_flyoutWindow)
    {
        try
        {
            s_flyoutWindow = winrt::make<winrt::BrightnessApp::implementation::FlyoutWindow>();
        }
        catch (...)
        {
        }
    }
    if (s_flyoutWindow)
    {
        return winrt::get_self<winrt::BrightnessApp::implementation::FlyoutWindow>(s_flyoutWindow);
    }
    return nullptr;
}

FlyoutWindow::~FlyoutWindow()
{
    Destroy();
}

bool FlyoutWindow::Create()
{
    return (GetImpl() != nullptr);
}

void FlyoutWindow::Show(const POINT* pTrayPos, const std::wstring& targetMonitorId)
{
    auto p = GetImpl();
    if (p)
    {
        p->SetTargetMonitor(targetMonitorId);
        p->ShowWindow(pTrayPos);
    }
}

void FlyoutWindow::Hide()
{
    if (s_flyoutWindow)
    {
        auto p = winrt::get_self<winrt::BrightnessApp::implementation::FlyoutWindow>(s_flyoutWindow);
        if (p) p->HideWindow();
    }
}

void FlyoutWindow::Destroy()
{
    if (s_flyoutWindow)
    {
        auto p = winrt::get_self<winrt::BrightnessApp::implementation::FlyoutWindow>(s_flyoutWindow);
        if (p) p->DestroyWindow();
        s_flyoutWindow = nullptr;
    }
}

bool FlyoutWindow::IsVisible() const
{
    if (s_flyoutWindow)
    {
        auto p = winrt::get_self<winrt::BrightnessApp::implementation::FlyoutWindow>(s_flyoutWindow);
        if (p) return p->IsWindowVisible();
    }
    return false;
}

ULONGLONG FlyoutWindow::GetHideTick() const
{
    if (s_flyoutWindow)
    {
        auto p = winrt::get_self<winrt::BrightnessApp::implementation::FlyoutWindow>(s_flyoutWindow);
        if (p) return p->GetHideTick();
    }
    return 0;
}

void FlyoutWindow::SetBrightness(int brightness, bool notifyHardware)
{
    SetBrightnessForTarget(L"", brightness, notifyHardware);
}

void FlyoutWindow::SetBrightnessForTarget(const std::wstring& targetMonitorId, int brightness, bool notifyHardware)
{
    if (s_flyoutWindow)
    {
        auto p = winrt::get_self<winrt::BrightnessApp::implementation::FlyoutWindow>(s_flyoutWindow);
        if (p) p->SetBrightnessForTarget(targetMonitorId, brightness, notifyHardware);
    }
}

void FlyoutWindow::ApplyPendingBrightness()
{
    if (s_flyoutWindow)
    {
        auto p = winrt::get_self<winrt::BrightnessApp::implementation::FlyoutWindow>(s_flyoutWindow);
        if (p) p->ApplyPendingBrightness();
    }
}

void FlyoutWindow::OnThemeChanged()
{
    if (s_flyoutWindow)
    {
        auto p = winrt::get_self<winrt::BrightnessApp::implementation::FlyoutWindow>(s_flyoutWindow);
        if (p) p->UpdateTheme();
    }
}

void FlyoutWindow::ResetGraphicsResources()
{
    Destroy();
    Create();
}
