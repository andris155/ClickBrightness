#include "HotkeyManager.h"
#include "Settings.h"
#include "resource.h"

HotkeyManager g_HotkeyManager;

HotkeyManager::HotkeyManager()
    : m_hwnd(NULL), m_registered(false) {
}

HotkeyManager::~HotkeyManager() {
    if (m_hwnd) {
        UnregisterAll(m_hwnd);
    }
}

bool HotkeyManager::RegisterAll(HWND hwnd) {
    m_hwnd = hwnd;
    UnregisterAll(hwnd);

    // Register Hotkey Up & Down (if adjust hotkeys enabled)
    UINT upMods = g_Settings.hotkeyUpMods;
    UINT upKey = g_Settings.hotkeyUpKey;
    BOOL bUp = FALSE;
    UINT downMods = g_Settings.hotkeyDownMods;
    UINT downKey = g_Settings.hotkeyDownKey;
    BOOL bDown = FALSE;

    if (g_Settings.hotkeyAdjustEnabled) {
        if (upKey != 0) {
            bUp = RegisterHotKey(hwnd, ID_HOTKEY_UP, upMods, upKey);
        }
        if (downKey != 0) {
            bDown = RegisterHotKey(hwnd, ID_HOTKEY_DOWN, downMods, downKey);
        }
    }

    // Register Hotkey Presets (if preset hotkeys enabled)
    UINT p1Mods = g_Settings.hotkeyPreset1Mods;
    UINT p1Key = g_Settings.hotkeyPreset1Key;
    BOOL bP1 = FALSE;
    UINT p2Mods = g_Settings.hotkeyPreset2Mods;
    UINT p2Key = g_Settings.hotkeyPreset2Key;
    BOOL bP2 = FALSE;

    if (g_Settings.hotkeyPresetsEnabled) {
        if (p1Key != 0) {
            bP1 = RegisterHotKey(hwnd, ID_HOTKEY_PRESET1, p1Mods, p1Key);
        }
        if (p2Key != 0) {
            bP2 = RegisterHotKey(hwnd, ID_HOTKEY_PRESET2, p2Mods, p2Key);
        }
    }

    // Register Hotkey Preset Toggle (if preset toggle shortcut enabled)
    UINT ptMods = g_Settings.hotkeyPresetToggleMods;
    UINT ptKey = g_Settings.hotkeyPresetToggleKey;
    BOOL bPT = FALSE;

    if (g_Settings.hotkeyPresetToggleEnabled) {
        if (ptKey != 0) {
            bPT = RegisterHotKey(hwnd, ID_HOTKEY_PRESET_TOGGLE, ptMods, ptKey);
        }
    }

    bool adjustOk = (!g_Settings.hotkeyAdjustEnabled) || ((upKey == 0 || bUp) && (downKey == 0 || bDown));
    bool presetsOk = (!g_Settings.hotkeyPresetsEnabled) || ((p1Key == 0 || bP1) && (p2Key == 0 || bP2));
    bool toggleOk = (!g_Settings.hotkeyPresetToggleEnabled) || (ptKey == 0 || bPT);
    m_registered = adjustOk && presetsOk && toggleOk;
    return m_registered;
}

void HotkeyManager::UnregisterAll(HWND hwnd) {
    UnregisterHotKey(hwnd, ID_HOTKEY_UP);
    UnregisterHotKey(hwnd, ID_HOTKEY_DOWN);
    UnregisterHotKey(hwnd, ID_HOTKEY_PRESET1);
    UnregisterHotKey(hwnd, ID_HOTKEY_PRESET2);
    UnregisterHotKey(hwnd, ID_HOTKEY_PRESET_TOGGLE);
    m_registered = false;
}

