#pragma once

#include <windows.h>
#include <string>
#include <algorithm>
#include <vector>
#include <cwctype>
#include "MonitorManager.h"

enum class CliAction {
    None,
    Help,
    GetBrightness,
    SetBrightness,
    StepUp,
    StepDown,
    Preset1,
    Preset2,
    Toggle,
    ShowFlyout,
    ShowSettings,
    ListMonitors,
    Exit
};

struct CliCommand {
    CliAction action = CliAction::None;
    int value = -1;       // For SetBrightness (0-100) or StepUp/StepDown custom delta
    bool showOsd = true;  // Default true; set to false if --no-osd is specified
};

inline void PrintToConsole(const std::wstring& text)
{
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD fileType = (hStdOut != NULL && hStdOut != INVALID_HANDLE_VALUE) ? GetFileType(hStdOut) : FILE_TYPE_UNKNOWN;

    bool attached = false;
    if (fileType == FILE_TYPE_UNKNOWN || hStdOut == NULL || hStdOut == INVALID_HANDLE_VALUE)
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
        {
            attached = true;
            hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        }
    }

    if (hStdOut && hStdOut != INVALID_HANDLE_VALUE)
    {
        DWORD dwMode = 0;
        if (GetConsoleMode(hStdOut, &dwMode))
        {
            DWORD written = 0;
            WriteConsoleW(hStdOut, text.c_str(), (DWORD)text.length(), &written, NULL);
        }
        else
        {
            // Output is redirected to a pipe or file: write UTF-8 bytes
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.length(), NULL, 0, NULL, NULL);
            if (utf8Len > 0)
            {
                std::string utf8Str(utf8Len, '\0');
                WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.length(), &utf8Str[0], utf8Len, NULL, NULL);
                DWORD written = 0;
                WriteFile(hStdOut, utf8Str.data(), (DWORD)utf8Str.length(), &written, NULL);
            }
        }
    }

    if (attached)
    {
        FreeConsole();
    }
}

inline std::wstring GetHelpText()
{
    return L"\r\nClickBrightness v1.0.7 - Hardware DDC/CI Monitor Brightness Control\r\n\r\n"
           L"Usage:\r\n"
           L"  ClickBrightness.exe [command] [options]\r\n\r\n"
           L"Commands:\r\n"
           L"  --set <0-100>, -b <0-100>   Set hardware brightness to specific percentage (e.g. --set 75)\r\n"
           L"  --up [step]                 Increase brightness by step % (default or custom, e.g. --up 10)\r\n"
           L"  --down [step]               Decrease brightness by step % (default or custom, e.g. --down 10)\r\n"
           L"  --toggle, -t                Toggle between Preset 1 and Preset 2 brightness levels\r\n"
           L"  --preset <1|2>, -p <1|2>    Jump directly to Preset 1 or Preset 2\r\n"
           L"  --get, -g                   Print current monitor brightness percentage to console\r\n"
           L"  --monitors, -m              List all detected DDC/CI physical displays and their status\r\n"
           L"  --flyout, -f                Open the Windows 11 brightness slider flyout popup\r\n"
           L"  --settings, -s              Open the Settings configuration window\r\n"
           L"  --exit, -e                  Cleanly terminate running background instance\r\n"
           L"  --help, -h, /?              Show this help information\r\n\r\n"
           L"Options:\r\n"
           L"  --osd                       Display on-screen banner during adjustment (default)\r\n"
           L"  --no-osd                    Suppress on-screen banner (silent adjustment)\r\n\r\n"
           L"Examples:\r\n"
           L"  ClickBrightness.exe --set 65\r\n"
           L"  ClickBrightness.exe --set 40 --no-osd\r\n"
           L"  ClickBrightness.exe --up 10\r\n"
           L"  ClickBrightness.exe --toggle\r\n"
           L"  ClickBrightness.exe --get\r\n"
           L"  ClickBrightness.exe --flyout\r\n\r\n";
}

