#pragma once

#include <windows.h>
#include <string>
#include <algorithm>
#include <vector>
#include <cwctype>
#include "MonitorManager.h"
#include "Settings.h"

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
    std::wstring monitorSelector; // Empty = Main/Sync group; otherwise index (1..N), ID, or name
    bool allMonitors = false;     // Apply adjustment to Main/Sync + all enabled Separate monitors
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
    return L"\r\nClickBrightness v2.0.0 - Hardware DDC/CI Monitor Brightness Control\r\n\r\n"
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
           L"  --monitor <target>          Target a specific display by index (1..N), ID, name, or 'sync'\r\n"
           L"  --all, -a                   Apply adjustment to Main/Sync and all Separate displays\r\n"
           L"  --osd                       Display on-screen banner during adjustment (default)\r\n"
           L"  --no-osd                    Suppress on-screen banner (silent adjustment)\r\n\r\n"
           L"Examples:\r\n"
           L"  ClickBrightness.exe --set 65\r\n"
           L"  ClickBrightness.exe --monitor 2 --set 40 --no-osd\r\n"
           L"  ClickBrightness.exe --monitor \"ASUS\" --up 10\r\n"
           L"  ClickBrightness.exe --all --set 50\r\n"
           L"  ClickBrightness.exe --get --monitor 2\r\n"
           L"  ClickBrightness.exe --monitors\r\n"
           L"  ClickBrightness.exe --flyout --monitor 2\r\n\r\n";
}

inline bool ResolveCliMonitorTarget(MonitorManager& monMgr, const std::wstring& selector, std::wstring& outTargetId)
{
    if (selector.empty() || _wcsicmp(selector.c_str(), L"main") == 0 || _wcsicmp(selector.c_str(), L"sync") == 0)
    {
        outTargetId.clear();
        return true;
    }

    auto monitors = monMgr.GetMonitors();
    if (monitors.empty()) return false;

    // 1. Check 1-based index
    bool allDigits = true;
    for (wchar_t ch : selector)
    {
        if (!iswdigit(ch)) { allDigits = false; break; }
    }
    if (allDigits)
    {
        int idx = _wtoi(selector.c_str());
        if (idx >= 1 && idx <= static_cast<int>(monitors.size()))
        {
            const auto& mon = monitors[idx - 1];
            const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
            bool isMain = (!g_Settings.mainMonitorId.empty() && _wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
            if (isMain || !cfg || cfg->controlMode == 0)
            {
                outTargetId.clear();
            }
            else
            {
                outTargetId = mon.id;
            }
            return true;
        }
    }

    // 2. Exact monitor ID match
    for (const auto& mon : monitors)
    {
        if (_wcsicmp(mon.id.c_str(), selector.c_str()) == 0)
        {
            const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
            bool isMain = (!g_Settings.mainMonitorId.empty() && _wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
            outTargetId = (isMain || !cfg || cfg->controlMode == 0) ? L"" : mon.id;
            return true;
        }
    }

    // 3. Substring match on custom name, friendlyName, or description
    std::wstring selLower = selector;
    std::transform(selLower.begin(), selLower.end(), selLower.begin(), ::towlower);
    for (const auto& mon : monitors)
    {
        const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
        std::wstring custom = cfg ? cfg->customName : L"";
        std::wstring friendly = mon.friendlyName;
        std::wstring desc = mon.description;
        std::transform(custom.begin(), custom.end(), custom.begin(), ::towlower);
        std::transform(friendly.begin(), friendly.end(), friendly.begin(), ::towlower);
        std::transform(desc.begin(), desc.end(), desc.begin(), ::towlower);

        if ((!custom.empty() && custom.find(selLower) != std::wstring::npos) ||
            (!friendly.empty() && friendly.find(selLower) != std::wstring::npos) ||
            (!desc.empty() && desc.find(selLower) != std::wstring::npos))
        {
            bool isMain = (!g_Settings.mainMonitorId.empty() && _wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
            outTargetId = (isMain || !cfg || cfg->controlMode == 0) ? L"" : mon.id;
            return true;
        }
    }

    return false;
}

inline std::wstring GetMonitorsText(MonitorManager& monMgr)
{
    auto monitors = monMgr.GetMonitors();
    if (monitors.empty())
    {
        return L"\r\nNo DDC/CI compatible monitors detected (or driver still initializing).\r\n\r\n";
    }

    std::wstring out = L"\r\nDetected Physical Monitors:\r\n";
    int idx = 1;
    for (const auto& mon : monitors)
    {
        const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
        std::wstring dispName = (cfg && !cfg->customName.empty()) ? cfg->customName
            : (!mon.friendlyName.empty() ? mon.friendlyName : (!mon.description.empty() ? mon.description : mon.deviceName));
        bool isMain = (!g_Settings.mainMonitorId.empty() && _wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
        std::wstring modeStr = isMain ? L"Main/Sync" : ((cfg && cfg->controlMode == 1) ? L"Separate" : L"Sync");

        out += L"  [" + std::to_wstring(idx++) + L"] " + dispName;
        if (mon.isPrimary) out += L" (Primary)";
        if (mon.isPoweredOff) out += L" (Off)";
        out += L" | ID: " + mon.id;
        out += L" | Mode: " + modeStr;
        out += L" | Brightness: " + std::to_wstring(mon.currentBrightness) + L"%";
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

    // Check for --no-osd / --osd / --all / --monitor anywhere in arguments
    int skipArgIdx = -1;
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
        else if (_wcsicmp(arg.c_str(), L"--all") == 0 || _wcsicmp(arg.c_str(), L"-a") == 0)
        {
            cmd.allMonitors = true;
        }
        else if (_wcsicmp(arg.c_str(), L"--monitor") == 0 || _wcsicmp(arg.c_str(), L"-mon") == 0)
        {
            if (i + 1 < argc)
            {
                cmd.monitorSelector = argv[i + 1];
                skipArgIdx = i + 1;
                ++i;
            }
        }
        else if (arg.rfind(L"--monitor=", 0) == 0)
        {
            cmd.monitorSelector = arg.substr(10);
        }
        else if (arg.rfind(L"-mon=", 0) == 0)
        {
            cmd.monitorSelector = arg.substr(5);
        }
    }

    for (int i = 0; i < argc; ++i)
    {
        if (i == skipArgIdx) continue;
        std::wstring arg = argv[i];
        if (_wcsicmp(arg.c_str(), L"--monitor") == 0 || _wcsicmp(arg.c_str(), L"-mon") == 0 ||
            arg.rfind(L"--monitor=", 0) == 0 || arg.rfind(L"-mon=", 0) == 0 ||
            _wcsicmp(arg.c_str(), L"--all") == 0 || _wcsicmp(arg.c_str(), L"-a") == 0)
        {
            continue;
        }

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
