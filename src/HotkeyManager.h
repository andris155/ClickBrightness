#pragma once

#include <windows.h>
#include <string>

class HotkeyManager {
public:
    HotkeyManager();
    ~HotkeyManager();

    bool RegisterAll(HWND hwnd);
    void UnregisterAll(HWND hwnd);

    static std::wstring GetKeyDisplayName(UINT vkCode);
    static std::wstring GetModifiersDisplayName(UINT mods);
    static std::wstring FormatHotkey(UINT mods, UINT vkCode);

private:
    HWND m_hwnd;
    bool m_registered;
};

extern HotkeyManager g_HotkeyManager;
