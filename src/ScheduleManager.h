#pragma once

#include <windows.h>
#include <string>

struct MonitorControlConfig;

namespace ScheduleManager {

void Init(HWND hWnd);
void Uninit();
void Tick();
void Refresh();

struct TodaySunInfo {
    bool valid = false;
    int riseHour = 0;
    int riseMin = 0;
    int setHour = 0;
    int setMin = 0;
    int effectiveRiseHour = 0;
    int effectiveRiseMin = 0;
    int effectiveSetHour = 0;
    int effectiveSetMin = 0;
    std::wstring text;
};

TodaySunInfo GetTodaySunInfo(double lat, double lon, int sunriseOffset = 0, int sunsetOffset = 0);
int GetDesiredStateForConfig(const MonitorControlConfig& cfg); // -1 = Disabled, 0 = Night, 1 = Day

} // namespace ScheduleManager
