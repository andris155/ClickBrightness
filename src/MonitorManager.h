#pragma once

#include <windows.h>
#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <vector>
#include <mutex>
#include <string>

#include <thread>
#include <condition_variable>
#include <atomic>

struct PhysicalMonitorInfo {
    HANDLE hPhysical;
    std::wstring description;
    std::wstring friendlyName;
    std::wstring deviceName;
    std::wstring id;
    DWORD minBrightness;
    DWORD maxBrightness;
    DWORD currentBrightness;
    bool isPrimary;
};

class MonitorManager {
public:
    MonitorManager();
    ~MonitorManager();

    bool Initialize();
    void Cleanup();
    bool Reinitialize();

    std::vector<PhysicalMonitorInfo> GetMonitors() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_monitors;
    }

    int GetCurrentBrightness();
    int GetCachedBrightness() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cachedBrightness;
    }
    bool SetBrightness(int brightness, bool async = true);

    bool IsDriverAvailable() const { return m_driverAvailable.load(); }

    void OnDisplayChanged();
    void OnPowerSuspend();
    void OnPowerResume();

private:
    void EnumMonitorsInternal();
    void WorkerLoop();
    void StartWorkerThread();
    void StopWorkerThread();

    mutable std::mutex m_mutex;
    std::vector<PHYSICAL_MONITOR> m_rawMonitors;
    std::vector<PhysicalMonitorInfo> m_monitors;
    int m_cachedBrightness;
    std::atomic<bool> m_driverAvailable;
    ULONGLONG m_lastEnumTick;

    // Async worker thread state
    std::thread m_workerThread;
    std::mutex m_workerMutex;
    std::condition_variable m_workerCv;
    int m_pendingTargetBrightness;
    bool m_pendingReinit;
    bool m_stopWorker;
};

extern MonitorManager g_MonitorManager;
