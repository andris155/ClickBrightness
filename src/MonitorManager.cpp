#include "MonitorManager.h"
#include "Settings.h"
#include "resource.h"
#include <map>
#include <wbemidl.h>
#include <wrl/client.h>

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

MonitorManager g_MonitorManager;

struct DisplayConfigSummary {
    std::map<std::wstring, std::vector<std::wstring>> friendlyNames;
    std::wstring internalFriendlyName;
    std::wstring internalDeviceName;
};

static bool IsInternalOutputTechnology(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY tech) {
    return tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
           tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
           tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED ||
           tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS;
}

static DisplayConfigSummary QueryDisplayConfigInfo() {
    DisplayConfigSummary summary;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || pathCount == 0) {
        return summary;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
        return summary;
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
                std::wstring friendly = targetName.monitorFriendlyDeviceName;
                if (!friendly.empty()) {
                    summary.friendlyNames[sourceName.viewGdiDeviceName].push_back(friendly);
                }
                if (IsInternalOutputTechnology(paths[i].targetInfo.outputTechnology) && summary.internalDeviceName.empty()) {
                    summary.internalDeviceName = sourceName.viewGdiDeviceName;
                    summary.internalFriendlyName = !friendly.empty() ? friendly : L"Internal Display";
                }
            }
        }
    }
    return summary;
}

struct ScopedBstr {
    BSTR bstr = nullptr;
    ScopedBstr(const wchar_t* s) : bstr(s ? SysAllocString(s) : nullptr) {}
    ~ScopedBstr() { if (bstr) SysFreeString(bstr); }
    operator BSTR() const { return bstr; }
};

struct ScopedComInit {
    bool shouldUninit = false;
    ScopedComInit() {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (hr == S_OK || hr == S_FALSE) {
            shouldUninit = true;
        }
    }
    ~ScopedComInit() {
        if (shouldUninit) {
            CoUninitialize();
        }
    }
};

static bool ConnectWmiServices(Microsoft::WRL::ComPtr<IWbemServices>& pSvc) {
    if (pSvc) return true;

    Microsoft::WRL::ComPtr<IWbemLocator> pLoc;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pLoc));
    if (FAILED(hr) || !pLoc) return false;

    ScopedBstr ns(L"ROOT\\WMI");
    hr = pLoc->ConnectServer(ns, NULL, NULL, NULL, 0, NULL, NULL, &pSvc);
    if (FAILED(hr) || !pSvc) {
        pSvc.Reset();
        return false;
    }

    CoSetProxyBlanket(
        pSvc.Get(),
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        NULL,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL,
        EOAC_NONE
    );
    return true;
}

static std::wstring EscapeWmiPathString(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'"') {
            out.push_back(L'\\');
        }
        out.push_back(c);
    }
    return out;
}

static bool ReadWmiBrightnessVariant(IWbemClassObject* pObj, const wchar_t* propName, DWORD& outVal) {
    VARIANT vtProp;
    VariantInit(&vtProp);
    HRESULT hr = pObj->Get(propName, 0, &vtProp, 0, 0);
    bool ok = false;
    if (SUCCEEDED(hr)) {
        if (vtProp.vt == VT_UI1) { outVal = vtProp.bVal; ok = true; }
        else if (vtProp.vt == VT_I4) { outVal = static_cast<DWORD>(vtProp.lVal); ok = true; }
        else if (vtProp.vt == VT_UI4) { outVal = vtProp.ulVal; ok = true; }
        else if (vtProp.vt == VT_I2) { outVal = static_cast<DWORD>(vtProp.iVal); ok = true; }
        else if (vtProp.vt == VT_UI2) { outVal = vtProp.uiVal; ok = true; }
    }
    VariantClear(&vtProp);
    return ok;
}

