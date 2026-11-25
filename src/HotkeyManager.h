#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct RegisteredHotkeyTarget {
    int id;
    std::wstring targetMonitorId; // empty = Main/Sync
    int action; // 0 = Up, 1 = Down, 2 = Preset1, 3 = Preset2, 4 = Toggle
    UINT vkCode;
};

class HotkeyManager {
public:
    HotkeyManager();
    ~HotkeyManager();

    bool RegisterAll(HWND hwnd);
    void UnregisterAll(HWND hwnd);
    bool ResolveHotkey(int id, std::wstring& outTargetMonitorId, int& outAction, UINT& outVkCode) const;

    static std::wstring GetKeyDisplayName(UINT vkCode);
    static std::wstring GetModifiersDisplayName(UINT mods);
    static std::wstring FormatHotkey(UINT mods, UINT vkCode);

private:
    HWND m_hwnd;
    bool m_registered;
    std::vector<RegisteredHotkeyTarget> m_bindings;
};

extern HotkeyManager g_HotkeyManager;
