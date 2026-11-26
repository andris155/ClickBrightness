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

struct BrightnessChangeEntry {
    std::wstring monitorId;
    std::wstring displayName;
    int brightness;
    bool useWindowsAccent;
    COLORREF accentColor;
};

struct PhysicalMonitorInfo {
    HANDLE hPhysical;
    std::wstring description;
    std::wstring friendlyName;
    std::wstring deviceName;
    std::wstring id;
    std::wstring wmiInstanceName;
    DWORD minBrightness;
    DWORD maxBrightness;
    DWORD currentBrightness;
    bool isPrimary;
    bool isInternal;
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
    int GetMonitorBrightnessById(const std::wstring& monitorId);
    int GetTargetControlBrightness(const std::wstring& targetMonitorId);
    void GetTargetBounds(const std::wstring& targetMonitorId, int& outMin, int& outMax) const;
    int GetEffectiveSyncedBrightness(const std::wstring& monitorId, int baseBrightness) const;
    int GetCachedBrightness() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cachedBrightness;
    }
    bool SetBrightness(int brightness, bool async = true);
    bool SetMonitorBrightnessTarget(const std::wstring& targetMonitorId, int brightness, bool async = true);
    std::vector<BrightnessChangeEntry> GetChangedMonitorsForTarget(const std::wstring& targetMonitorId, int targetBrightness) const;

    bool IsMainDisplayInternal() const;
    void UpdateFnSyncListener();
    void HandleWmiBrightnessEvent(int brightness);

    bool IsDriverAvailable() const { return m_driverAvailable.load(); }

    void OnDisplayChanged();
    void OnPowerSuspend();
    void OnPowerResume();
    void OnDisplayPowerOff();
    void OnDisplayPowerOn();
    bool IsDisplayPoweredOn() const { return m_displayPoweredOn.load(); }

private:
    void EnumMonitorsInternal();
    void WorkerLoop();
    void StartWorkerThread();
    void StopWorkerThread();
    void GetSyncFirstHitBoundsLocked(int& outFirstHit100, int& outFirstHit0) const;
    int GetEffectiveSyncedBrightnessLocked(const std::wstring& monitorId, int baseBrightness) const;

    mutable std::mutex m_mutex;
    std::vector<PHYSICAL_MONITOR> m_rawMonitors;
    std::vector<PhysicalMonitorInfo> m_monitors;
    int m_cachedBrightness;
    int m_syncBaseBrightness{ 45 };
    std::atomic<bool> m_driverAvailable;
    std::atomic<bool> m_displayPoweredOn;
    std::atomic<int> m_consecutiveFailures;
    ULONGLONG m_lastEnumTick;

    // Async worker thread state
    std::thread m_workerThread;
    std::mutex m_workerMutex;
    std::condition_variable m_workerCv;
    int m_pendingTargetBrightness;
    std::wstring m_pendingTargetMonitorId; // empty = Main/Sync group
    bool m_pendingReinit;
    bool m_stopWorker;

    // Laptop Fn-key WMI event listener
    std::thread m_fnSyncThread;
    std::atomic<bool> m_fnSyncRunning{ false };
    HANDLE m_fnSyncStopEvent{ NULL };
    std::atomic<ULONGLONG> m_lastAppInternalWriteTick{ 0 };
    std::atomic<int> m_lastAppInternalWriteVal{ -1 };
};

extern MonitorManager g_MonitorManager;