std::wstring HotkeyManager::GetKeyDisplayName(UINT vkCode) {
    if (vkCode == 0) {
        return L"None";
    }

    // Function keys F1 - F24
    if (vkCode >= VK_F1 && vkCode <= VK_F24) {
        wchar_t buf[16];
        swprintf_s(buf, L"F%d", 1 + (vkCode - VK_F1));
        return buf;
    }

    // Common navigation / editing / special keys
    switch (vkCode) {
    case VK_UP: return L"Up Arrow";
    case VK_DOWN: return L"Down Arrow";
    case VK_LEFT: return L"Left Arrow";
    case VK_RIGHT: return L"Right Arrow";
    case VK_PRIOR: return L"Page Up";
    case VK_NEXT: return L"Page Down";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_SPACE: return L"Space";
    case VK_RETURN: return L"Enter";
    case VK_BACK: return L"Backspace";
    case VK_TAB: return L"Tab";
    case VK_ESCAPE: return L"Esc";
    case VK_PAUSE: return L"Pause";
    case VK_CAPITAL: return L"Caps Lock";
    case VK_SCROLL: return L"Scroll Lock";
    case VK_PRINT:
    case VK_SNAPSHOT: return L"Print Screen";
    case VK_VOLUME_UP: return L"Volume Up";
    case VK_VOLUME_DOWN: return L"Volume Down";
    case VK_VOLUME_MUTE: return L"Volume Mute";
    case VK_MEDIA_NEXT_TRACK: return L"Next Track";
    case VK_MEDIA_PREV_TRACK: return L"Prev Track";
    case VK_MEDIA_PLAY_PAUSE: return L"Play/Pause";
    case VK_NUMPAD0: return L"Num 0";
    case VK_NUMPAD1: return L"Num 1";
    case VK_NUMPAD2: return L"Num 2";
    case VK_NUMPAD3: return L"Num 3";
    case VK_NUMPAD4: return L"Num 4";
    case VK_NUMPAD5: return L"Num 5";
    case VK_NUMPAD6: return L"Num 6";
    case VK_NUMPAD7: return L"Num 7";
    case VK_NUMPAD8: return L"Num 8";
    case VK_NUMPAD9: return L"Num 9";
    case VK_MULTIPLY: return L"Num *";
    case VK_ADD: return L"Num +";
    case VK_SUBTRACT: return L"Num -";
    case VK_DECIMAL: return L"Num .";
    case VK_DIVIDE: return L"Num /";
    }

    // Alphanumeric keys 'A'-'Z' and '0'-'9'
    if ((vkCode >= 'A' && vkCode <= 'Z') || (vkCode >= '0' && vkCode <= '9')) {
        return std::wstring(1, (wchar_t)vkCode);
    }

    // Query Windows keyboard layout for exact key name
    UINT scanCode = MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC);
    if (scanCode != 0) {
        LONG lParam = (scanCode << 16);
        if (vkCode == VK_RCONTROL || vkCode == VK_RMENU || vkCode == VK_INSERT ||
            vkCode == VK_DELETE || vkCode == VK_HOME || vkCode == VK_END ||
            vkCode == VK_PRIOR || vkCode == VK_NEXT || vkCode == VK_LEFT ||
            vkCode == VK_RIGHT || vkCode == VK_UP || vkCode == VK_DOWN ||
            vkCode == VK_DIVIDE || vkCode == VK_NUMLOCK) {
            lParam |= (1 << 24);
        }
        wchar_t keyName[64] = { 0 };
        if (GetKeyNameTextW(lParam, keyName, 64) > 0) {
            return keyName;
        }
    }

    wchar_t buf[32];
    swprintf_s(buf, L"Key 0x%02X", vkCode);
    return buf;
}

std::wstring HotkeyManager::GetModifiersDisplayName(UINT mods) {
    std::wstring result;
    if (mods & MOD_CONTROL) result += L"Ctrl + ";
    if (mods & MOD_ALT)     result += L"Alt + ";
    if (mods & MOD_SHIFT)   result += L"Shift + ";
    if (mods & MOD_WIN)     result += L"Win + ";
    return result;
}

std::wstring HotkeyManager::FormatHotkey(UINT mods, UINT vkCode) {
    std::wstring result = GetModifiersDisplayName(mods);
    result += GetKeyDisplayName(vkCode);
    return result;
}