inline std::wstring GetMonitorsText(MonitorManager& monMgr)
{
    auto monitors = monMgr.GetMonitors();
    if (monitors.empty())
    {
        return L"\r\nNo DDC/CI compatible monitors detected (or driver still initializing).\r\n\r\n";
    }

    std::wstring out = L"\r\nDetected DDC/CI Physical Monitors:\r\n";
    int idx = 1;
    for (const auto& mon : monitors)
    {
        out += L"  [" + std::to_wstring(idx++) + L"] ";
        if (!mon.friendlyName.empty())
        {
            out += mon.friendlyName;
        }
        else if (!mon.description.empty())
        {
            out += mon.description;
        }
        else
        {
            out += mon.deviceName;
        }

        if (mon.isPrimary)
        {
            out += L" (Primary)";
        }

        out += L" - Brightness: " + std::to_wstring(mon.currentBrightness) + L"%";
        out += L" (Range: " + std::to_wstring(mon.minBrightness) + L"-" + std::to_wstring(mon.maxBrightness) + L"%)\r\n";
    }
    out += L"\r\n";
    return out;
}

inline CliCommand ParseCommandLine(const std::wstring& cmdLine)
{
    CliCommand cmd;
    if (cmdLine.empty()) return cmd;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &argc);
    if (!argv || argc <= 0) return cmd;

    // Check for --no-osd / --osd anywhere in arguments
    for (int i = 0; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if (_wcsicmp(arg.c_str(), L"--no-osd") == 0 || _wcsicmp(arg.c_str(), L"-no-osd") == 0 || _wcsicmp(arg.c_str(), L"/no-osd") == 0)
        {
            cmd.showOsd = false;
        }
        else if (_wcsicmp(arg.c_str(), L"--osd") == 0 || _wcsicmp(arg.c_str(), L"-osd") == 0 || _wcsicmp(arg.c_str(), L"/osd") == 0)
        {
            cmd.showOsd = true;
        }
    }

    for (int i = 0; i < argc; ++i)
    {
        std::wstring arg = argv[i];

        // Help
        if (_wcsicmp(arg.c_str(), L"--help") == 0 || _wcsicmp(arg.c_str(), L"-h") == 0 ||
            _wcsicmp(arg.c_str(), L"/?") == 0 || _wcsicmp(arg.c_str(), L"-?") == 0)
        {
            cmd.action = CliAction::Help;
            break;
        }

        // Get brightness
        if (_wcsicmp(arg.c_str(), L"--get") == 0 || _wcsicmp(arg.c_str(), L"-g") == 0)
        {
            cmd.action = CliAction::GetBrightness;
            break;
        }

        // List monitors
        if (_wcsicmp(arg.c_str(), L"--monitors") == 0 || _wcsicmp(arg.c_str(), L"-m") == 0)
        {
            cmd.action = CliAction::ListMonitors;
            break;
        }

        // Exit
        if (_wcsicmp(arg.c_str(), L"--exit") == 0 || _wcsicmp(arg.c_str(), L"-e") == 0)
        {
            cmd.action = CliAction::Exit;
            break;
        }

        // Settings
        if (_wcsicmp(arg.c_str(), L"--settings") == 0 || _wcsicmp(arg.c_str(), L"-s") == 0)
        {
            cmd.action = CliAction::ShowSettings;
            break;
        }

        // Flyout
        if (_wcsicmp(arg.c_str(), L"--flyout") == 0 || _wcsicmp(arg.c_str(), L"-f") == 0)
        {
            cmd.action = CliAction::ShowFlyout;
            break;
        }

        // Toggle
        if (_wcsicmp(arg.c_str(), L"--toggle") == 0 || _wcsicmp(arg.c_str(), L"-t") == 0)
        {
            cmd.action = CliAction::Toggle;
            break;
        }

        // Preset 1
        if (_wcsicmp(arg.c_str(), L"--preset1") == 0)
        {
            cmd.action = CliAction::Preset1;
            break;
        }

        // Preset 2
        if (_wcsicmp(arg.c_str(), L"--preset2") == 0)
        {
            cmd.action = CliAction::Preset2;
            break;
        }

        // Preset with argument: --preset 1, -p 2, --preset=1
        if (_wcsicmp(arg.c_str(), L"--preset") == 0 || _wcsicmp(arg.c_str(), L"-p") == 0)
        {
            if (i + 1 < argc)
            {
                int pNum = _wtoi(argv[i + 1]);
                cmd.action = (pNum == 2) ? CliAction::Preset2 : CliAction::Preset1;
            }
            else
            {
                cmd.action = CliAction::Preset1;
            }
            break;
        }
        if (arg.rfind(L"--preset=", 0) == 0)
        {
            int pNum = _wtoi(arg.c_str() + 9);
            cmd.action = (pNum == 2) ? CliAction::Preset2 : CliAction::Preset1;
            break;
        }
        if (arg.rfind(L"-p=", 0) == 0)
        {
            int pNum = _wtoi(arg.c_str() + 3);
            cmd.action = (pNum == 2) ? CliAction::Preset2 : CliAction::Preset1;
            break;
        }

        // Set brightness: --set <0-100>, -b <0-100>, --brightness <0-100>, or raw number
        if (_wcsicmp(arg.c_str(), L"--set") == 0 || _wcsicmp(arg.c_str(), L"-b") == 0 || _wcsicmp(arg.c_str(), L"--brightness") == 0)
        {
            cmd.action = CliAction::SetBrightness;
            if (i + 1 < argc)
            {
                cmd.value = (std::clamp)(_wtoi(argv[i + 1]), 0, 100);
            }
            break;
        }
        if (arg.rfind(L"--set=", 0) == 0)
        {
            cmd.action = CliAction::SetBrightness;
            cmd.value = (std::clamp)(_wtoi(arg.c_str() + 6), 0, 100);
            break;
        }
        if (arg.rfind(L"-b=", 0) == 0)
        {
            cmd.action = CliAction::SetBrightness;
            cmd.value = (std::clamp)(_wtoi(arg.c_str() + 3), 0, 100);
            break;
        }
        if (arg.rfind(L"--brightness=", 0) == 0)
        {
            cmd.action = CliAction::SetBrightness;
            cmd.value = (std::clamp)(_wtoi(arg.c_str() + 13), 0, 100);
            break;
        }

        // Step Up: --up [step], --up=<step>
        if (_wcsicmp(arg.c_str(), L"--up") == 0)
        {
            cmd.action = CliAction::StepUp;
            if (i + 1 < argc && iswdigit(argv[i + 1][0]))
            {
                cmd.value = _wtoi(argv[i + 1]);
            }
            break;
        }
        if (arg.rfind(L"--up=", 0) == 0)
        {
            cmd.action = CliAction::StepUp;
            cmd.value = _wtoi(arg.c_str() + 5);
            break;
        }

        // Step Down: --down [step], --down=<step>
        if (_wcsicmp(arg.c_str(), L"--down") == 0)
        {
            cmd.action = CliAction::StepDown;
            if (i + 1 < argc && iswdigit(argv[i + 1][0]))
            {
                cmd.value = _wtoi(argv[i + 1]);
            }
            break;
        }
        if (arg.rfind(L"--down=", 0) == 0)
        {
            cmd.action = CliAction::StepDown;
            cmd.value = _wtoi(arg.c_str() + 7);
            break;
        }

        // Raw number: e.g. "ClickBrightness.exe 75"
        if (iswdigit(arg[0]))
        {
            bool allDigits = true;
            for (wchar_t ch : arg)
            {
                if (!iswdigit(ch)) { allDigits = false; break; }
            }
            if (allDigits)
            {
                cmd.action = CliAction::SetBrightness;
                cmd.value = (std::clamp)(_wtoi(arg.c_str()), 0, 100);
                break;
            }
        }
    }

    LocalFree(argv);
    return cmd;
}
