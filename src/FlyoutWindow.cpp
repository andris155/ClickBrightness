#include "pch.h"
#include "FlyoutWindow.h"
#include "FlyoutWindow.xaml.h"
#include "MonitorManager.h"
#include "Settings.h"

FlyoutWindow g_FlyoutWindow;

static winrt::ClickBrightness::FlyoutWindow s_mainFlyoutWindow{ nullptr };
static winrt::ClickBrightness::FlyoutWindow s_separateFlyoutWindow{ nullptr };

static winrt::ClickBrightness::implementation::FlyoutWindow* GetOrCreateImpl(winrt::ClickBrightness::FlyoutWindow& slot)
{
    if (!slot)
    {
        try
        {
            slot = winrt::make<winrt::ClickBrightness::implementation::FlyoutWindow>();
        }
        catch (...)
        {
        }
    }
    if (slot)
    {
        return winrt::get_self<winrt::ClickBrightness::implementation::FlyoutWindow>(slot);
    }
    return nullptr;
}

static winrt::ClickBrightness::implementation::FlyoutWindow* GetImplIfCreated(winrt::ClickBrightness::FlyoutWindow const& slot)
{
    if (slot)
    {
        return winrt::get_self<winrt::ClickBrightness::implementation::FlyoutWindow>(slot);
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

static std::wstring FindFirstSeparateMonitorId()
{
    for (const auto& mon : g_MonitorManager.GetMonitors())
    {
        if (!g_Settings.mainMonitorId.empty() && _wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0)
        {
            continue;
        }
        const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
        if (cfg && cfg->enabled && cfg->controlMode == 1)
        {
            return mon.id;
        }
    }
    return L"";
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

void FlyoutWindow::Show(const POINT* pTrayPos, const std::wstring& targetMonitorId, UINT trayIconId)
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
        p->SetTargetMonitor(targetMonitorId, trayIconId);
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

static bool IsMonitorAvailableForFlyout(const std::wstring& monId)
{
    if (monId.empty() || _wcsicmp(monId.c_str(), L"__SYNC__") == 0) return true;
    for (const auto& mon : g_MonitorManager.GetMonitors())
    {
        if (_wcsicmp(mon.id.c_str(), monId.c_str()) == 0)
        {
            const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
            return (!cfg || cfg->enabled);
        }
    }
    return false;
}

void FlyoutWindow::RefreshOnDisplayChange()
{
    if (auto* p = GetImplIfCreated(s_mainFlyoutWindow))
    {
        p->RefreshLayoutInPlace();
    }
    if (auto* p = GetImplIfCreated(s_separateFlyoutWindow))
    {
        if (p->IsWindowVisible())
        {
            if (IsMonitorAvailableForFlyout(p->GetTargetMonitor()))
            {
                p->RefreshLayoutInPlace();
            }
            else
            {
                p->HideWindow();
            }
        }
        else
        {
            std::wstring sepId = FindFirstSeparateMonitorId();
            if (!sepId.empty() && p->GetTargetMonitor().empty())
            {
                p->SetTargetMonitor(sepId);
            }
            if (!p->GetTargetMonitor().empty())
            {
                p->RefreshLayoutInPlace();
            }
        }
    }
    else
    {
        std::wstring sepId = FindFirstSeparateMonitorId();
        if (!sepId.empty())
        {
            if (auto* pSep = GetOrCreateImpl(s_separateFlyoutWindow))
            {
                pSep->SetTargetMonitor(sepId);
                pSep->Prewarm();
            }
        }
    }
}

void FlyoutWindow::ResetGraphicsResources()
{
    bool mainWasVisible = false;
    bool sepWasVisible = false;
    std::wstring sepTarget;

    if (auto* p = GetImplIfCreated(s_mainFlyoutWindow))
    {
        mainWasVisible = p->IsWindowVisible();
    }
    if (auto* p = GetImplIfCreated(s_separateFlyoutWindow))
    {
        sepWasVisible = p->IsWindowVisible();
        sepTarget = p->GetTargetMonitor();
    }

    Destroy();
    Create();

    if (mainWasVisible)
    {
        Show(nullptr, L"");
    }
    else if (sepWasVisible && IsMonitorAvailableForFlyout(sepTarget))
    {
        Show(nullptr, sepTarget);
    }
}
