#include "MonitorManager.h"
#include "Settings.h"
#include "resource.h"
#include <map>

#pragma comment(lib, "dxva2.lib")

MonitorManager g_MonitorManager;

static std::map<std::wstring, std::vector<std::wstring>> QueryAllDisplayFriendlyNames() {
    std::map<std::wstring, std::vector<std::wstring>> result;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || pathCount == 0) {
        return result;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
        return result;
    }

    for (size_t i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
        sourceName.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
            targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            targetName.header.size = sizeof(targetName);
            targetName.header.adapterId = paths[i].targetInfo.adapterId;
            targetName.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&targetName.header) == ERROR_SUCCESS) {
                if (targetName.monitorFriendlyDeviceName[0] != L'\0') {
                    result[sourceName.viewGdiDeviceName].push_back(targetName.monitorFriendlyDeviceName);
                }
            }
        }
    }
    return result;
}

struct EnumContext {
    std::vector<PHYSICAL_MONITOR>* rawMonitors;
    std::vector<PhysicalMonitorInfo>* monitors;
    std::map<std::wstring, std::vector<std::wstring>> friendlyNames;
};

static BOOL CALLBACK MonitorEnumCallback(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    UNREFERENCED_PARAMETER(hdcMonitor);
    UNREFERENCED_PARAMETER(lprcMonitor);

    EnumContext* ctx = reinterpret_cast<EnumContext*>(dwData);
    if (!ctx) return TRUE;

    MONITORINFOEXW mi = { sizeof(MONITORINFOEXW) };
    bool isPrimary = false;
    std::wstring devName;
    if (GetMonitorInfoW(hMonitor, reinterpret_cast<LPMONITORINFO>(&mi))) {
        isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        devName = mi.szDevice;
    }

    DWORD numMonitors = 0;
    if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &numMonitors) && numMonitors > 0) {
        std::vector<PHYSICAL_MONITOR> phys(numMonitors);
        if (GetPhysicalMonitorsFromHMONITOR(hMonitor, numMonitors, phys.data())) {
            for (DWORD i = 0; i < numMonitors; ++i) {
                ctx->rawMonitors->push_back(phys[i]);

                DWORD minB = 0, curB = 0, maxB = 100;
                if (GetMonitorBrightness(phys[i].hPhysicalMonitor, &minB, &curB, &maxB)) {
                    PhysicalMonitorInfo info;
                    info.hPhysical = phys[i].hPhysicalMonitor;
                    info.description = phys[i].szPhysicalMonitorDescription;
                    info.friendlyName = info.description;
                    info.deviceName = devName;

                    auto it = ctx->friendlyNames.find(devName);
                    if (it != ctx->friendlyNames.end() && !it->second.empty()) {
                        if (i < it->second.size()) {
                            info.friendlyName = it->second[i];
                        } else {
                            info.friendlyName = it->second[0];
                        }
                    }

                    std::wstring cleanDev = devName;
                    size_t slashPos = cleanDev.find_last_of(L"\\/");
                    if (slashPos != std::wstring::npos) {
                        cleanDev = cleanDev.substr(slashPos + 1);
                    }
                    info.id = cleanDev + L"#" + std::to_wstring(i);
                    info.minBrightness = minB;
                    info.maxBrightness = maxB;
                    info.currentBrightness = curB;
                    info.isPrimary = isPrimary;
                    ctx->monitors->push_back(info);
                }
            }
        }
    }
    return TRUE;
}

MonitorManager::MonitorManager()
    : m_cachedBrightness(45), m_driverAvailable(false), m_lastEnumTick(0),
      m_pendingTargetBrightness(-1), m_pendingReinit(false), m_stopWorker(false) {
}

MonitorManager::~MonitorManager() {
    Cleanup();
}

void MonitorManager::StartWorkerThread() {
    if (!m_workerThread.joinable()) {
        m_stopWorker = false;
        m_workerThread = std::thread(&MonitorManager::WorkerLoop, this);
    }
}

void MonitorManager::StopWorkerThread() {
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_stopWorker = true;
        m_pendingTargetBrightness = -1;
        m_pendingReinit = false;
    }
    m_workerCv.notify_all();

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void MonitorManager::Cleanup() {
    StopWorkerThread();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_rawMonitors.empty()) {
        DestroyPhysicalMonitors((DWORD)m_rawMonitors.size(), m_rawMonitors.data());
        m_rawMonitors.clear();
    }
    m_monitors.clear();
    m_driverAvailable = false;
}

static int SafeCalcBrightnessPercent(DWORD curB, DWORD minB, DWORD maxB) {
    if (maxB <= minB) return (int)curB;
    if (curB <= minB) return 0;
    if (curB >= maxB) return 100;
    return (int)((curB - minB) * 100 / (maxB - minB));
}

