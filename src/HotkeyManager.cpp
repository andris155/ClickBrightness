#include "HotkeyManager.h"
#include "Settings.h"
#include "MonitorManager.h"
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

    bool allOk = true;
    int nextId = 2001;
    std::vector<std::pair<UINT, UINT>> usedKeys;

    auto registerGroup = [&](const std::wstring& targetMonId, const MonitorControlConfig& cfg) {
        auto regOne = [&](int action, UINT mods, UINT vk) {
            if (vk == 0) return true;
            for (const auto& uk : usedKeys) {
                if (uk.first == mods && uk.second == vk) {
                    return true; // Skip duplicate hotkey so one hotkey is never used multiple times
                }
            }
            int id = nextId++;
            if (RegisterHotKey(hwnd, id, mods, vk)) {
                usedKeys.push_back({ mods, vk });
                m_bindings.push_back({ id, targetMonId, action, vk });
                return true;
            }
            return false;
        };

        if (cfg.hotkeyAdjustEnabled) {
            if (!regOne(0, cfg.hotkeyUpMods, cfg.hotkeyUpKey)) allOk = false;
            if (!regOne(1, cfg.hotkeyDownMods, cfg.hotkeyDownKey)) allOk = false;
        }
        if (cfg.hotkeyPresetsEnabled) {
            if (!regOne(2, cfg.hotkeyPreset1Mods, cfg.hotkeyPreset1Key)) allOk = false;
            if (!regOne(3, cfg.hotkeyPreset2Mods, cfg.hotkeyPreset2Key)) allOk = false;
        }
        if (cfg.hotkeyPresetToggleEnabled) {
            if (!regOne(4, cfg.hotkeyPresetToggleMods, cfg.hotkeyPresetToggleKey)) allOk = false;
        }
    };

    // Main/Sync group
    registerGroup(L"", g_Settings.GetEffectiveMonitorConfig(g_Settings.mainMonitorId));

    // Enabled Separate non-main monitors
    for (const auto& mon : g_MonitorManager.GetMonitors()) {
        if (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0) continue;
        const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
        if (cfg && cfg->enabled && cfg->controlMode == 1) {
            registerGroup(mon.id, *cfg);
        }
    }

    m_registered = allOk;
    return m_registered;
}

void HotkeyManager::UnregisterAll(HWND hwnd) {
    for (const auto& b : m_bindings) {
        UnregisterHotKey(hwnd, b.id);
    }
    m_bindings.clear();
    UnregisterHotKey(hwnd, ID_HOTKEY_UP);
    UnregisterHotKey(hwnd, ID_HOTKEY_DOWN);
    UnregisterHotKey(hwnd, ID_HOTKEY_PRESET1);
    UnregisterHotKey(hwnd, ID_HOTKEY_PRESET2);
    UnregisterHotKey(hwnd, ID_HOTKEY_PRESET_TOGGLE);
    m_registered = false;
}

bool HotkeyManager::ResolveHotkey(int id, std::wstring& outTargetMonitorId, int& outAction, UINT& outVkCode) const {
    for (const auto& b : m_bindings) {
        if (b.id == id) {
            outTargetMonitorId = b.targetMonitorId;
            outAction = b.action;
            outVkCode = b.vkCode;
            return true;
        }
    }
    return false;
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
