#include "pch.h"
#include "App.xaml.h"
#include "resource.h"
#include "CliParser.h"
#include "MonitorManager.h"
#include "Settings.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(pCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    // Ensure Per-Monitor V2 DPI awareness is active
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Parse optional --simulate-monitors=N / --simulate-monitors N / --sim=N before GUI or CLI monitor actions
    std::wstring fullCmd = (pCmdLine ? pCmdLine : L"");
    {
        auto tryParseSim = [&](const std::wstring& cmdStr) {
            std::wstring lower = cmdStr;
            if (!lower.empty()) CharLowerBuffW(&lower[0], (DWORD)lower.size());
            const wchar_t* keys[] = { L"--simulate-monitors=", L"--simulate-monitors ", L"--sim=", L"--sim " };
            for (const wchar_t* k : keys) {
                size_t pos = lower.find(k);
                if (pos != std::wstring::npos) {
                    size_t valPos = pos + wcslen(k);
                    while (valPos < cmdStr.size() && cmdStr[valPos] == L' ') valPos++;
                    int val = _wtoi(cmdStr.c_str() + valPos);
                    if (val >= 0 && val <= 16) {
                        g_Settings.cliSimulateMonitorsOverride = val;
                    }
                    break;
                }
            }
        };
        tryParseSim(fullCmd);
    }

    // Single instance mutex check & message forwarding
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"ClickBrightness_v2.0.0_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CliCommand cmd = ParseCommandLine(fullCmd);

        if (cmd.action == CliAction::Help)
        {
            PrintToConsole(GetHelpText());
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }

        if (cmd.action == CliAction::ListMonitors)
        {
            g_Settings.Load();
            MonitorManager monMgr;
            monMgr.Initialize();
            PrintToConsole(GetMonitorsText(monMgr));
            monMgr.Cleanup();
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }

        HWND hExisting = FindWindowW(L"ClickBrightness_MessageWndClass", L"ClickBrightness_HiddenMsgWindow");
        if (!hExisting)
        {
            hExisting = FindWindowW(L"ClickBrightness_MessageWndClass", NULL);
        }

        if (hExisting)
        {
            COPYDATASTRUCT cds;
            cds.dwData = 0xCB01;
            cds.cbData = (DWORD)((fullCmd.length() + 1) * sizeof(wchar_t));
            cds.lpData = (PVOID)fullCmd.c_str();

            LRESULT lr = SendMessageW(hExisting, WM_COPYDATA, (WPARAM)NULL, (LPARAM)&cds);

            if (cmd.action == CliAction::GetBrightness)
            {
                if (lr > 0)
                {
                    int b = (int)lr - 1;
                    PrintToConsole(L"Current Brightness: " + std::to_wstring(b) + L"%\r\n");
                    if (hMutex) CloseHandle(hMutex);
                    return b;
                }
            }
        }
        else
        {
            // Fallback: signal named IPC event (cross-desktop compatible)
            const wchar_t* targetEvent = nullptr;
            if (cmd.action == CliAction::Exit) targetEvent = EVENT_NAME_EXIT;
            else if (cmd.action == CliAction::StepUp) targetEvent = EVENT_NAME_BRIGHTNESS_UP;
            else if (cmd.action == CliAction::StepDown) targetEvent = EVENT_NAME_BRIGHTNESS_DOWN;
            else targetEvent = EVENT_NAME_SHOW_SETTINGS;

            HANDLE hEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, targetEvent);
            if (hEvt)
            {
                SetEvent(hEvt);
                CloseHandle(hEvt);
            }
        }

        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // App is NOT running: handle standalone CLI-only requests before starting GUI
    CliCommand cmd = ParseCommandLine(fullCmd);

    if (cmd.action == CliAction::Help)
    {
        PrintToConsole(GetHelpText());
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 0;
    }

    if (cmd.action == CliAction::GetBrightness)
    {
        g_Settings.Load();
        MonitorManager monMgr;
        monMgr.Initialize();
        std::wstring targetId;
        int b = (!cmd.monitorSelector.empty() && ResolveCliMonitorTarget(monMgr, cmd.monitorSelector, targetId))
            ? monMgr.GetTargetControlBrightness(targetId)
            : monMgr.GetCurrentBrightness();
        PrintToConsole(L"Current Brightness: " + std::to_wstring(b) + L"%\r\n");
        monMgr.Cleanup();
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return b;
    }

    if (cmd.action == CliAction::ListMonitors)
    {
        g_Settings.Load();
        MonitorManager monMgr;
        monMgr.Initialize();
        PrintToConsole(GetMonitorsText(monMgr));
        monMgr.Cleanup();
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 0;
    }

    if (cmd.action == CliAction::Exit)
    {
        PrintToConsole(L"ClickBrightness is not running.\r\n");
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 0;
    }

    try
    {
        // Initialize WinRT apartment
        winrt::init_apartment(winrt::apartment_type::single_threaded);

        // Start WinUI 3 Application
        winrt::Microsoft::UI::Xaml::Application::Start([](auto&&)
        {
            winrt::make<winrt::BrightnessApp::implementation::App>();
        });
    }
    catch (...)
    {
    }

    if (hMutex)
    {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    // Clean, instant exit without DLL unload stalls
    ExitProcess(0);
    return 0;
}