void MonitorManager::EnumMonitorsInternal() {
    if (!m_rawMonitors.empty()) {
        DestroyPhysicalMonitors((DWORD)m_rawMonitors.size(), m_rawMonitors.data());
        m_rawMonitors.clear();
    }
    m_monitors.clear();

    EnumContext ctx;
    ctx.rawMonitors = &m_rawMonitors;
    ctx.monitors = &m_monitors;
    ctx.friendlyNames = QueryAllDisplayFriendlyNames();

    EnumDisplayMonitors(NULL, NULL, MonitorEnumCallback, reinterpret_cast<LPARAM>(&ctx));

    if (!m_monitors.empty()) {
        m_driverAvailable = true;
        // Prioritize primary monitor for cached brightness
        bool foundPrimary = false;
        for (const auto& mon : m_monitors) {
            if (mon.isPrimary) {
                m_cachedBrightness = SafeCalcBrightnessPercent(mon.currentBrightness, mon.minBrightness, mon.maxBrightness);
                foundPrimary = true;
                break;
            }
        }
        if (!foundPrimary) {
            const auto& mon = m_monitors[0];
            m_cachedBrightness = SafeCalcBrightnessPercent(mon.currentBrightness, mon.minBrightness, mon.maxBrightness);
        }
        if (m_cachedBrightness < 0) m_cachedBrightness = 0;
        if (m_cachedBrightness > 100) m_cachedBrightness = 100;
        g_Settings.cachedBrightness = m_cachedBrightness;

        HWND hMain = FindWindowW(L"ClickBrightness_MessageWndClass", L"ClickBrightness_HiddenMsgWindow");
        if (!hMain) hMain = FindWindowW(L"ClickBrightness_MessageWndClass", NULL);
        if (hMain) {
            PostMessageW(hMain, WM_APP_BRIGHTNESS_UPDATED, m_cachedBrightness, 0);
        }
    } else {
        m_driverAvailable = false;
    }
    m_lastEnumTick = GetTickCount64();
}

bool MonitorManager::Initialize() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cachedBrightness = g_Settings.cachedBrightness;
    }
    StartWorkerThread();
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_pendingReinit = true;
    }
    m_workerCv.notify_one();
    return true;
}

bool MonitorManager::Reinitialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    EnumMonitorsInternal();
    return m_driverAvailable.load();
}

int MonitorManager::GetCurrentBrightness() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_driverAvailable || m_monitors.empty()) {
        // Attempt quick re-enumeration if it's been more than 1 second since last attempt
        if (GetTickCount64() - m_lastEnumTick > 1000) {
            EnumMonitorsInternal();
        }
        return m_cachedBrightness;
    }

    // Find and query primary monitor first if selected
    for (auto& mon : m_monitors) {
        if (mon.isPrimary && g_Settings.IsMonitorSelected(mon.id)) {
            DWORD minB = 0, curB = 0, maxB = 100;
            if (GetMonitorBrightness(mon.hPhysical, &minB, &curB, &maxB)) {
                mon.currentBrightness = curB;
                mon.minBrightness = minB;
                mon.maxBrightness = maxB;
                m_cachedBrightness = SafeCalcBrightnessPercent(curB, minB, maxB);
                g_Settings.cachedBrightness = m_cachedBrightness;
                return m_cachedBrightness;
            }
        }
    }

    // Query first selected monitor
    for (auto& mon : m_monitors) {
        if (g_Settings.IsMonitorSelected(mon.id)) {
            DWORD minB = 0, curB = 0, maxB = 100;
            if (GetMonitorBrightness(mon.hPhysical, &minB, &curB, &maxB)) {
                mon.currentBrightness = curB;
                mon.minBrightness = minB;
                mon.maxBrightness = maxB;
                m_cachedBrightness = SafeCalcBrightnessPercent(curB, minB, maxB);
                g_Settings.cachedBrightness = m_cachedBrightness;
                return m_cachedBrightness;
            }
        }
    }

    // Fallback to first monitor if no selected monitor responded
    if (!m_monitors.empty()) {
        auto& mon = m_monitors[0];
        DWORD minB = 0, curB = 0, maxB = 100;
        if (GetMonitorBrightness(mon.hPhysical, &minB, &curB, &maxB)) {
            mon.currentBrightness = curB;
            mon.minBrightness = minB;
            mon.maxBrightness = maxB;
            m_cachedBrightness = SafeCalcBrightnessPercent(curB, minB, maxB);
            g_Settings.cachedBrightness = m_cachedBrightness;
            return m_cachedBrightness;
        } else {
            m_driverAvailable = false;
        }
    }

    return m_cachedBrightness;
}

