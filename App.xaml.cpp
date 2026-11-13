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
static HANDLE s_hEvents[4] = { NULL, NULL, NULL, NULL };
static std::atomic<bool> s_ipcRunning{ false };
static std::thread s_ipcThread;

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
    case WM_APP_BRIGHTNESS_UPDATED:
    {
        g_TrayIcon.UpdateBrightness((int)wParam);
        g_FlyoutWindow.SetBrightness((int)wParam, false);
        return 0;
    }

    case WM_APP_TRAY_WHEEL:
    {
        int step = static_cast<int>(wParam);
        g_OverlayWindow.TriggerTrayWheel(step);
        return 0;
    }

    case WM_APP_TRAYMSG:
    {
        UINT event = (UINT)LOWORD(lParam);
        static ULONGLONG s_lastTrayLeftClick = 0;
        static ULONGLONG s_lastTrayRightClick = 0;
        static ULONGLONG s_lastTrayMiddleClick = 0;

        switch (event)
        {
        case WM_MBUTTONUP:
        {
            ULONGLONG now = GetTickCount64();
            if (now - s_lastTrayMiddleClick < 350) break;
            s_lastTrayMiddleClick = now;

            if (g_FlyoutWindow.IsVisible())
            {
                g_FlyoutWindow.Hide();
            }

            if (g_Settings.trayMiddleClickEnabled)
            {
                int target = (std::clamp)(g_Settings.trayMiddleClickBrightness, 0, 100);
                g_OverlayWindow.TriggerBrightnessSet(target, g_Settings.trayMiddleClickShowOsd);
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
                g_FlyoutWindow.Show(&pt);
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
                g_TrayIcon.ShowContextMenu();
            }
            catch (...)
            {
            }
            break;
        }
        }
        return 0;
    }

    case WM_HOTKEY:
    {
        static ULONGLONG s_lastHotkeyTick = 0;
        static WPARAM s_lastHotkeyId = 0;

        ULONGLONG now = GetTickCount64();
        int interval = g_Settings.hotkeyRepeatIntervalMs;
        if (interval > 0 && wParam == s_lastHotkeyId && (now - s_lastHotkeyTick) < (ULONGLONG)interval)
        {
            return 0;
        }
        s_lastHotkeyTick = now;
        s_lastHotkeyId = wParam;

        int step = g_Settings.hotkeyStep;
        if (wParam == ID_HOTKEY_UP)
        {
            if (g_Settings.hotkeyAdjustEnabled)
            {
                g_OverlayWindow.TriggerBrightnessDelta(+step, g_Settings.hotkeyAdjustShowOsd);
            }
        }
        else if (wParam == ID_HOTKEY_DOWN)
        {
            if (g_Settings.hotkeyAdjustEnabled)
            {
                g_OverlayWindow.TriggerBrightnessDelta(-step, g_Settings.hotkeyAdjustShowOsd);
            }
        }
        else if (wParam == ID_HOTKEY_PRESET1)
        {
            if (g_Settings.hotkeyPresetsEnabled)
            {
                g_OverlayWindow.TriggerBrightnessSet(g_Settings.hotkeyPreset1Brightness, g_Settings.hotkeyPresetsShowOsd);
            }
        }
        else if (wParam == ID_HOTKEY_PRESET2)
        {
            if (g_Settings.hotkeyPresetsEnabled)
            {
                g_OverlayWindow.TriggerBrightnessSet(g_Settings.hotkeyPreset2Brightness, g_Settings.hotkeyPresetsShowOsd);
            }
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
        if (wParam == PBT_APMSUSPEND)
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
                int b = g_MonitorManager.GetCachedBrightness();
                g_TrayIcon.UpdateBrightness(b);
                OnSettingsDisplayChanged();
            }
            return 0;
        }
        break;
    }

    case WM_DESTROY:
    {
        KillTimer(hwnd, IDT_MONITOR_RETRY);
        KillTimer(hwnd, IDT_GPU_RESET_COOLDOWN);
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

            // Check command line for --settings / -s
            LPWSTR cmdLine = GetCommandLineW();
            bool openSettings = (cmdLine && (wcsstr(cmdLine, L"--settings") || wcsstr(cmdLine, L"-s")));

            if (openSettings)
            {
                ShowSettingsWindow();
            }
            else
            {
                TrimProcessWorkingSetAsync();
            }
        }
        catch (...)
        {
        }
    }
}
