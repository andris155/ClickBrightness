#include "pch.h"
#include "App.xaml.h"
#include "resource.h"

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

    // Single instance mutex check & message forwarding
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"ClickBrightness_v1.0.1_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        const wchar_t* targetEvent = nullptr;
        if (pCmdLine && (wcsstr(pCmdLine, L"--exit") || wcsstr(pCmdLine, L"-e")))
        {
            targetEvent = EVENT_NAME_EXIT;
        }
        else if (pCmdLine && (wcsstr(pCmdLine, L"--settings") || wcsstr(pCmdLine, L"-s")))
        {
            targetEvent = EVENT_NAME_SHOW_SETTINGS;
        }
        else if (pCmdLine && wcsstr(pCmdLine, L"--up"))
        {
            targetEvent = EVENT_NAME_BRIGHTNESS_UP;
        }
        else if (pCmdLine && wcsstr(pCmdLine, L"--down"))
        {
            targetEvent = EVENT_NAME_BRIGHTNESS_DOWN;
        }
        else
        {
            targetEvent = EVENT_NAME_SHOW_SETTINGS;
        }

        if (targetEvent)
        {
            // 1. Signal named IPC event (cross-desktop compatible)
            HANDLE hEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, targetEvent);
            if (hEvt)
            {
                SetEvent(hEvt);
                CloseHandle(hEvt);
            }

            // 2. Fallback: post message to hidden window if on the same desktop
            HWND hExisting = FindWindowW(L"ClickBrightness_MessageWndClass", L"ClickBrightness_HiddenMsgWindow");
            if (!hExisting)
            {
                hExisting = FindWindowW(L"ClickBrightness_MessageWndClass", NULL);
            }
            if (hExisting)
            {
                if (targetEvent == EVENT_NAME_EXIT)
                {
                    PostMessageW(hExisting, WM_COMMAND, MAKEWPARAM(IDM_TRAY_EXIT, 0), 0);
                }
                else if (targetEvent == EVENT_NAME_BRIGHTNESS_UP)
                {
                    PostMessageW(hExisting, WM_HOTKEY, ID_HOTKEY_UP, 0);
                }
                else if (targetEvent == EVENT_NAME_BRIGHTNESS_DOWN)
                {
                    PostMessageW(hExisting, WM_HOTKEY, ID_HOTKEY_DOWN, 0);
                }
                else if (targetEvent == EVENT_NAME_SHOW_SETTINGS)
                {
                    PostMessageW(hExisting, WM_COMMAND, MAKEWPARAM(IDM_TRAY_OPTIONS, 0), 0);
                }
            }
        }
        if (hMutex) CloseHandle(hMutex);
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