static void EnumInternalMonitorsWmi(std::vector<PhysicalMonitorInfo>& monitors, const DisplayConfigSummary& dispCfg) {
    ScopedComInit com;
    Microsoft::WRL::ComPtr<IWbemServices> pSvc;
    if (!ConnectWmiServices(pSvc)) return;

    ScopedBstr wql(L"WQL");
    ScopedBstr query(L"SELECT InstanceName, CurrentBrightness, Active FROM WmiMonitorBrightness");
    Microsoft::WRL::ComPtr<IEnumWbemClassObject> pEnum;
    HRESULT hr = pSvc->ExecQuery(wql, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnum);
    if (FAILED(hr) || !pEnum) return;

    // Determine if the internal display device is currently the primary monitor
    bool internalIsPrimary = false;
    if (!dispCfg.internalDeviceName.empty()) {
        DISPLAY_DEVICEW dd = { sizeof(dd) };
        for (DWORD idx = 0; EnumDisplayDevicesW(NULL, idx, &dd, 0); ++idx) {
            if (_wcsicmp(dd.DeviceName, dispCfg.internalDeviceName.c_str()) == 0) {
                internalIsPrimary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
                break;
            }
        }
    } else if (monitors.empty()) {
        internalIsPrimary = true;
    }

    int internalIndex = 0;
    Microsoft::WRL::ComPtr<IWbemClassObject> pObj;
    ULONG uReturn = 0;
    while (SUCCEEDED(pEnum->Next(WBEM_INFINITE, 1, &pObj, &uReturn)) && uReturn > 0) {
        VARIANT vtActive;
        VariantInit(&vtActive);
        bool isActive = true;
        if (SUCCEEDED(pObj->Get(L"Active", 0, &vtActive, 0, 0)) && vtActive.vt == VT_BOOL) {
            isActive = (vtActive.boolVal != VARIANT_FALSE);
        }
        VariantClear(&vtActive);

        if (isActive) {
            VARIANT vtInst;
            VariantInit(&vtInst);
            std::wstring instName;
            if (SUCCEEDED(pObj->Get(L"InstanceName", 0, &vtInst, 0, 0)) && vtInst.vt == VT_BSTR && vtInst.bstrVal) {
                instName = vtInst.bstrVal;
            }
            VariantClear(&vtInst);

            DWORD curB = 50;
            if (!instName.empty() && ReadWmiBrightnessVariant(pObj.Get(), L"CurrentBrightness", curB)) {
                if (curB > 100) curB = 100;
                PhysicalMonitorInfo info;
                info.hPhysical = NULL;
                info.isInternal = true;
                info.wmiInstanceName = instName;
                info.description = L"Internal Display (WMI)";
                info.friendlyName = !dispCfg.internalFriendlyName.empty() ? dispCfg.internalFriendlyName : L"Internal Display";
                info.deviceName = dispCfg.internalDeviceName;
                info.id = L"INTERNAL#" + std::to_wstring(internalIndex++);
                info.minBrightness = 0;
                info.maxBrightness = 100;
                info.currentBrightness = curB;
                info.isPrimary = internalIsPrimary;
                monitors.push_back(info);
            }
        }
        pObj.Reset();
    }
}

static bool GetInternalBrightnessWmi(const std::wstring& instanceName, DWORD& outBrightness) {
    ScopedComInit com;
    Microsoft::WRL::ComPtr<IWbemServices> pSvc;
    if (!ConnectWmiServices(pSvc)) return false;

    ScopedBstr wql(L"WQL");
    ScopedBstr query(L"SELECT InstanceName, CurrentBrightness, Active FROM WmiMonitorBrightness");
    Microsoft::WRL::ComPtr<IEnumWbemClassObject> pEnum;
    if (FAILED(pSvc->ExecQuery(wql, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnum)) || !pEnum) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWbemClassObject> pObj;
    ULONG uReturn = 0;
    while (SUCCEEDED(pEnum->Next(WBEM_INFINITE, 1, &pObj, &uReturn)) && uReturn > 0) {
        VARIANT vtInst;
        VariantInit(&vtInst);
        bool match = false;
        if (SUCCEEDED(pObj->Get(L"InstanceName", 0, &vtInst, 0, 0)) && vtInst.vt == VT_BSTR && vtInst.bstrVal) {
            match = (instanceName.empty() || _wcsicmp(vtInst.bstrVal, instanceName.c_str()) == 0);
        }
        VariantClear(&vtInst);

        if (match && ReadWmiBrightnessVariant(pObj.Get(), L"CurrentBrightness", outBrightness)) {
            if (outBrightness > 100) outBrightness = 100;
            return true;
        }
        pObj.Reset();
    }
    return false;
}

static bool SetInternalBrightnessWmi(IWbemServices* pSvc, const std::wstring& instanceName, DWORD brightness) {
    if (!pSvc || instanceName.empty()) return false;
    if (brightness > 100) brightness = 100;

    ScopedBstr className(L"WmiMonitorBrightnessMethods");
    ScopedBstr methodName(L"WmiSetBrightness");

    Microsoft::WRL::ComPtr<IWbemClassObject> pClass;
    if (FAILED(pSvc->GetObject(className, 0, NULL, &pClass, NULL)) || !pClass) return false;

    Microsoft::WRL::ComPtr<IWbemClassObject> pInParamsDef;
    if (FAILED(pClass->GetMethod(methodName, 0, &pInParamsDef, NULL)) || !pInParamsDef) return false;

    Microsoft::WRL::ComPtr<IWbemClassObject> pInInst;
    if (FAILED(pInParamsDef->SpawnInstance(0, &pInInst)) || !pInInst) return false;

    VARIANT vtTimeout;
    VariantInit(&vtTimeout);
    vtTimeout.vt = VT_I4;
    vtTimeout.lVal = 0;
    pInInst->Put(L"Timeout", 0, &vtTimeout, 0);
    VariantClear(&vtTimeout);

    VARIANT vtBrightness;
    VariantInit(&vtBrightness);
    vtBrightness.vt = VT_UI1;
    vtBrightness.bVal = static_cast<BYTE>(brightness);
    pInInst->Put(L"Brightness", 0, &vtBrightness, 0);
    VariantClear(&vtBrightness);

    std::wstring objPath = L"WmiMonitorBrightnessMethods.InstanceName=\"" + EscapeWmiPathString(instanceName) + L"\"";
    ScopedBstr bstrPath(objPath.c_str());

    HRESULT hr = pSvc->ExecMethod(bstrPath, methodName, 0, NULL, pInInst.Get(), NULL, NULL);
    return SUCCEEDED(hr);
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
                    info.isInternal = false;
                    ctx->monitors->push_back(info);
                }
            }
        }
    }
    return TRUE;
}

