#pragma once

#define IDI_APP_ICON                    101

// Tray context menu items
#define IDM_TRAY_OPTIONS                1001
#define IDM_TRAY_ABOUT                  1002
#define IDM_TRAY_EXIT                   1003

// Hotkey IDs
#define ID_HOTKEY_UP                    2001
#define ID_HOTKEY_DOWN                  2002
#define ID_HOTKEY_PRESET1               2003
#define ID_HOTKEY_PRESET2               2004

// IPC Event Names
#define EVENT_NAME_SHOW_SETTINGS        L"ClickBrightness_Event_ShowSettings"
#define EVENT_NAME_EXIT                 L"ClickBrightness_Event_Exit"
#define EVENT_NAME_BRIGHTNESS_UP        L"ClickBrightness_Event_Up"
#define EVENT_NAME_BRIGHTNESS_DOWN      L"ClickBrightness_Event_Down"

// Timers
#define IDT_HOTKEY_DEBOUNCE             3001
#define IDT_OVERLAY_HIDE                3002
#define IDT_MONITOR_RETRY               3003
#define IDT_GPU_RESET_COOLDOWN          3004
#define IDT_OVERLAY_UNCLOAK             3005

// Custom Windows Messages
#define WM_APP_TRAYMSG                  (WM_APP + 1)
#define WM_APP_BRIGHTNESS_UPDATED       (WM_APP + 2)
#define WM_APP_TRAY_WHEEL               (WM_APP + 3)
