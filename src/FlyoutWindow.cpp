#include "pch.h"
#include "FlyoutWindow.h"
#include "FlyoutWindow.xaml.h"

FlyoutWindow g_FlyoutWindow;

static winrt::BrightnessApp::FlyoutWindow s_mainFlyoutWindow{ nullptr };
static winrt::BrightnessApp::FlyoutWindow s_separateFlyoutWindow{ nullptr };

static winrt::BrightnessApp::implementation::FlyoutWindow* GetOrCreateImpl(winrt::BrightnessApp::FlyoutWindow& slot)
{
    if (!slot)
    {
        try
        {
            slot = winrt::make<winrt::BrightnessApp::implementation::FlyoutWindow>();
        }
        catch (...)
        {
        }
    }
    if (slot)
    {
        return winrt::get_self<winrt::BrightnessApp::implementation::FlyoutWindow>(slot);
    }
    return nullptr;
}

static winrt::BrightnessApp::implementation::FlyoutWindow* GetImplIfCreated(winrt::BrightnessApp::FlyoutWindow const& slot)
{
    if (slot)
    {
        return winrt::get_self<winrt::BrightnessApp::implementation::FlyoutWindow>(slot);
    }
    return nullptr;
}

static bool IsSeparateTarget(const std::wstring& targetMonitorId)
{
    return !targetMonitorId.empty() && _wcsicmp(targetMonitorId.c_str(), L"__SYNC__") != 0;
}

FlyoutWindow::~FlyoutWindow()
{
    Destroy();
}

bool FlyoutWindow::Create()
{
    auto* p = GetOrCreateImpl(s_mainFlyoutWindow);
    if (p)
    {
        p->Prewarm();
        return true;
    }
    return false;
}

void FlyoutWindow::Show(const POINT* pTrayPos, const std::wstring& targetMonitorId)
{
    bool isSeparate = IsSeparateTarget(targetMonitorId);
    auto* other = GetImplIfCreated(isSeparate ? s_mainFlyoutWindow : s_separateFlyoutWindow);
    if (other && other->IsWindowVisible())
    {
        other->HideWindow();
    }

    auto* p = GetOrCreateImpl(isSeparate ? s_separateFlyoutWindow : s_mainFlyoutWindow);
    if (p)
    {
        p->SetTargetMonitor(targetMonitorId);
        p->ShowWindow(pTrayPos);
    }
}

void FlyoutWindow::Hide()
{
    if (auto* p = GetImplIfCreated(s_mainFlyoutWindow)) p->HideWindow();
    if (auto* p = GetImplIfCreated(s_separateFlyoutWindow)) p->HideWindow();
}

void FlyoutWindow::Destroy()
{
    if (auto* p = GetImplIfCreated(s_mainFlyoutWindow)) p->DestroyWindow();
    s_mainFlyoutWindow = nullptr;
    if (auto* p = GetImplIfCreated(s_separateFlyoutWindow)) p->DestroyWindow();
    s_separateFlyoutWindow = nullptr;
}

bool FlyoutWindow::IsVisible() const
{
    if (auto* p = GetImplIfCreated(s_mainFlyoutWindow))
    {
        if (p->IsWindowVisible()) return true;
    }
    if (auto* p = GetImplIfCreated(s_separateFlyoutWindow))
    {
        if (p->IsWindowVisible()) return true;
    }
    return false;
}

ULONGLONG FlyoutWindow::GetHideTick() const
{
    ULONGLONG t = 0;
    if (auto* p = GetImplIfCreated(s_mainFlyoutWindow))
    {
        t = (std::max)(t, p->GetHideTick());
    }
    if (auto* p = GetImplIfCreated(s_separateFlyoutWindow))
    {
        t = (std::max)(t, p->GetHideTick());
    }
    return t;
}

void FlyoutWindow::SetBrightness(int brightness, bool notifyHardware)
{
    SetBrightnessForTarget(L"", brightness, notifyHardware);
}

void FlyoutWindow::SetBrightnessForTarget(const std::wstring& targetMonitorId, int brightness, bool notifyHardware)
{
    if (auto* p = GetImplIfCreated(s_mainFlyoutWindow))
    {
        p->SetBrightnessForTarget(targetMonitorId, brightness, notifyHardware);
    }
    if (auto* p = GetImplIfCreated(s_separateFlyoutWindow))
    {
        p->SetBrightnessForTarget(targetMonitorId, brightness, notifyHardware);
    }
}

void FlyoutWindow::ApplyPendingBrightness()
{
    if (auto* p = GetImplIfCreated(s_mainFlyoutWindow)) p->ApplyPendingBrightness();
    if (auto* p = GetImplIfCreated(s_separateFlyoutWindow)) p->ApplyPendingBrightness();
}

void FlyoutWindow::OnThemeChanged()
{
    if (auto* p = GetImplIfCreated(s_mainFlyoutWindow)) p->UpdateTheme();
    if (auto* p = GetImplIfCreated(s_separateFlyoutWindow)) p->UpdateTheme();
}

void FlyoutWindow::ResetGraphicsResources()
{
    Destroy();
    Create();
}
