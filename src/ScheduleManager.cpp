#include "pch.h"
#include "ScheduleManager.h"
#include "SolarCalculator.h"
#include "Settings.h"
#include "MonitorManager.h"
#include "OverlayWindow.h"
#include "FlyoutWindow.h"
#include "TrayIcon.h"
#include "resource.h"
#include <map>
#include <algorithm>

namespace ScheduleManager {

static HWND s_hWnd = NULL;
static std::map<std::wstring, int> s_lastEvaluatedStates;

void Init(HWND hWnd) {
    s_hWnd = hWnd;
    if (s_hWnd) {
        SetTimer(s_hWnd, IDT_SCHEDULE_TICK, 15000, NULL);
    }
    Refresh();
}

void Uninit() {
    if (s_hWnd) {
        KillTimer(s_hWnd, IDT_SCHEDULE_TICK);
        KillTimer(s_hWnd, IDT_SCHEDULE_EXACT);
        s_hWnd = NULL;
    }
    s_lastEvaluatedStates.clear();
}

TodaySunInfo GetTodaySunInfo(double lat, double lon, int sunriseOffset, int sunsetOffset) {
    TodaySunInfo info;
    SYSTEMTIME st;
    GetLocalTime(&st);
    auto times = SolarCalculator::Calculate(lat, lon, st);
    if (times.hasSunrise && times.hasSunset) {
        info.valid = true;
        info.riseHour = times.sunriseHour;
        info.riseMin = times.sunriseMinute;
        info.setHour = times.sunsetHour;
        info.setMin = times.sunsetMinute;

        int effRise = (times.sunriseHour * 60 + times.sunriseMinute + sunriseOffset) % 1440;
        if (effRise < 0) effRise += 1440;
        int effSet = (times.sunsetHour * 60 + times.sunsetMinute + sunsetOffset) % 1440;
        if (effSet < 0) effSet += 1440;

        info.effectiveRiseHour = effRise / 60;
        info.effectiveRiseMin = effRise % 60;
        info.effectiveSetHour = effSet / 60;
        info.effectiveSetMin = effSet % 60;

        wchar_t buf[64];
        swprintf_s(buf, L"%02d:%02d | %02d:%02d", info.riseHour, info.riseMin, info.setHour, info.setMin);
        info.text = buf;
    }
    return info;
}

static bool GetTransitionMinutesForConfig(const MonitorControlConfig& cfg, const SYSTEMTIME& st, int& outDayM, int& outNightM) {
    if (cfg.scheduleMode == 1) {
        outDayM = cfg.scheduleDayHour * 60 + cfg.scheduleDayMinute;
        outNightM = cfg.scheduleNightHour * 60 + cfg.scheduleNightMinute;
        return true;
    }
    if (cfg.scheduleMode == 2) {
        auto times = SolarCalculator::Calculate(cfg.scheduleLatitude, cfg.scheduleLongitude, st);
        if (times.hasSunrise && times.hasSunset) {
            outDayM = (times.sunriseHour * 60 + times.sunriseMinute + cfg.scheduleSunriseOffset) % 1440;
            if (outDayM < 0) outDayM += 1440;
            outNightM = (times.sunsetHour * 60 + times.sunsetMinute + cfg.scheduleSunsetOffset) % 1440;
            if (outNightM < 0) outNightM += 1440;
            return true;
        }
    }
    return false;
}

int GetDesiredStateForConfig(const MonitorControlConfig& cfg) {
    if (cfg.scheduleMode == 0) {
        return -1;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    int nowM = st.wHour * 60 + st.wMinute;

    int dayM = 0, nightM = 0;
    if (!GetTransitionMinutesForConfig(cfg, st, dayM, nightM)) {
        return -1;
    }

    if (dayM < nightM) {
        return (nowM >= dayM && nowM < nightM) ? 1 : 0;
    } else if (nightM < dayM) {
        return (nowM >= nightM && nowM < dayM) ? 0 : 1;
    }
    return 1;
}

static void ScheduleExactNextSecondIfImminent(const MonitorControlConfig& cfg, const SYSTEMTIME& st) {
    if (!s_hWnd || cfg.scheduleMode == 0) return;
    int dayM = 0, nightM = 0;
    if (!GetTransitionMinutesForConfig(cfg, st, dayM, nightM)) return;

    int nowM = st.wHour * 60 + st.wMinute;
    int nextM = (nowM + 1) % 1440;
    if (nextM == dayM || nextM == nightM) {
        UINT ms = (UINT)((std::max)(1, 60 - (int)st.wSecond)) * 1000u + 50u;
        SetTimer(s_hWnd, IDT_SCHEDULE_EXACT, ms, NULL);
    }
}

void Tick() {
    if (!g_MonitorManager.IsDriverAvailable() || !g_MonitorManager.IsDisplayPoweredOn() ||
        g_MonitorManager.IsSessionLocked() || g_MonitorManager.IsRemoteSession()) {
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);

    // 1. Main Display / Sync Group
    if (!g_Settings.mainMonitorId.empty()) {
        const auto mainCfg = g_Settings.GetEffectiveMonitorConfig(g_Settings.mainMonitorId);
        int desired = GetDesiredStateForConfig(mainCfg);
        if (desired == -1) {
            s_lastEvaluatedStates.erase(L"__SYNC__");
        } else {
            ScheduleExactNextSecondIfImminent(mainCfg, st);
            auto it = s_lastEvaluatedStates.find(L"__SYNC__");
            if (it == s_lastEvaluatedStates.end() || it->second != desired) {
                int targetB = (std::clamp)(desired == 1 ? mainCfg.scheduleDayBrightness : mainCfg.scheduleNightBrightness, 0, 100);
                if (mainCfg.scheduleShowOsd) {
                    g_OverlayWindow.TriggerBrightnessSetForTarget(L"", targetB, true);
                } else {
                    g_MonitorManager.SetMonitorBrightnessTarget(L"", targetB, true);
                    g_TrayIcon.UpdateBrightness(targetB);
                    g_FlyoutWindow.SetBrightness(targetB, false);
                }
                s_lastEvaluatedStates[L"__SYNC__"] = desired;
            }
        }
    }

    // 2. Separate-mode non-main monitors
    for (const auto& mon : g_MonitorManager.GetMonitors()) {
        if (!g_Settings.mainMonitorId.empty() && _wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0) {
            continue;
        }
        const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
        if (!cfg || !cfg->enabled || cfg->controlMode != 1) {
            s_lastEvaluatedStates.erase(mon.id);
            continue;
        }
        int desired = GetDesiredStateForConfig(*cfg);
        if (desired == -1) {
            s_lastEvaluatedStates.erase(mon.id);
            continue;
        }
        ScheduleExactNextSecondIfImminent(*cfg, st);
        auto it = s_lastEvaluatedStates.find(mon.id);
        if (it == s_lastEvaluatedStates.end() || it->second != desired) {
            int targetB = (std::clamp)(desired == 1 ? cfg->scheduleDayBrightness : cfg->scheduleNightBrightness, 0, 100);
            if (cfg->scheduleShowOsd) {
                g_OverlayWindow.TriggerBrightnessSetForTarget(mon.id, targetB, true);
            } else {
                g_MonitorManager.SetMonitorBrightnessTarget(mon.id, targetB, true);
                g_TrayIcon.UpdateBrightnessForTarget(mon.id, targetB);
            }
            s_lastEvaluatedStates[mon.id] = desired;
        }
    }
}

void Refresh() {
    s_lastEvaluatedStates.clear();
    Tick();
}

} // namespace ScheduleManager
