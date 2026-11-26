#include "pch.h"
#include "App.xaml.h"
#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

#if __has_include("module.g.cpp")
#include "module.g.cpp"
#endif

#if __has_include("XamlMetaDataProvider.g.cpp")
#include "XamlMetaDataProvider.g.cpp"
#endif

#include "XamlTypeInfo.Impl.g.cpp"
#include "XamlTypeInfo.g.cpp"

#include "Settings.h"
#include "LocalizationManager.h"
#include "MonitorManager.h"
#include "TrayIcon.h"
#include "FlyoutWindow.h"
#include "OverlayWindow.h"
#include "SettingsWindow.xaml.h"
#include "HotkeyManager.h"
#include "ThemeHelper.h"
#include "CliParser.h"
#include "resource.h"

#include <strsafe.h>
#include <roerrorapi.h>
#include <dbt.h>
#include <thread>
#include <atomic>

static const wchar_t* MAIN_WND_CLASS = L"ClickBrightness_MessageWndClass";
static HWND g_hMainWnd = NULL;
static UINT g_uTaskbarRestartMsg = 0;
static int g_monitorRetryCount = 0;
static HPOWERNOTIFY s_hPowerNotify = NULL;
static HANDLE s_hEvents[4] = { NULL, NULL, NULL, NULL };
static std::atomic<bool> s_ipcRunning{ false };
static std::thread s_ipcThread;
static bool s_toggleKeyHeld = false;
static UINT s_toggleHeldVk = 0;

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    try
    {
        if (uMsg == g_uTaskbarRestartMsg && g_uTaskbarRestartMsg != 0)
        {
            int b = g_MonitorManager.GetCachedBrightness();
            g_TrayIcon.Create(hwnd, WM_APP_TRAYMSG, 1);
            g_TrayIcon.UpdateBrightness(b);
            return 0;
        }

    switch (uMsg)
    {
    case WM_COPYDATA:
    {
        PCOPYDATASTRUCT pcds = (PCOPYDATASTRUCT)lParam;
        if (pcds && pcds->lpData && pcds->dwData == 0xCB01)
        {
            std::wstring cmdStr = (const wchar_t*)pcds->lpData;
            CliCommand cmd = ParseCommandLine(cmdStr);
            switch (cmd.action)
            {
            case CliAction::GetBrightness:
            {
                int b = g_MonitorManager.GetCachedBrightness();
                return (LRESULT)(b + 1); // +1 offset so 0% brightness returns non-zero
            }
            case CliAction::SetBrightness:
            {
                if (cmd.value >= 0 && cmd.value <= 100)
                {
                    g_OverlayWindow.TriggerBrightnessSet(cmd.value, cmd.showOsd);
                }
                return 1;
            }
            case CliAction::StepUp:
            {
                int delta = (cmd.value > 0) ? cmd.value : g_Settings.hotkeyStep;
                g_OverlayWindow.TriggerBrightnessDelta(+delta, cmd.showOsd);
                return 1;
            }
            case CliAction::StepDown:
            {
                int delta = (cmd.value > 0) ? cmd.value : g_Settings.hotkeyStep;
                g_OverlayWindow.TriggerBrightnessDelta(-delta, cmd.showOsd);
                return 1;
            }
            case CliAction::Preset1:
            {
                g_OverlayWindow.TriggerBrightnessSet(g_Settings.hotkeyPreset1Brightness, cmd.showOsd);
                return 1;
            }
            case CliAction::Preset2:
            {
                g_OverlayWindow.TriggerBrightnessSet(g_Settings.hotkeyPreset2Brightness, cmd.showOsd);
                return 1;
            }
            case CliAction::Toggle:
            {
                int curB = g_MonitorManager.GetCachedBrightness();
                int p1 = g_Settings.hotkeyPresetToggle1Brightness;
                int p2 = g_Settings.hotkeyPresetToggle2Brightness;
                int target = (curB == p1) ? p2 : ((curB == p2) ? p1 : (abs(curB - p1) < abs(curB - p2) ? p2 : p1));
                g_OverlayWindow.TriggerBrightnessSet(target, cmd.showOsd);
                return 1;
            }
            case CliAction::ShowFlyout:
            {
                POINT pt;
                GetCursorPos(&pt);
                g_FlyoutWindow.Show(&pt);
                return 1;
            }
            case CliAction::ShowSettings:
            {
                ShowSettingsWindow();
                return 1;
            }
            case CliAction::Exit:
            {
                PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDM_TRAY_EXIT, 0), 0);
                return 1;
            }
            default:
            {
                ShowSettingsWindow();
                return 1;
            }
            }
        }
        return 0;
    }

    case WM_APP_BRIGHTNESS_UPDATED:
    {
        g_TrayIcon.UpdateBrightness((int)wParam);
        g_TrayIcon.SyncExtraTrayIcons();
        g_FlyoutWindow.SetBrightness((int)wParam, false);
        return 0;
    }

    case WM_APP_POWER_STATE_UPDATED:
    {
        g_TrayIcon.UpdateBrightness(g_MonitorManager.GetCachedBrightness());
        g_TrayIcon.SyncExtraTrayIcons(true);
        if (g_FlyoutWindow.IsVisible())
        {
            g_FlyoutWindow.RefreshOnDisplayChange();
        }
        OnSettingsDisplayChanged();
        return 0;
    }

    case WM_APP_TRAY_WHEEL:
    {
        int step = static_cast<int>(wParam);
        UINT trayId = static_cast<UINT>(lParam);
        std::wstring targetMonId = g_TrayIcon.GetTargetMonitorByTrayId(trayId);
        g_OverlayWindow.TriggerTrayWheelForTarget(targetMonId, step);
        return 0;
    }

    case WM_APP_TRAYMSG:
    {
        UINT trayId = (UINT)HIWORD(lParam);
        if (trayId == 0) trayId = 1;
        std::wstring targetMonId = g_TrayIcon.GetTargetMonitorByTrayId(trayId);
        std::wstring effMonId = targetMonId.empty() ? g_Settings.mainMonitorId : targetMonId;
        const auto& effCfg = g_Settings.GetEffectiveMonitorConfig(effMonId);

        UINT event = (UINT)LOWORD(lParam);
        static ULONGLONG s_lastTrayLeftClick = 0;
        static ULONGLONG s_lastTrayRightClick = 0;
        static ULONGLONG s_lastTrayMiddleClick = 0;
        static ULONGLONG s_lastHoverPowerCheck = 0;

        switch (event)
        {
        case WM_MOUSEMOVE:
        case NIN_POPUPOPEN:
        {
            ULONGLONG now = GetTickCount64();
            if (now - s_lastHoverPowerCheck >= 1500)
            {
                s_lastHoverPowerCheck = now;
                g_MonitorManager.RefreshExternalPowerStatesAsync();
            }
            break;
        }
        case WM_MBUTTONUP:
        {
            ULONGLONG now = GetTickCount64();
            if (now - s_lastTrayMiddleClick < 350) break;
            s_lastTrayMiddleClick = now;

            if (g_FlyoutWindow.IsVisible())
            {
                g_FlyoutWindow.Hide();
            }

            if (effCfg.trayMiddleClickEnabled)
            {
                int target = (std::clamp)(effCfg.trayMiddleClickBrightness, 0, 100);
                g_OverlayWindow.TriggerBrightnessSetForTarget(targetMonId, target, effCfg.trayMiddleClickShowOsd);
            }
            break;
        }
        case WM_LBUTTONUP:
        case NIN_SELECT:
        {
            ULONGLONG now = GetTickCount64();
            if (now - s_lastTrayLeftClick < 250) break;
            if (now - g_FlyoutWindow.GetHideTick() < 250) break;
            s_lastTrayLeftClick = now;

            if (g_FlyoutWindow.IsVisible())
            {
                g_FlyoutWindow.Hide();
            }
            else
            {
                g_OverlayWindow.ApplyPendingBrightness();
                POINT pt;
                GetCursorPos(&pt);
                g_FlyoutWindow.Show(&pt, targetMonId);
            }
            break;
        }
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
        {
            ULONGLONG now = GetTickCount64();
            if (now - s_lastTrayRightClick < 250) break;
            s_lastTrayRightClick = now;

            if (g_FlyoutWindow.IsVisible())
            {
                g_FlyoutWindow.Hide();
            }

            try
            {
                g_TrayIcon.ShowContextMenu(trayId);
            }
            catch (...)
            {
            }
            break;
        }
        }
        return 0;
    }

    case WM_CLOSE:
    {
        ExitApp();
        return 0;
    }

    case WM_HOTKEY:
    {
        static ULONGLONG s_lastHotkeyTick = 0;
        static WPARAM s_lastHotkeyId = 0;
        static ULONGLONG s_lastToggleTick = 0;

        std::wstring targetMonId;
        int action = -1;
        UINT toggleVk = 0;
        if (!g_HotkeyManager.ResolveHotkey((int)wParam, targetMonId, action, toggleVk))
        {
            return 0;
        }

        ULONGLONG now = GetTickCount64();

        // One-shot toggle handling with hold suppression
        if (action == 4)
        {
            if (s_toggleKeyHeld)
            {
                return 0;
            }
            if (s_lastToggleTick != 0 && (now - s_lastToggleTick) < 300)
            {
                return 0;
            }

            s_toggleKeyHeld = true;
            s_toggleHeldVk = toggleVk;
            s_lastToggleTick = now;
            SetTimer(hwnd, IDT_PRESET_TOGGLE_RELEASE, 50, NULL);
        }
        else
        {
            int interval = g_Settings.hotkeyRepeatIntervalMs;
            if (interval > 0 && wParam == s_lastHotkeyId && (now - s_lastHotkeyTick) < (ULONGLONG)interval)
            {
                return 0;
            }
            s_lastHotkeyTick = now;
            s_lastHotkeyId = wParam;
        }

        std::wstring effMonId = targetMonId.empty() ? g_Settings.mainMonitorId : targetMonId;
        const auto& effCfg = g_Settings.GetEffectiveMonitorConfig(effMonId);

        int step = effCfg.hotkeyStep;
        if (action == 0 && effCfg.hotkeyAdjustEnabled)
        {
            g_OverlayWindow.TriggerBrightnessDeltaForTarget(targetMonId, +step, effCfg.hotkeyAdjustShowOsd);
        }
        else if (action == 1 && effCfg.hotkeyAdjustEnabled)
        {
            g_OverlayWindow.TriggerBrightnessDeltaForTarget(targetMonId, -step, effCfg.hotkeyAdjustShowOsd);
        }
        else if (action == 2 && effCfg.hotkeyPresetsEnabled)
        {
            g_OverlayWindow.TriggerBrightnessSetForTarget(targetMonId, effCfg.hotkeyPreset1Brightness, effCfg.hotkeyPresetsShowOsd);
        }
        else if (action == 3 && effCfg.hotkeyPresetsEnabled)
        {
            g_OverlayWindow.TriggerBrightnessSetForTarget(targetMonId, effCfg.hotkeyPreset2Brightness, effCfg.hotkeyPresetsShowOsd);
        }
        else if (action == 4 && effCfg.hotkeyPresetToggleEnabled)
        {
            int curB = g_MonitorManager.GetMonitorBrightnessById(targetMonId);
            int p1 = effCfg.hotkeyPresetToggle1Brightness;
            int p2 = effCfg.hotkeyPresetToggle2Brightness;
            int target = (curB == p1) ? p2 : ((curB == p2) ? p1 : (abs(curB - p1) < abs(curB - p2) ? p2 : p1));
            g_OverlayWindow.TriggerBrightnessSetForTarget(targetMonId, target, effCfg.hotkeyPresetToggleShowOsd);
        }
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        switch (id)
        {
        case IDM_TRAY_OPTIONS:
            ShowSettingsWindow();
            break;
        case IDM_TRAY_EXIT:
            ExitApp();
            break;
        }
        return 0;
    }

    case WM_DISPLAYCHANGE:
    {
        g_MonitorManager.OnDisplayChanged();

        // GPU driver update / mode switch occurred: debounce complete graphics pipeline recreation
        // to let the new GPU driver fully complete initialization before recreating swapchains
        SetTimer(hwnd, IDT_GPU_RESET_COOLDOWN, 1500, NULL);

        if (!g_MonitorManager.IsDriverAvailable())
        {
            g_monitorRetryCount = 0;
            SetTimer(hwnd, IDT_MONITOR_RETRY, 1500, NULL);
        }
        return 0;
    }

    case WM_DEVICECHANGE:
    {
        // DBT_DEVNODES_CHANGED (0x0007) is broadcast by Windows when GPU driver restarts/reloads
        if (wParam == DBT_DEVNODES_CHANGED || wParam == 0x0007)
        {
            g_MonitorManager.OnDisplayChanged();
            SetTimer(hwnd, IDT_GPU_RESET_COOLDOWN, 1500, NULL);
        }
        return 0;
    }

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
    {
        ThemeHelper::GetAccentColors(true);
        g_OverlayWindow.OnThemeChanged();
        g_FlyoutWindow.OnThemeChanged();
        g_TrayIcon.OnThemeChanged();
        OnSettingsThemeChanged();
        return 0;
    }

    case WM_POWERBROADCAST:
    {
        if (wParam == PBT_POWERSETTINGCHANGE)
        {
            auto pSetting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
            if (pSetting && IsEqualGUID(pSetting->PowerSetting, GUID_MONITOR_POWER_ON))
            {
                DWORD powerState = (pSetting->DataLength >= sizeof(DWORD))
                    ? *reinterpret_cast<const DWORD*>(pSetting->Data)
                    : 1;
                // 0 = display off / standby, 1 = display on, 2 = display dimmed
                if (powerState == 0)
                {
                    g_MonitorManager.OnDisplayPowerOff();
                }
                else
                {
                    g_MonitorManager.OnDisplayPowerOn();
                }
            }
            return TRUE;
        }
        else if (wParam == PBT_APMSUSPEND)
        {
            g_MonitorManager.OnPowerSuspend();
        }
        else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
        {
            g_MonitorManager.OnPowerResume();
            int b = g_MonitorManager.GetCachedBrightness();
            g_TrayIcon.UpdateBrightness(b);
            SetTimer(hwnd, IDT_GPU_RESET_COOLDOWN, 500, NULL);
        }
        return TRUE;
    }

    case WM_TIMER:
    {
        if (wParam == IDT_GPU_RESET_COOLDOWN)
        {
            KillTimer(hwnd, IDT_GPU_RESET_COOLDOWN);

            // 1. Re-initialize physical monitors on the new GPU driver
            g_MonitorManager.Reinitialize();
            g_HotkeyManager.RegisterAll(hwnd);

            // 2. Recreate Direct2D, Direct3D, XAML swapchains, and tray menus on the new GPU device
            g_TrayIcon.ResetGraphicsResources();
            g_FlyoutWindow.ResetGraphicsResources();
            g_OverlayWindow.ResetGraphicsResources();

            // 3. Gracefully refresh Settings window if open (do not destroy/recreate)
            OnSettingsDisplayChanged();

            int b = g_MonitorManager.GetCachedBrightness();
            g_TrayIcon.UpdateBrightness(b);

            // If driver is still settling or physical monitors aren't ready yet, start retry loop
            if (!g_MonitorManager.IsDriverAvailable())
            {
                g_monitorRetryCount = 0;
                SetTimer(hwnd, IDT_MONITOR_RETRY, 1500, NULL);
            }
            return 0;
        }
        else if (wParam == IDT_MONITOR_RETRY)
        {
            g_monitorRetryCount++;
            if (g_MonitorManager.Reinitialize() || g_monitorRetryCount >= 8)
            {
                KillTimer(hwnd, IDT_MONITOR_RETRY);
                g_monitorRetryCount = 0;
                g_HotkeyManager.RegisterAll(hwnd);
                g_TrayIcon.SyncExtraTrayIcons();
                g_FlyoutWindow.RefreshOnDisplayChange();
                int b = g_MonitorManager.GetCachedBrightness();
                g_TrayIcon.UpdateBrightness(b);
                OnSettingsDisplayChanged();
            }
            return 0;
        }
        else if (wParam == IDT_POWER_STATE_POLL)
        {
            if (g_Settings.disableTurnedOffControl && g_MonitorManager.IsDisplayPoweredOn())
            {
                g_MonitorManager.RefreshExternalPowerStatesAsync();
            }
            return 0;
        }
        else if (wParam == IDT_PRESET_TOGGLE_RELEASE)
        {
            UINT vk = s_toggleHeldVk ? s_toggleHeldVk : g_Settings.hotkeyPresetToggleKey;
            if (vk == 0 || (GetAsyncKeyState(vk) & 0x8000) == 0)
            {
                // Key has been physically released! Stop timer and re-arm toggle
                KillTimer(hwnd, IDT_PRESET_TOGGLE_RELEASE);
                s_toggleKeyHeld = false;
                s_toggleHeldVk = 0;
            }
            return 0;
        }
        break;
    }

    case WM_DESTROY:
    {
        KillTimer(hwnd, IDT_PRESET_TOGGLE_RELEASE);
        s_toggleKeyHeld = false;
        KillTimer(hwnd, IDT_MONITOR_RETRY);
        KillTimer(hwnd, IDT_GPU_RESET_COOLDOWN);
        if (s_hPowerNotify)
        {
            UnregisterPowerSettingNotification(s_hPowerNotify);
            s_hPowerNotify = NULL;
        }
        g_Settings.Save();
        g_HotkeyManager.UnregisterAll(hwnd);
        g_TrayIcon.Destroy();
        g_FlyoutWindow.Destroy();
        g_OverlayWindow.Destroy();
        g_MonitorManager.Cleanup();
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
    catch (...)
    {
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
}


void ExitApp()
{
    static bool s_exiting = false;
    if (s_exiting) return;
    s_exiting = true;

    // 0. Unhook mouse wheel hook and remove tray icons immediately so cursor never lags on exit
    g_TrayIcon.Destroy();

    // 1. Terminate IPC listener thread
    s_ipcRunning = false;
    for (int i = 0; i < 4; ++i)
    {
        if (s_hEvents[i])
        {
            SetEvent(s_hEvents[i]);
        }
    }
    if (s_ipcThread.joinable())
    {
        s_ipcThread.join();
    }
    for (int i = 0; i < 4; ++i)
    {
        if (s_hEvents[i])
        {
            CloseHandle(s_hEvents[i]);
            s_hEvents[i] = NULL;
        }
    }

    // 2. Tear down UI windows cleanly
    CloseSettingsWindow();
    CloseTrayMenu();

    if (s_hPowerNotify)
    {
        UnregisterPowerSettingNotification(s_hPowerNotify);
        s_hPowerNotify = NULL;
    }

    // 3. Destroy message window (triggers WM_DESTROY for remaining cleanup)
    if (g_hMainWnd)
    {
        HWND h = g_hMainWnd;
        g_hMainWnd = NULL;
        DestroyWindow(h);
    }

    // 4. Request WinUI 3 message loop exit
    try
    {
        winrt::Microsoft::UI::Xaml::Application::Current().Exit();
    }
    catch (...) {}
}

namespace winrt::BrightnessApp::implementation
{
    App::App()
    {
        UnhandledException([](auto const&, Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& args)
        {
            args.Handled(true);
        });

        g_Settings.Load();
        g_Loc.Initialize();
        g_Loc.SetLanguage(g_Settings.language);
        bool isDark = ThemeHelper::IsAppInDarkMode(g_Settings.appTheme);
        RequestedTheme(isDark ? Microsoft::UI::Xaml::ApplicationTheme::Dark : Microsoft::UI::Xaml::ApplicationTheme::Light);

        InitializeComponent();
    }

    void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        try
        {
            // Register TaskbarCreated message for Explorer restarts
            g_uTaskbarRestartMsg = RegisterWindowMessageW(L"TaskbarCreated");

            // Load settings
            g_Settings.Load();
        g_Loc.Initialize();
        g_Loc.SetLanguage(g_Settings.language);

            // Register message-only window class
            HINSTANCE hInstance = GetModuleHandleW(NULL);
            WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
            wc.lpfnWndProc = MainWndProc;
            wc.hInstance = hInstance;
            wc.lpszClassName = MAIN_WND_CLASS;
            RegisterClassExW(&wc);

            g_hMainWnd = CreateWindowExW(
                0,
                MAIN_WND_CLASS,
                L"ClickBrightness_HiddenMsgWindow",
                WS_OVERLAPPED,
                0, 0, 0, 0,
                NULL, NULL, hInstance, NULL
            );

            // Register display standby notification to prevent waking sleeping monitors
            if (g_hMainWnd)
            {
                ChangeWindowMessageFilterEx(g_hMainWnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);
                s_hPowerNotify = RegisterPowerSettingNotification(g_hMainWnd, &GUID_MONITOR_POWER_ON, DEVICE_NOTIFY_WINDOW_HANDLE);
            }

            // Initialize DDC/CI monitor manager
            g_MonitorManager.Initialize();
            int initialBrightness = g_MonitorManager.GetCachedBrightness();

            // Initialize Tray Icon
            if (g_hMainWnd)
            {
                g_TrayIcon.Create(g_hMainWnd, WM_APP_TRAYMSG, 1);
                g_TrayIcon.UpdateBrightness(initialBrightness);

                // Register Hotkeys
                g_HotkeyManager.RegisterAll(g_hMainWnd);

                // Low-frequency (5s) background check for external monitor power state (VCP 0xD6)
                SetTimer(g_hMainWnd, IDT_POWER_STATE_POLL, 5000, NULL);
            }

            // Pre-create flyout and overlay windows
            g_FlyoutWindow.Create();
            g_OverlayWindow.Create();

            // Setup IPC named events
            s_hEvents[0] = CreateEventW(NULL, FALSE, FALSE, EVENT_NAME_SHOW_SETTINGS);
            s_hEvents[1] = CreateEventW(NULL, FALSE, FALSE, EVENT_NAME_EXIT);
            s_hEvents[2] = CreateEventW(NULL, FALSE, FALSE, EVENT_NAME_BRIGHTNESS_UP);
            s_hEvents[3] = CreateEventW(NULL, FALSE, FALSE, EVENT_NAME_BRIGHTNESS_DOWN);
            for (int i = 0; i < 4; ++i)
            {
                if (s_hEvents[i]) ResetEvent(s_hEvents[i]);
            }
            s_ipcRunning = true;

            s_ipcThread = std::thread([]()
            {
                while (s_ipcRunning)
                {
                    DWORD dwWait = WaitForMultipleObjects(4, s_hEvents, FALSE, INFINITE);
                    if (!s_ipcRunning) break;

                    if (dwWait == WAIT_OBJECT_0) // Show Settings
                    {
                        if (g_hMainWnd) PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(IDM_TRAY_OPTIONS, 0), 0);
                    }
                    else if (dwWait == WAIT_OBJECT_0 + 1) // Exit
                    {
                        if (g_hMainWnd) PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(IDM_TRAY_EXIT, 0), 0);
                        break;
                    }
                    else if (dwWait == WAIT_OBJECT_0 + 2) // Brightness Up
                    {
                        if (g_hMainWnd && g_Settings.hotkeyAdjustEnabled) PostMessageW(g_hMainWnd, WM_HOTKEY, ID_HOTKEY_UP, 0);
                    }
                    else if (dwWait == WAIT_OBJECT_0 + 3) // Brightness Down
                    {
                        if (g_hMainWnd && g_Settings.hotkeyAdjustEnabled) PostMessageW(g_hMainWnd, WM_HOTKEY, ID_HOTKEY_DOWN, 0);
                    }
                    else
                    {
                        break;
                    }
                }
            });

            // Check command line for initial actions
            LPWSTR cmdLine = GetCommandLineW();
            CliCommand cmd = ParseCommandLine(cmdLine ? cmdLine : L"");

            switch (cmd.action)
            {
            case CliAction::ShowSettings:
                ShowSettingsWindow();
                break;
            case CliAction::ShowFlyout:
            {
                POINT pt;
                GetCursorPos(&pt);
                g_FlyoutWindow.Show(&pt);
                break;
            }
            case CliAction::SetBrightness:
                if (cmd.value >= 0 && cmd.value <= 100)
                {
                    g_OverlayWindow.TriggerBrightnessSet(cmd.value, cmd.showOsd);
                }
                TrimProcessWorkingSetAsync();
                break;
            case CliAction::StepUp:
            {
                int delta = (cmd.value > 0) ? cmd.value : g_Settings.hotkeyStep;
                g_OverlayWindow.TriggerBrightnessDelta(+delta, cmd.showOsd);
                TrimProcessWorkingSetAsync();
                break;
            }
            case CliAction::StepDown:
            {
                int delta = (cmd.value > 0) ? cmd.value : g_Settings.hotkeyStep;
                g_OverlayWindow.TriggerBrightnessDelta(-delta, cmd.showOsd);
                TrimProcessWorkingSetAsync();
                break;
            }
            case CliAction::Preset1:
                g_OverlayWindow.TriggerBrightnessSet(g_Settings.hotkeyPreset1Brightness, cmd.showOsd);
                TrimProcessWorkingSetAsync();
                break;
            case CliAction::Preset2:
                g_OverlayWindow.TriggerBrightnessSet(g_Settings.hotkeyPreset2Brightness, cmd.showOsd);
                TrimProcessWorkingSetAsync();
                break;
            case CliAction::Toggle:
            {
                int curB = g_MonitorManager.GetCachedBrightness();
                int p1 = g_Settings.hotkeyPresetToggle1Brightness;
                int p2 = g_Settings.hotkeyPresetToggle2Brightness;
                int target = (curB == p1) ? p2 : ((curB == p2) ? p1 : (abs(curB - p1) < abs(curB - p2) ? p2 : p1));
                g_OverlayWindow.TriggerBrightnessSet(target, cmd.showOsd);
                TrimProcessWorkingSetAsync();
                break;
            }
            default:
                if (cmdLine && (wcsstr(cmdLine, L"--settings") || wcsstr(cmdLine, L"-s")))
                {
                    ShowSettingsWindow();
                }
                else
                {
                    TrimProcessWorkingSetAsync();
                }
                break;
            }
        }
        catch (...)
        {
        }
    }
}