static constexpr int kNoPendingBrightness = -9999;

MonitorManager::MonitorManager()
    : m_cachedBrightness(45), m_syncBaseBrightness(45), m_driverAvailable(false), m_displayPoweredOn(true), m_consecutiveFailures(0), m_lastEnumTick(0),
      m_pendingTargetBrightness(kNoPendingBrightness), m_pendingReinit(false), m_stopWorker(false) {
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
        m_pendingTargetBrightness = kNoPendingBrightness;
        m_pendingReinit = false;
    }
    m_workerCv.notify_all();

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void MonitorManager::Cleanup() {
    m_fnSyncRunning = false;
    if (m_fnSyncStopEvent) {
        SetEvent(m_fnSyncStopEvent);
    }
    if (m_fnSyncThread.joinable()) {
        m_fnSyncThread.join();
    }
    if (m_fnSyncStopEvent) {
        CloseHandle(m_fnSyncStopEvent);
        m_fnSyncStopEvent = NULL;
    }

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

static int GetSimMonitorCount() {
    int count = (g_Settings.cliSimulateMonitorsOverride >= 0)
        ? g_Settings.cliSimulateMonitorsOverride
        : g_Settings.simulateMonitorsCount;
    return (std::clamp)(count, 0, 16);
}

void MonitorManager::EnumMonitorsInternal() {
    if (!m_rawMonitors.empty()) {
        DestroyPhysicalMonitors((DWORD)m_rawMonitors.size(), m_rawMonitors.data());
        m_rawMonitors.clear();
    }

    int simCount = GetSimMonitorCount();
    if (simCount > 0) {
        std::map<std::wstring, DWORD> prevSimValues;
        for (const auto& prev : m_monitors) {
            prevSimValues[prev.id] = prev.currentBrightness;
        }
        m_monitors.clear();

        for (int i = 0; i < simCount; ++i) {
            PhysicalMonitorInfo info;
            info.hPhysical = NULL;
            info.minBrightness = 0;
            info.maxBrightness = 100;
            if (i == 0) {
                info.id = L"SIM_INTERNAL_0";
                info.friendlyName = L"Simulated Laptop Display";
                info.description = L"Simulated Internal Panel";
                info.deviceName = L"\\\\.\\DISPLAY_SIM0";
                info.isPrimary = true;
                info.isInternal = true;
            } else {
                info.id = L"SIM_DISPLAY_" + std::to_wstring(i);
                info.friendlyName = L"Simulated Monitor " + std::to_wstring(i + 1);
                info.description = L"Simulated DDC/CI Display";
                info.deviceName = L"\\\\.\\DISPLAY_SIM" + std::to_wstring(i);
                info.isPrimary = false;
                info.isInternal = false;
            }
            auto prevIt = prevSimValues.find(info.id);
            if (prevIt != prevSimValues.end()) {
                info.currentBrightness = prevIt->second;
            } else if (const auto* cfg = g_Settings.FindMonitorConfig(info.id)) {
                info.currentBrightness = (DWORD)(std::clamp)(cfg->cachedBrightness, 0, 100);
            } else {
                info.currentBrightness = (i == 0) ? 60 : 50;
            }
            m_monitors.push_back(info);
        }
    } else {
        m_monitors.clear();
        DisplayConfigSummary dispCfg = QueryDisplayConfigInfo();
        EnumContext ctx;
        ctx.rawMonitors = &m_rawMonitors;
        ctx.monitors = &m_monitors;
        ctx.friendlyNames = dispCfg.friendlyNames;

        EnumDisplayMonitors(NULL, NULL, MonitorEnumCallback, reinterpret_cast<LPARAM>(&ctx));
        EnumInternalMonitorsWmi(m_monitors, dispCfg);
    }

    if (!m_monitors.empty()) {
        m_driverAvailable = true;

        // Ensure every detected monitor has a MonitorControlConfig entry
        bool mainConnected = false;
        for (const auto& mon : m_monitors) {
            g_Settings.GetOrCreateMonitorConfig(mon.id, mon.friendlyName);
            if (!g_Settings.mainMonitorId.empty() && _wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0) {
                mainConnected = true;
            }
        }
        if (!mainConnected) {
            for (const auto& mon : m_monitors) {
                if (mon.isPrimary) {
                    g_Settings.mainMonitorId = mon.id;
                    mainConnected = true;
                    break;
                }
            }
            if (!mainConnected) {
                g_Settings.mainMonitorId = m_monitors[0].id;
            }
        }

        // Prioritize Main Display for cached brightness
        bool foundMain = false;
        for (const auto& mon : m_monitors) {
            if (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0) {
                m_cachedBrightness = SafeCalcBrightnessPercent(mon.currentBrightness, mon.minBrightness, mon.maxBrightness);
                foundMain = true;
                break;
            }
        }
        if (!foundMain) {
            const auto& mon = m_monitors[0];
            m_cachedBrightness = SafeCalcBrightnessPercent(mon.currentBrightness, mon.minBrightness, mon.maxBrightness);
        }
        if (m_cachedBrightness < 0) m_cachedBrightness = 0;
        if (m_cachedBrightness > 100) m_cachedBrightness = 100;
        m_syncBaseBrightness = m_cachedBrightness;
        g_Settings.cachedBrightness = m_cachedBrightness;

        HWND hMain = FindWindowW(L"ClickBrightness_MessageWndClass", L"ClickBrightness_HiddenMsgWindow");
        if (!hMain) hMain = FindWindowW(L"ClickBrightness_MessageWndClass", NULL);
        if (hMain) {
            PostMessageW(hMain, WM_APP_BRIGHTNESS_UPDATED, m_cachedBrightness, 0);
        }
        m_consecutiveFailures = 0;
    } else {
        m_driverAvailable = false;
    }
    m_consecutiveFailures = 0;
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

    if (GetSimMonitorCount() > 0 && !m_monitors.empty()) {
        for (const auto& mon : m_monitors) {
            if (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0) {
                m_cachedBrightness = SafeCalcBrightnessPercent(mon.currentBrightness, mon.minBrightness, mon.maxBrightness);
                g_Settings.cachedBrightness = m_cachedBrightness;
                return m_cachedBrightness;
            }
        }
        const auto& mon = m_monitors[0];
        m_cachedBrightness = SafeCalcBrightnessPercent(mon.currentBrightness, mon.minBrightness, mon.maxBrightness);
        g_Settings.cachedBrightness = m_cachedBrightness;
        return m_cachedBrightness;
    }

    // Find and query primary monitor first if selected
    for (auto& mon : m_monitors) {
        if (mon.isPrimary && g_Settings.IsMonitorSelected(mon.id)) {
            DWORD minB = 0, curB = 0, maxB = 100;
            bool ok = mon.isInternal
                ? GetInternalBrightnessWmi(mon.wmiInstanceName, curB)
                : (GetMonitorBrightness(mon.hPhysical, &minB, &curB, &maxB) != FALSE);
            if (ok) {
                mon.currentBrightness = curB;
                if (!mon.isInternal) {
                    mon.minBrightness = minB;
                    mon.maxBrightness = maxB;
                }
                m_cachedBrightness = SafeCalcBrightnessPercent(curB, mon.minBrightness, mon.maxBrightness);
                g_Settings.cachedBrightness = m_cachedBrightness;
                return m_cachedBrightness;
            }
        }
    }

    // Query first selected monitor
    for (auto& mon : m_monitors) {
        if (g_Settings.IsMonitorSelected(mon.id)) {
            DWORD minB = 0, curB = 0, maxB = 100;
            bool ok = mon.isInternal
                ? GetInternalBrightnessWmi(mon.wmiInstanceName, curB)
                : (GetMonitorBrightness(mon.hPhysical, &minB, &curB, &maxB) != FALSE);
            if (ok) {
                mon.currentBrightness = curB;
                if (!mon.isInternal) {
                    mon.minBrightness = minB;
                    mon.maxBrightness = maxB;
                }
                m_cachedBrightness = SafeCalcBrightnessPercent(curB, mon.minBrightness, mon.maxBrightness);
                g_Settings.cachedBrightness = m_cachedBrightness;
                return m_cachedBrightness;
            }
        }
    }

    // Fallback to first monitor if no selected monitor responded
    if (!m_monitors.empty()) {
        auto& mon = m_monitors[0];
        DWORD minB = 0, curB = 0, maxB = 100;
        bool ok = mon.isInternal
            ? GetInternalBrightnessWmi(mon.wmiInstanceName, curB)
            : (GetMonitorBrightness(mon.hPhysical, &minB, &curB, &maxB) != FALSE);
        if (ok) {
            mon.currentBrightness = curB;
            if (!mon.isInternal) {
                mon.minBrightness = minB;
                mon.maxBrightness = maxB;
            }
            m_cachedBrightness = SafeCalcBrightnessPercent(curB, mon.minBrightness, mon.maxBrightness);
            g_Settings.cachedBrightness = m_cachedBrightness;
            return m_cachedBrightness;
        } else {
            m_driverAvailable = false;
        }
    }

    return m_cachedBrightness;
}

bool MonitorManager::SetBrightness(int brightness, bool async) {
    return SetMonitorBrightnessTarget(L"", brightness, async);
}

int MonitorManager::GetMonitorBrightnessById(const std::wstring& monitorId) {
    if (monitorId.empty() || _wcsicmp(monitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0) {
        return GetCurrentBrightness();
    }
    {
        std::lock_guard<std::mutex> wlock(m_workerMutex);
        if (m_pendingTargetBrightness != kNoPendingBrightness) {
            if (_wcsicmp(m_pendingTargetMonitorId.c_str(), monitorId.c_str()) == 0) {
                return (std::clamp)(m_pendingTargetBrightness, 0, 100);
            }
            if (m_pendingTargetMonitorId.empty()) {
                const auto* cfg = g_Settings.FindMonitorConfig(monitorId);
                if (!cfg || cfg->controlMode == 0) {
                    return cfg ? AppSettings::ComputeSyncedBrightness(m_pendingTargetBrightness, *cfg) : (std::clamp)(m_pendingTargetBrightness, 0, 100);
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& mon : m_monitors) {
        if (_wcsicmp(mon.id.c_str(), monitorId.c_str()) == 0) {
            if (GetSimMonitorCount() > 0) {
                int pct = SafeCalcBrightnessPercent(mon.currentBrightness, mon.minBrightness, mon.maxBrightness);
                auto* cfg = const_cast<MonitorControlConfig*>(g_Settings.FindMonitorConfig(mon.id));
                if (cfg) cfg->cachedBrightness = pct;
                return pct;
            }
            DWORD minB = 0, curB = 0, maxB = 100;
            bool ok = mon.isInternal
                ? GetInternalBrightnessWmi(mon.wmiInstanceName, curB)
                : (GetMonitorBrightness(mon.hPhysical, &minB, &curB, &maxB) != FALSE);
            if (ok) {
                mon.currentBrightness = curB;
                if (!mon.isInternal) {
                    mon.minBrightness = minB;
                    mon.maxBrightness = maxB;
                }
                int pct = SafeCalcBrightnessPercent(curB, mon.minBrightness, mon.maxBrightness);
                auto* cfg = const_cast<MonitorControlConfig*>(g_Settings.FindMonitorConfig(mon.id));
                if (cfg) cfg->cachedBrightness = pct;
                return pct;
            }
            const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
            return cfg ? cfg->cachedBrightness : m_cachedBrightness;
        }
    }
    return m_cachedBrightness;
}

void MonitorManager::GetTargetBounds(const std::wstring& targetMonitorId, int& outMin, int& outMax) const {
    outMin = 0;
    outMax = 100;

    bool isSyncTarget = targetMonitorId.empty() || _wcsicmp(targetMonitorId.c_str(), L"__SYNC__") == 0;
    if (!isSyncTarget && _wcsicmp(targetMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0) {
        const auto* mainCfg = g_Settings.FindMonitorConfig(g_Settings.mainMonitorId);
        if (!mainCfg || mainCfg->controlMode == 0) {
            isSyncTarget = true;
        }
    }
    if (!isSyncTarget) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    // Pass 1: expand range so overscroll-enabled monitors can reach 0% and 100%
    for (const auto& mon : m_monitors) {
        const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
        if (cfg && !cfg->enabled) continue;
        bool isMain = (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
        int mode = cfg ? cfg->controlMode : 0;
        if (isMain || mode != 0 || !cfg) continue;

        int off = cfg->offset;
        if (off > 0 && cfg->syncOverscrollMin) {
            if (-off < outMin) outMin = -off;
        }
        if (off < 0 && cfg->syncOverscrollMax) {
            if (100 - off > outMax) outMax = 100 - off;
        }
    }
    // Pass 2: clamp range for monitors with overscroll disabled
    for (const auto& mon : m_monitors) {
        const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
        if (cfg && !cfg->enabled) continue;
        bool isMain = (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
        int mode = cfg ? cfg->controlMode : 0;
        if (isMain || mode != 0 || !cfg) continue;

        int off = cfg->offset;
        if (!cfg->syncOverscrollMax) {
            if (100 - off < outMax) outMax = 100 - off;
        }
        if (!cfg->syncOverscrollMin) {
            if (-off > outMin) outMin = -off;
        }
    }
    if (outMin > outMax) {
        outMin = 0;
        outMax = 100;
    }
}

int MonitorManager::GetTargetControlBrightness(const std::wstring& targetMonitorId) {
    bool isSyncTarget = targetMonitorId.empty() || _wcsicmp(targetMonitorId.c_str(), L"__SYNC__") == 0;
    if (!isSyncTarget && _wcsicmp(targetMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0) {
        const auto* mainCfg = g_Settings.FindMonitorConfig(g_Settings.mainMonitorId);
        if (!mainCfg || mainCfg->controlMode == 0) {
            isSyncTarget = true;
        }
    }
    if (isSyncTarget) {
        int minB = 0, maxB = 100;
        GetTargetBounds(targetMonitorId, minB, maxB);
        std::lock_guard<std::mutex> lock(m_mutex);
        return (std::clamp)(m_syncBaseBrightness, minB, maxB);
    }
    return GetMonitorBrightnessById(targetMonitorId);
}

std::vector<BrightnessChangeEntry> MonitorManager::GetChangedMonitorsForTarget(const std::wstring& targetMonitorId, int targetBrightness) const {
    std::vector<BrightnessChangeEntry> result;

    std::lock_guard<std::mutex> lock(m_mutex);
    bool isSyncTarget = targetMonitorId.empty() || _wcsicmp(targetMonitorId.c_str(), L"__SYNC__") == 0;
    if (!isSyncTarget && _wcsicmp(targetMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0) {
        const auto* mainCfg = g_Settings.FindMonitorConfig(g_Settings.mainMonitorId);
        if (!mainCfg || mainCfg->controlMode == 0) {
            isSyncTarget = true;
        }
    }

    for (const auto& mon : m_monitors) {
        const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
        bool enabled = cfg ? cfg->enabled : true;
        if (!enabled) continue;

        bool isMain = (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
        int mode = cfg ? cfg->controlMode : 0;

        if (isSyncTarget) {
            if (!isMain && mode != 0) continue; // Skip Separate non-main monitors
            int effB = (!isMain && cfg) ? AppSettings::ComputeSyncedBrightness(targetBrightness, *cfg) : (std::clamp)(targetBrightness, 0, 100);
            const auto& effCfg = g_Settings.GetEffectiveMonitorConfig(mon.id);
            std::wstring dispName = (cfg && !cfg->customName.empty()) ? cfg->customName : AppSettings::SanitizeMonitorName(mon.friendlyName);
            if (dispName.empty()) dispName = mon.id;
            result.push_back({ mon.id, dispName, effB, effCfg.overlayUseWindowsAccent, effCfg.overlayAccentColor });
        } else {
            if (_wcsicmp(mon.id.c_str(), targetMonitorId.c_str()) != 0) continue;
            const auto& effCfg = g_Settings.GetEffectiveMonitorConfig(mon.id);
            std::wstring dispName = (cfg && !cfg->customName.empty()) ? cfg->customName : AppSettings::SanitizeMonitorName(mon.friendlyName);
            if (dispName.empty()) dispName = mon.id;
            result.push_back({ mon.id, dispName, targetBrightness, effCfg.overlayUseWindowsAccent, effCfg.overlayAccentColor });
        }
    }
    return result;
}

bool MonitorManager::SetMonitorBrightnessTarget(const std::wstring& targetMonitorId, int brightness, bool async) {
    int minB = 0, maxB = 100;
    GetTargetBounds(targetMonitorId, minB, maxB);
    brightness = (std::clamp)(brightness, minB, maxB);

    bool isSyncTarget = targetMonitorId.empty() || _wcsicmp(targetMonitorId.c_str(), L"__SYNC__") == 0;
    if (!isSyncTarget && _wcsicmp(targetMonitorId.c_str(), g_Settings.mainMonitorId.c_str()) == 0) {
        const auto* mainCfg = g_Settings.FindMonitorConfig(g_Settings.mainMonitorId);
        if (!mainCfg || mainCfg->controlMode == 0) {
            isSyncTarget = true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (isSyncTarget) {
            m_syncBaseBrightness = brightness;
            m_cachedBrightness = (std::clamp)(brightness, 0, 100);
            g_Settings.cachedBrightness = m_cachedBrightness;
            if (!g_Settings.mainMonitorId.empty()) {
                auto it = g_Settings.monitorConfigs.find(g_Settings.mainMonitorId);
                if (it != g_Settings.monitorConfigs.end()) it->second.cachedBrightness = m_cachedBrightness;
            }
        } else {
            auto it = g_Settings.monitorConfigs.find(targetMonitorId);
            if (it != g_Settings.monitorConfigs.end()) it->second.cachedBrightness = brightness;
        }
    }

    if (async) {
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            m_pendingTargetBrightness = brightness;
            m_pendingTargetMonitorId = isSyncTarget ? L"" : targetMonitorId;
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

    ScopedComInit comSync;
    Microsoft::WRL::ComPtr<IWbemServices> pWmiSync;

    bool allSuccess = true;
    bool isSim = (GetSimMonitorCount() > 0);
    for (auto& mon : m_monitors) {
        const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
        if (cfg && !cfg->enabled) continue;

        bool isMain = (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
        int mode = cfg ? cfg->controlMode : 0;
        int effB = brightness;

        if (isSyncTarget) {
            if (!isMain && mode != 0) continue;
            effB = (!isMain && cfg) ? AppSettings::ComputeSyncedBrightness(brightness, *cfg) : (std::clamp)(brightness, 0, 100);
        } else {
            if (_wcsicmp(mon.id.c_str(), targetMonitorId.c_str()) != 0) continue;
        }

        DWORD targetVal = (mon.maxBrightness > mon.minBrightness)
            ? (mon.minBrightness + (mon.maxBrightness - mon.minBrightness) * effB / 100)
            : (DWORD)effB;
        if (isSim) {
            mon.currentBrightness = targetVal;
        } else if (mon.isInternal) {
            m_lastAppInternalWriteTick = GetTickCount64();
            m_lastAppInternalWriteVal = (int)targetVal;
            if (!ConnectWmiServices(pWmiSync) || !SetInternalBrightnessWmi(pWmiSync.Get(), mon.wmiInstanceName, targetVal)) {
                allSuccess = false;
            }
        } else {
            if (!SetMonitorBrightness(mon.hPhysical, targetVal)) {
                allSuccess = false;
            }
        }
    }

    if (!allSuccess) {
        m_driverAvailable = false;
    }

    return allSuccess;
}

void MonitorManager::WorkerLoop() {
    ScopedComInit comWorker;
    Microsoft::WRL::ComPtr<IWbemServices> pWmiSvc;

    while (true) {
        int targetBrightness = kNoPendingBrightness;
        std::wstring targetMonitorId;
        bool doReinit = false;

        {
            std::unique_lock<std::mutex> lock(m_workerMutex);
            m_workerCv.wait(lock, [this]() {
                return m_stopWorker || m_pendingTargetBrightness != kNoPendingBrightness || m_pendingReinit;
            });

            if (m_stopWorker) {
                break;
            }

            if (m_pendingReinit) {
                m_pendingReinit = false;
                doReinit = true;
            }

            if (m_pendingTargetBrightness != kNoPendingBrightness) {
                targetBrightness = m_pendingTargetBrightness;
                targetMonitorId = m_pendingTargetMonitorId;
                m_pendingTargetBrightness = kNoPendingBrightness;
                m_pendingTargetMonitorId.clear();
            }
        }

        if (doReinit) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                EnumMonitorsInternal();
            }
            UpdateFnSyncListener();
        }

        if (targetBrightness != kNoPendingBrightness) {
            if (!m_displayPoweredOn.load()) {
                continue;
            }
            struct MonTarget {
                std::wstring id;
                HANDLE hPhysical;
                bool isInternal;
                std::wstring wmiInstanceName;
                DWORD targetVal;
            };
            std::vector<MonTarget> targets;
            bool isSim = (GetSimMonitorCount() > 0);

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_driverAvailable || m_monitors.empty()) {
                    EnumMonitorsInternal();
                }
                if (m_driverAvailable && !m_monitors.empty()) {
                    bool isSyncTarget = targetMonitorId.empty();
                    for (const auto& mon : m_monitors) {
                        const auto* cfg = g_Settings.FindMonitorConfig(mon.id);
                        if (cfg && !cfg->enabled) continue;

                        bool isMain = (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0);
                        int mode = cfg ? cfg->controlMode : 0;
                        int effB = targetBrightness;

                        if (isSyncTarget) {
                            if (!isMain && mode != 0) continue;
                            effB = (!isMain && cfg) ? AppSettings::ComputeSyncedBrightness(targetBrightness, *cfg) : (std::clamp)(targetBrightness, 0, 100);
                        } else {
                            if (_wcsicmp(mon.id.c_str(), targetMonitorId.c_str()) != 0) continue;
                        }

                        DWORD targetVal = (mon.maxBrightness > mon.minBrightness)
                            ? (mon.minBrightness + (mon.maxBrightness - mon.minBrightness) * effB / 100)
                            : (DWORD)effB;
                        targets.push_back({ mon.id, mon.hPhysical, mon.isInternal, mon.wmiInstanceName, targetVal });
                    }
                }
            }

            bool allSuccess = true;
            for (const auto& tgt : targets) {
                // Check if a newer target brightness arrived while writing or worker is stopping
                {
                    std::lock_guard<std::mutex> lock(m_workerMutex);
                    if (m_stopWorker || m_pendingTargetBrightness != kNoPendingBrightness || m_pendingReinit) {
                        // Coalesce: abort remaining old writes to immediately handle new target
                        break;
                    }
                }

                // Verify the monitor is still present (not destroyed by suspend/reinit)
                bool validHandle = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (const auto& mon : m_monitors) {
                        if (isSim ? (_wcsicmp(mon.id.c_str(), tgt.id.c_str()) == 0)
                                  : (tgt.isInternal ? (mon.isInternal && mon.wmiInstanceName == tgt.wmiInstanceName)
                                                    : (!mon.isInternal && mon.hPhysical == tgt.hPhysical))) {
                            validHandle = true;
                            break;
                        }
                    }
                }
                if (!validHandle) {
                    break;
                }

                bool ok = false;
                if (isSim) {
                    ok = true;
                } else if (tgt.isInternal) {
                    m_lastAppInternalWriteTick = GetTickCount64();
                    m_lastAppInternalWriteVal = (int)tgt.targetVal;
                    if (!ConnectWmiServices(pWmiSvc) || !SetInternalBrightnessWmi(pWmiSvc.Get(), tgt.wmiInstanceName, tgt.targetVal)) {
                        pWmiSvc.Reset();
                        if (ConnectWmiServices(pWmiSvc)) {
                            ok = SetInternalBrightnessWmi(pWmiSvc.Get(), tgt.wmiInstanceName, tgt.targetVal);
                        }
                    } else {
                        ok = true;
                    }
                } else {
                    ok = (SetMonitorBrightness(tgt.hPhysical, tgt.targetVal) != FALSE);
                    if (!ok) {
                        // Retry once after a brief micro-sleep in case of transient I2C bus contention
                        Sleep(25);
                        ok = (SetMonitorBrightness(tgt.hPhysical, tgt.targetVal) != FALSE);
                    }
                }

                if (ok) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (auto& mon : m_monitors) {
                        if (isSim ? (_wcsicmp(mon.id.c_str(), tgt.id.c_str()) == 0)
                                  : (tgt.isInternal ? (mon.isInternal && mon.wmiInstanceName == tgt.wmiInstanceName)
                                                    : (!mon.isInternal && mon.hPhysical == tgt.hPhysical))) {
                            mon.currentBrightness = tgt.targetVal;
                            break;
                        }
                    }
                } else {
                    allSuccess = false;
                }
            }

            if (!targets.empty()) {
                if (allSuccess) {
                    m_consecutiveFailures = 0;
                } else {
                    int fails = ++m_consecutiveFailures;
                    // Require 3 consecutive failed operations before declaring hardware/driver unavailable
                    if (fails >= 3) {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_driverAvailable = false;
                    }
                }
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
        m_pendingTargetBrightness = kNoPendingBrightness;
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

void MonitorManager::OnDisplayPowerOff() {
    m_displayPoweredOn = false;
    std::lock_guard<std::mutex> lock(m_workerMutex);
    m_pendingTargetBrightness = kNoPendingBrightness;
}

void MonitorManager::OnDisplayPowerOn() {
    if (!m_displayPoweredOn.load()) {
        m_displayPoweredOn = true;
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_pendingReinit = true;
        m_workerCv.notify_one();
    }
}

bool MonitorManager::IsMainDisplayInternal() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& mon : m_monitors) {
        if (_wcsicmp(mon.id.c_str(), g_Settings.mainMonitorId.c_str()) == 0) {
            return mon.isInternal;
        }
    }
    return false;
}

class WmiBrightnessEventSink : public IWbemObjectSink {
public:
    WmiBrightnessEventSink(MonitorManager* mgr) : m_refCount(1), m_mgr(mgr) {}
    virtual ~WmiBrightnessEventSink() = default;

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG l = InterlockedDecrement(&m_refCount);
        if (l == 0) delete this;
        return l;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IWbemObjectSink) {
            *ppv = static_cast<IWbemObjectSink*>(this);
            AddRef();
            return WBEM_S_NO_ERROR;
        }
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Indicate(LONG lObjectCount, IWbemClassObject** apObjArray) override {
        for (LONG i = 0; i < lObjectCount; ++i) {
            if (!apObjArray[i]) continue;
            DWORD b = 0;
            if (ReadWmiBrightnessVariant(apObjArray[i], L"Brightness", b)) {
                if (b > 100) b = 100;
                if (m_mgr) {
                    m_mgr->HandleWmiBrightnessEvent(static_cast<int>(b));
                }
            }
        }
        return WBEM_S_NO_ERROR;
    }
    HRESULT STDMETHODCALLTYPE SetStatus(LONG, HRESULT, BSTR, IWbemClassObject*) override {
        return WBEM_S_NO_ERROR;
    }

private:
    LONG m_refCount;
    MonitorManager* m_mgr;
};

void MonitorManager::HandleWmiBrightnessEvent(int brightness) {
    if (!g_Settings.syncExternalWithLaptopFnKeys || !IsMainDisplayInternal()) return;
    brightness = (std::clamp)(brightness, 0, 100);
    if (GetTickCount64() - m_lastAppInternalWriteTick.load() < 600 && brightness == m_lastAppInternalWriteVal.load()) {
        return; // Ignore echo from our own WMI write
    }
    SetMonitorBrightnessTarget(L"__SYNC__", brightness, true);
    HWND hMain = FindWindowW(L"ClickBrightness_MessageWndClass", L"ClickBrightness_HiddenMsgWindow");
    if (!hMain) hMain = FindWindowW(L"ClickBrightness_MessageWndClass", NULL);
    if (hMain) {
        PostMessageW(hMain, WM_APP_BRIGHTNESS_UPDATED, (WPARAM)brightness, 0);
    }
}

void MonitorManager::UpdateFnSyncListener() {
    bool shouldRun = (GetSimMonitorCount() == 0) && g_Settings.syncExternalWithLaptopFnKeys && IsMainDisplayInternal();
    if (!shouldRun) {
        if (m_fnSyncRunning.load()) {
            m_fnSyncRunning = false;
            if (m_fnSyncStopEvent) SetEvent(m_fnSyncStopEvent);
            if (m_fnSyncThread.joinable()) m_fnSyncThread.join();
            if (m_fnSyncStopEvent) { CloseHandle(m_fnSyncStopEvent); m_fnSyncStopEvent = NULL; }
        }
        return;
    }

    if (m_fnSyncRunning.load()) return;

    if (!m_fnSyncStopEvent) {
        m_fnSyncStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    } else {
        ResetEvent(m_fnSyncStopEvent);
    }
    m_fnSyncRunning = true;
    m_fnSyncThread = std::thread([this]() {
        ScopedComInit com;
        Microsoft::WRL::ComPtr<IWbemServices> pSvc;
        if (!ConnectWmiServices(pSvc)) {
            m_fnSyncRunning = false;
            return;
        }
        Microsoft::WRL::ComPtr<IWbemObjectSink> pSink;
        pSink.Attach(new WmiBrightnessEventSink(this));
        ScopedBstr wql(L"WQL");
        ScopedBstr query(L"SELECT * FROM WmiMonitorBrightnessEvent");
        if (SUCCEEDED(pSvc->ExecNotificationQueryAsync(wql, query, WBEM_FLAG_SEND_STATUS, NULL, pSink.Get()))) {
            if (m_fnSyncStopEvent) {
                WaitForSingleObject(m_fnSyncStopEvent, INFINITE);
            }
            pSvc->CancelAsyncCall(pSink.Get());
        }
        m_fnSyncRunning = false;
    });
}