bool MonitorManager::SetBrightness(int brightness, bool async) {
    if (brightness < 0) brightness = 0;
    if (brightness > 100) brightness = 100;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cachedBrightness = brightness;
        g_Settings.cachedBrightness = brightness;
    }

    if (async) {
        // Enqueue to worker thread with coalescing
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            m_pendingTargetBrightness = brightness;
        }
        m_workerCv.notify_one();
        return true;
    }

    // Synchronous execution path (fallback or shutdown)
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_driverAvailable || m_monitors.empty()) {
        EnumMonitorsInternal();
        if (!m_driverAvailable || m_monitors.empty()) {
            return false;
        }
    }

    bool allSuccess = true;
    for (const auto& mon : m_monitors) {
        if (!g_Settings.IsMonitorSelected(mon.id)) {
            continue; // Skip monitors not selected
        }
        DWORD targetVal = (mon.maxBrightness > mon.minBrightness)
            ? (mon.minBrightness + (mon.maxBrightness - mon.minBrightness) * brightness / 100)
            : (DWORD)brightness;
        if (!SetMonitorBrightness(mon.hPhysical, targetVal)) {
            allSuccess = false;
        }
    }

    if (!allSuccess) {
        m_driverAvailable = false;
    }

    return allSuccess;
}

void MonitorManager::WorkerLoop() {
    while (true) {
        int targetBrightness = -1;
        bool doReinit = false;

        {
            std::unique_lock<std::mutex> lock(m_workerMutex);
            m_workerCv.wait(lock, [this]() {
                return m_stopWorker || m_pendingTargetBrightness != -1 || m_pendingReinit;
            });

            if (m_stopWorker) {
                break;
            }

            if (m_pendingReinit) {
                m_pendingReinit = false;
                doReinit = true;
            }

            if (m_pendingTargetBrightness != -1) {
                targetBrightness = m_pendingTargetBrightness;
                m_pendingTargetBrightness = -1;
            }
        }

        if (doReinit) {
            std::lock_guard<std::mutex> lock(m_mutex);
            EnumMonitorsInternal();
        }

        if (targetBrightness != -1) {
            struct MonTarget {
                HANDLE hPhysical;
                DWORD targetVal;
            };
            std::vector<MonTarget> targets;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_driverAvailable || m_monitors.empty()) {
                    EnumMonitorsInternal();
                }
                if (m_driverAvailable && !m_monitors.empty()) {
                    bool anySelected = false;
                    for (const auto& mon : m_monitors) {
                        if (g_Settings.IsMonitorSelected(mon.id)) {
                            anySelected = true;
                            break;
                        }
                    }
                    for (const auto& mon : m_monitors) {
                        if (anySelected && !g_Settings.IsMonitorSelected(mon.id)) {
                            continue; // Skip monitors not selected
                        }
                        DWORD targetVal = (mon.maxBrightness > mon.minBrightness)
                            ? (mon.minBrightness + (mon.maxBrightness - mon.minBrightness) * targetBrightness / 100)
                            : (DWORD)targetBrightness;
                        targets.push_back({ mon.hPhysical, targetVal });
                    }
                }
            }

            bool allSuccess = true;
            for (const auto& tgt : targets) {
                // Check if a newer target brightness arrived while writing or worker is stopping
                {
                    std::lock_guard<std::mutex> lock(m_workerMutex);
                    if (m_stopWorker || m_pendingTargetBrightness != -1 || m_pendingReinit) {
                        // Coalesce: abort remaining old writes to immediately handle new target
                        break;
                    }
                }

                // Verify the physical monitor handle is still open and valid (not destroyed by suspend/reinit)
                bool validHandle = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (const auto& mon : m_monitors) {
                        if (mon.hPhysical == tgt.hPhysical) {
                            validHandle = true;
                            break;
                        }
                    }
                }
                if (!validHandle) {
                    break;
                }

                if (SetMonitorBrightness(tgt.hPhysical, tgt.targetVal)) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (auto& mon : m_monitors) {
                        if (mon.hPhysical == tgt.hPhysical) {
                            mon.currentBrightness = tgt.targetVal;
                            break;
                        }
                    }
                } else {
                    allSuccess = false;
                }
            }

            if (!allSuccess && !targets.empty()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_driverAvailable = false;
            }
        }
    }
}

void MonitorManager::OnDisplayChanged() {
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_pendingReinit = true;
    }
    m_workerCv.notify_one();
}

void MonitorManager::OnPowerSuspend() {
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_pendingTargetBrightness = -1;
        m_pendingReinit = false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_rawMonitors.empty()) {
        DestroyPhysicalMonitors((DWORD)m_rawMonitors.size(), m_rawMonitors.data());
        m_rawMonitors.clear();
    }
    m_monitors.clear();
    m_driverAvailable = false;
}

void MonitorManager::OnPowerResume() {
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_pendingReinit = true;
    }
    m_workerCv.notify_one();
}
