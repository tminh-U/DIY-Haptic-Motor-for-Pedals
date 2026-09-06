#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dbt.h>
#include <tlhelp32.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <timeapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <string_view>
#include "structed_file_AC.h"
#include "structed_file_ACC.h"
#include "settings_service.h"
#include "firmware_service.h"
#include "modern_ui.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")
#endif

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Build with: powershell -ExecutionPolicy Bypass -File build.ps1



// ==============================================================================
// 1. CORE TELEMETRY PIPELINE
// ==============================================================================
using namespace std;

const int AC_POLL_HZ = 240;
const int ACC_POLL_HZ = 60;
const int SERIAL_HZ = 60;
const auto AC_POLL_DURATION = chrono::microseconds(1000000 / AC_POLL_HZ);
const auto ACC_POLL_DURATION = chrono::microseconds(1000000 / ACC_POLL_HZ);
const auto SERIAL_DURATION = chrono::microseconds(1000000 / SERIAL_HZ);
const char* PYTHON_SHARED_MEMORY_NAME = "haptic_telemetry_v1";
const float DEFAULT_ABS_SLIP_RATIO = 0.10f;
const float DEFAULT_SUSPENSION_MAX_TRAVEL = 0.10f;
atomic<float> g_masterGain{0.78f};
atomic<float> g_absGain{1.0f};
atomic<float> g_roadGain{0.68f};
atomic<float> g_slipGain{0.82f};

enum class GameKind : int {
    None = 0,
    AC = 1,
    ACC = 2
};

GameKind detectRunningGame() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return GameKind::None;

    bool acRunning = false;
    bool accRunning = false;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const wchar_t* name = entry.szExeFile;
            if (_wcsicmp(name, L"AC2-Win64-Shipping.exe") == 0 || _wcsicmp(name, L"acc.exe") == 0) {
                accRunning = true;
            } else if (_wcsicmp(name, L"acs.exe") == 0 || _wcsicmp(name, L"acs_x86.exe") == 0) {
                acRunning = true;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    // ACC gets priority if both executables happen to overlap during startup.
    if (accRunning) return GameKind::ACC;
    if (acRunning) return GameKind::AC;
    return GameKind::None;
}

atomic<HANDLE> hSerial{INVALID_HANDLE_VALUE};
atomic<bool> g_pauseSerialWrites{false};
atomic<bool> g_connectionStopRequested{false};
atomic<ULONGLONG> g_lastDeviceHeartbeat{0};
mutex g_serialWriteMutex;
const char* HAPTIC_HANDSHAKE_REQUEST = "ID?\n";
const char* HAPTIC_HANDSHAKE_PREFIX = "HAPTIC_PEDAL,1,";

vector<string> getAvailableCOMPorts();

bool configureSerial(HANDLE serial) {
    DCB dcbSerialParams = {};
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(serial, &dcbSerialParams)) {
        return false;
    }
    dcbSerialParams.BaudRate = CBR_115200;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity   = NOPARITY;
    dcbSerialParams.fDtrControl = DTR_CONTROL_DISABLE; // Don't reset ESP32 on connect
    dcbSerialParams.fRtsControl = RTS_CONTROL_DISABLE;
    if (!SetCommState(serial, &dcbSerialParams)) {
        return false;
    }

    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = 10;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 5;
    if (!SetCommTimeouts(serial, &timeouts)) {
        return false;
    }
    return true;
}

bool readHapticIdentity(HANDLE serial, char* deviceId, size_t deviceIdSize) {
    DWORD bytesWritten = 0;
    if (!WriteFile(serial, HAPTIC_HANDSHAKE_REQUEST,
            static_cast<DWORD>(strlen(HAPTIC_HANDSHAKE_REQUEST)), &bytesWritten, NULL)
        || bytesWritten != strlen(HAPTIC_HANDSHAKE_REQUEST)) {
        return false;
    }

    char received[160] = {};
    size_t receivedLength = 0;
    DWORD deadline = GetTickCount() + 700;
    while (static_cast<LONG>(GetTickCount() - deadline) < 0) {
        char chunk[64];
        DWORD bytesRead = 0;
        if (ReadFile(serial, chunk, sizeof(chunk), &bytesRead, NULL) && bytesRead > 0) {
            size_t toCopy = min<size_t>(bytesRead, sizeof(received) - receivedLength - 1);
            memcpy(received + receivedLength, chunk, toCopy);
            receivedLength += toCopy;
            received[receivedLength] = '\0';

            const char* identity = strstr(received, HAPTIC_HANDSHAKE_PREFIX);
            if (identity) {
                identity += strlen(HAPTIC_HANDSHAKE_PREFIX);
                size_t idLength = strcspn(identity, "\r\n");
                if (idLength > 0 && idLength < deviceIdSize) {
                    memcpy(deviceId, identity, idLength);
                    deviceId[idLength] = '\0';
                    return true;
                }
                return false;
            }
        }
    }
    return false;
}

bool tryOpenHapticPort(const string& portName, HANDLE& outSerial, char* deviceId, size_t deviceIdSize) {
    string fullPort = "\\\\.\\" + portName;
    HANDLE serial = CreateFileA(fullPort.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (serial == INVALID_HANDLE_VALUE) return false;

    if (!configureSerial(serial)) {
        CloseHandle(serial);
        return false;
    }

    // Some USB-UART bridges can still reset the ESP32 while the port opens.
    // Wait for its Core 0 UART task before asking for the identity.
    for (int waitStep = 0; waitStep < 18; ++waitStep) {
        if (g_connectionStopRequested.load(memory_order_acquire)) {
            CloseHandle(serial);
            return false;
        }
        Sleep(100);
    }
    if (!PurgeComm(serial, PURGE_RXCLEAR | PURGE_TXCLEAR)) {
        CloseHandle(serial);
        return false;
    }

    if (!readHapticIdentity(serial, deviceId, deviceIdSize)) {
        CloseHandle(serial);
        return false;
    }
    if (g_connectionStopRequested.load(memory_order_acquire)) {
        CloseHandle(serial);
        return false;
    }
    outSerial = serial;
    return true;
}

bool initSerialAuto(char* portName, size_t portNameSize, char* deviceId, size_t deviceIdSize) {
    for (const string& candidate : getAvailableCOMPorts()) {
        HANDLE serial = INVALID_HANDLE_VALUE;
        char foundDeviceId[32] = {};
        if (!tryOpenHapticPort(candidate, serial, foundDeviceId, sizeof(foundDeviceId))) continue;

        strncpy_s(portName, portNameSize, candidate.c_str(), _TRUNCATE);
        strncpy_s(deviceId, deviceIdSize, foundDeviceId, _TRUNCATE);
        g_lastDeviceHeartbeat.store(GetTickCount64(), memory_order_release);
        hSerial.store(serial, memory_order_release);
        return true;
    }
    return false;
}

void closeSerial() {
    HANDLE serial = hSerial.exchange(INVALID_HANDLE_VALUE, memory_order_acq_rel);
    if (serial != INVALID_HANDLE_VALUE) {
        CloseHandle(serial);
    }
}

struct SMElement {
    HANDLE hMapFile;
    unsigned char* mapFileBuffer;
};

SMElement m_physics;
SMElement m_static;
SMElement m_pythonTelemetry;

// clean
void dismiss(SMElement& element) {
    if (element.mapFileBuffer) {
        UnmapViewOfFile(element.mapFileBuffer);
        element.mapFileBuffer = nullptr;
    }
    if (element.hMapFile) {
        CloseHandle(element.hMapFile);
        element.hMapFile = nullptr;
    }
}

// get data from shared memory (open only, do not create)
bool initPhysics(GameKind game) {
    if (m_physics.mapFileBuffer != nullptr) return true; // Already initialized

    m_physics.hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_physics");
    if (!m_physics.hMapFile) return false;
    
    size_t mapSize = (game == GameKind::ACC) ? sizeof(acc::SPageFilePhysics) : sizeof(SPageFilePhysics);
    m_physics.mapFileBuffer = (unsigned char*)MapViewOfFile(m_physics.hMapFile, FILE_MAP_READ, 0, 0, mapSize);
    if (!m_physics.mapFileBuffer) {
        CloseHandle(m_physics.hMapFile);
        m_physics.hMapFile = nullptr;
        return false;
    }
    return true;
}

bool initStatic(GameKind game) {
    if (m_static.mapFileBuffer != nullptr) return true;

    m_static.hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_static");
    if (!m_static.hMapFile) return false;

    size_t mapSize = (game == GameKind::ACC) ? sizeof(acc::SPageFileStatic) : sizeof(SPageFileStatic);
    m_static.mapFileBuffer = static_cast<unsigned char*>(MapViewOfFile(
        m_static.hMapFile, FILE_MAP_READ, 0, 0, mapSize));
    if (!m_static.mapFileBuffer) {
        CloseHandle(m_static.hMapFile);
        m_static.hMapFile = nullptr;
        return false;
    }
    return true;
}

bool initPythonTelemetry() {
    if (m_pythonTelemetry.mapFileBuffer != nullptr) return true;

    m_pythonTelemetry.hMapFile = OpenFileMappingA(
        FILE_MAP_READ,
        FALSE,
        PYTHON_SHARED_MEMORY_NAME
    );
    if (!m_pythonTelemetry.hMapFile) return false;

    m_pythonTelemetry.mapFileBuffer = static_cast<unsigned char*>(MapViewOfFile(
        m_pythonTelemetry.hMapFile,
        FILE_MAP_READ,
        0,
        0,
        44
    ));
    if (!m_pythonTelemetry.mapFileBuffer) {
        CloseHandle(m_pythonTelemetry.hMapFile);
        m_pythonTelemetry.hMapFile = nullptr;
        return false;
    }
    return true;
}

void sendDataToESP32(float absVal, float slipRatioL, float slipRatioR, float roadL, float roadR) {
    if (g_pauseSerialWrites.load(memory_order_acquire)) return;
    const float master = g_masterGain.load(memory_order_relaxed);
    absVal *= master * g_absGain.load(memory_order_relaxed);
    slipRatioL *= master * g_slipGain.load(memory_order_relaxed);
    slipRatioR *= master * g_slipGain.load(memory_order_relaxed);
    roadL *= master * g_roadGain.load(memory_order_relaxed);
    roadR *= master * g_roadGain.load(memory_order_relaxed);
    char buffer[64];
    int len = sprintf_s(buffer, "%.4f,%.4f,%.4f,%.4f,%.4f\n", absVal, slipRatioL, slipRatioR, roadL, roadR);
    lock_guard<mutex> writeLock(g_serialWriteMutex);
    HANDLE serial = hSerial.load(memory_order_acquire);
    if (serial == INVALID_HANDLE_VALUE) return;
    DWORD bytesWritten = 0;
    if (!WriteFile(serial, buffer, len, &bytesWritten, NULL)
        || bytesWritten != static_cast<DWORD>(len)) {
        g_connectionStopRequested.store(true, memory_order_release);
    }
}

float suspensionMaxTravel(GameKind game, int wheelIndex) {
    if (!m_static.mapFileBuffer || wheelIndex < 0 || wheelIndex >= 4) {
        return DEFAULT_SUSPENSION_MAX_TRAVEL;
    }

    float value = (game == GameKind::ACC)
        ? reinterpret_cast<acc::SPageFileStatic*>(m_static.mapFileBuffer)->suspensionMaxTravel[wheelIndex]
        : reinterpret_cast<SPageFileStatic*>(m_static.mapFileBuffer)->suspensionMaxTravel[wheelIndex];
    return (isfinite(value) && value >= 0.01f && value <= 1.0f)
        ? value
        : DEFAULT_SUSPENSION_MAX_TRAVEL;
}

float calculateRoadIntensity(float currentTravel, float previousTravel, float maxTravel, float deltaTimeSeconds) {
    if (!isfinite(currentTravel) || !isfinite(previousTravel)
        || !isfinite(maxTravel) || !isfinite(deltaTimeSeconds)
        || maxTravel <= 0.0f || deltaTimeSeconds <= 0.0f) {
        return 0.0f;
    }

    // Normalized suspension velocity.  A value of 1 means the wheel moved by
    // one full suspension travel per second; larger impacts are saturated.
    float normalizedRate = fabs(currentTravel - previousTravel)
        / (maxTravel * deltaTimeSeconds);
    return clamp(normalizedRate, 0.0f, 1.0f);
}

struct NormalizedTelemetry {
    uint32_t sequence = 0;
    float brake = 0.0f;
    float speedKmh = 0.0f;
    float slipRatioFL = 0.0f;
    float slipRatioFR = 0.0f;
    float ndSlipFL = 0.0f;
    float ndSlipFR = 0.0f;
    float suspensionFL = 0.0f;
    float suspensionFR = 0.0f;
    bool nativeAbsValid = false;
    bool nativeAbsActive = false;
};

bool validateTelemetry(const NormalizedTelemetry& parsed) {
    if (!isfinite(parsed.brake) || !isfinite(parsed.speedKmh)
        || !isfinite(parsed.slipRatioFL) || !isfinite(parsed.slipRatioFR)
        || !isfinite(parsed.ndSlipFL) || !isfinite(parsed.ndSlipFR)
        || !isfinite(parsed.suspensionFL) || !isfinite(parsed.suspensionFR)) {
        return false;
    }

    if (parsed.brake < -0.01f || parsed.brake > 1.01f
        || parsed.speedKmh < -5.0f || parsed.speedKmh > 1000.0f
        || fabs(parsed.slipRatioFL) > 10.0f || fabs(parsed.slipRatioFR) > 10.0f
        || fabs(parsed.ndSlipFL) > 100.0f || fabs(parsed.ndSlipFR) > 100.0f
        || fabs(parsed.suspensionFL) > 2.0f || fabs(parsed.suspensionFR) > 2.0f) {
        return false;
    }

    return true;
}

#pragma pack(push, 1)
struct PythonTelemetryShared {
    char magic[4];
    uint32_t sequenceStart;
    float brake;
    float speedKmh;
    float slipRatioFL;
    float slipRatioFR;
    float ndSlipFL;
    float ndSlipFR;
    float suspensionFL;
    float suspensionFR;
    uint32_t sequenceEnd;
};
#pragma pack(pop)

static_assert(sizeof(PythonTelemetryShared) == 44, "Unexpected Python telemetry layout");

bool readPythonTelemetry(NormalizedTelemetry& out) {
    if (!m_pythonTelemetry.mapFileBuffer) return false;

    volatile PythonTelemetryShared* shared =
        reinterpret_cast<volatile PythonTelemetryShared*>(m_pythonTelemetry.mapFileBuffer);

    if (shared->magic[0] != 'H' || shared->magic[1] != 'P'
        || shared->magic[2] != 'T' || shared->magic[3] != '1') {
        return false;
    }

    uint32_t sequenceBefore = shared->sequenceStart;
    MemoryBarrier();
    if ((sequenceBefore & 1U) != 0U || sequenceBefore == 0U) return false;

    NormalizedTelemetry parsed;
    parsed.sequence = sequenceBefore;
    parsed.brake = shared->brake;
    parsed.speedKmh = shared->speedKmh;
    parsed.slipRatioFL = shared->slipRatioFL;
    parsed.slipRatioFR = shared->slipRatioFR;
    parsed.ndSlipFL = shared->ndSlipFL;
    parsed.ndSlipFR = shared->ndSlipFR;
    parsed.suspensionFL = shared->suspensionFL;
    parsed.suspensionFR = shared->suspensionFR;

    MemoryBarrier();
    uint32_t sequenceEnd = shared->sequenceEnd;
    uint32_t sequenceAfter = shared->sequenceStart;
    if (sequenceBefore != sequenceEnd || sequenceBefore != sequenceAfter) return false;
    if (!validateTelemetry(parsed)) return false;

    out = parsed;
    return true;
}

bool readAccTelemetry(NormalizedTelemetry& out) {
    if (!m_physics.mapFileBuffer) return false;

    volatile acc::SPageFilePhysics* shared =
        reinterpret_cast<volatile acc::SPageFilePhysics*>(m_physics.mapFileBuffer);

    int packetBefore = shared->packetId;
    MemoryBarrier();

    NormalizedTelemetry parsed;
    parsed.sequence = static_cast<uint32_t>(packetBefore);
    parsed.brake = shared->brake;
    parsed.speedKmh = shared->speedKmh;
    parsed.slipRatioFL = shared->slipRatio[0];
    parsed.slipRatioFR = shared->slipRatio[1];
    parsed.suspensionFL = shared->suspensionTravel[0];
    parsed.suspensionFR = shared->suspensionTravel[1];
    // In ACC the earlier float `abs` is the live intervention signal. The
    // later `absInAction` member is an unused compatibility field and remains
    // zero even while the in-game brake indicator shows ABS intervention.
    float absSignal = shared->abs;

    MemoryBarrier();
    int packetAfter = shared->packetId;
    if (packetBefore != packetAfter || packetBefore <= 0) return false;
    if (!isfinite(absSignal) || absSignal < -0.01f || absSignal > 1.01f) return false;

    parsed.nativeAbsValid = true;
    parsed.nativeAbsActive = (absSignal > 0.5f);
    if (!validateTelemetry(parsed)) return false;

    out = parsed;
    return true;
}

// ==============================================================================
// 2. THREADING & GUI CONTROLLER
// ==============================================================================

#define IDC_COMBO_PORTS     1001

HINSTANCE hInst;
HWND hWndMain;
HWND hComboPorts;
bool g_manualFlashPortSelected = false;
HFONT hFontRegular, hFontBold;
HFONT hFontSmall, hFontPageTitle, hFontHero, hFontValue;
HICON g_appIcon = nullptr;
bool g_appIconOwned = false;
HANDLE g_singleInstanceMutex = nullptr;
vector<wstring> g_privateUiFontPaths;
bool g_googleSansFlexLoaded = false;

const wchar_t* regularUiFontFamily() {
    return g_googleSansFlexLoaded ? L"Google Sans Flex 18pt" : L"Segoe UI";
}

const wchar_t* mediumUiFontFamily() {
    return g_googleSansFlexLoaded ? L"Google Sans Flex 18pt Medium" : L"Segoe UI";
}

void loadPrivateUiFonts() {
    const wstring fontDirectory = firmwareServiceAppDirectory() + L"\\fonts\\";
    const wchar_t* fontFiles[] = {
        L"GoogleSansFlex-Regular.ttf",
        L"GoogleSansFlex-Medium.ttf"
    };

    for (const wchar_t* fontFile : fontFiles) {
        const wstring fontPath = fontDirectory + fontFile;
        if (AddFontResourceExW(fontPath.c_str(), FR_PRIVATE, nullptr) > 0) {
            g_privateUiFontPaths.push_back(fontPath);
        }
    }
    g_googleSansFlexLoaded = g_privateUiFontPaths.size() == 2;
}

void unloadPrivateUiFonts() {
    for (const wstring& fontPath : g_privateUiFontPaths) {
        RemoveFontResourceExW(fontPath.c_str(), FR_PRIVATE, nullptr);
    }
    g_privateUiFontPaths.clear();
    g_googleSansFlexLoaded = false;
}

bool privateUiFontSmokeTest() {
    loadPrivateUiFonts();
    if (!g_googleSansFlexLoaded) return false;

    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-21, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, regularUiFontFamily());
    wchar_t selectedFace[LF_FACESIZE] = {};
    HGDIOBJ previousFont = dc && font ? SelectObject(dc, font) : nullptr;
    const bool selected = dc && font
        && GetTextFaceW(dc, LF_FACESIZE, selectedFace) > 0
        && wcscmp(selectedFace, L"Google Sans Flex 18pt") == 0;
    if (previousFont) SelectObject(dc, previousFont);
    if (font) DeleteObject(font);
    if (dc) DeleteDC(dc);
    unloadPrivateUiFonts();
    return selected;
}

AppSettings g_settings;
int g_activePage = 0;
int g_dragSlider = -1;
std::wstring g_latestFirmware = L"Not checked";
std::wstring g_firmwareStatus = L"Ready";
std::string g_latestFirmwareUrl;
std::string g_latestFirmwareSketchUrl;
DWORD g_latestFirmwareOffset = 0;
bool g_latestSupportsBlankBoard = false;
mutex g_firmwareMutex;
atomic<bool> g_updateChecking{false};
atomic<bool> g_flashing{false};
atomic<int> g_firmwareStage{0};
atomic<int> g_downloadProgress{0};
atomic<bool> g_appClosing{false};
atomic<bool> g_powerSuspended{false};
atomic<ULONGLONG> g_reconnectNotBefore{0};
thread g_updateThread;
thread g_flashThread;
std::wstring g_pendingFirmwarePath;
std::wstring g_pendingFirmwarePort;
bool g_pendingFirmwareIsTemporary = false;
bool g_pendingFirmwareUsesArduinoCli = false;
DWORD g_pendingFirmwareOffset = 0;
NOTIFYICONDATAW g_trayIcon = {};
bool g_trayIconVisible = false;
const UINT WM_TRAYICON = WM_APP + 20;
constexpr int UI_DESIGN_WIDTH = 720;
constexpr int UI_DESIGN_HEIGHT = 900;
constexpr int UI_LOGICAL_WIDTH = 480;
constexpr int UI_LOGICAL_HEIGHT = 600;

atomic<bool> isRunning(false);
atomic<bool> isConnected(false);
atomic<int> g_detectedGameForUi{0};
thread workerThread;
thread serialMonitorThread;

// ESP32 panic detection via serial read
atomic<bool> g_esp32Panicked{false};
atomic<bool> g_panicMsgBoxShown{false};
mutex g_panicMutex;
char g_panicMessage[2048] = {0};

// Worker -> GUI thread communication via atomics (no cross-thread GUI calls)
// 0 = idle, 1 = connected, 2 = serial error, 3 = disconnected, 4 = no matching haptic device
atomic<int> g_serialStatus{0};
char g_serialPortName[32] = {0};
char g_hapticDeviceId[32] = {0};

// Live Telemetry Cache for GUI rendering
struct LiveData {
    atomic<float> brake{0.0f};
    atomic<float> absVal{0.0f}; // Canonical ABS state: 0.0 inactive, 1.0 active
    atomic<float> slipL{0.0f};
    atomic<float> slipR{0.0f};
    atomic<float> susL{0.0f};
    atomic<float> susR{0.0f};
    atomic<float> ndSlipL{0.0f};
    atomic<float> ndSlipR{0.0f};
    atomic<int>   gameKind{0}; // GameKind value
    atomic<int>   acState{0};  // 0 = closed, 1 = game found/waiting, 2 = telemetry active
    atomic<int>   fps{0};
} g_live;

// The high-rate simulator reader publishes here; the serial sender takes one
// coherent copy at 60 Hz. Keeping WriteFile off the reader thread prevents a
// slow USB-UART driver from reducing shared-memory capture frequency.
struct SerialTelemetryFrame {
    float absVal = 0.0f;
    float slipRatioL = 0.0f;
    float slipRatioR = 0.0f;
    float roadL = 0.0f;
    float roadR = 0.0f;
};

mutex g_serialFrameMutex;
SerialTelemetryFrame g_serialFrame;

void publishSerialFrame(float absVal, float slipRatioL, float slipRatioR,
                        float roadL, float roadR) {
    lock_guard<mutex> lock(g_serialFrameMutex);
    g_serialFrame = {absVal, slipRatioL, slipRatioR, roadL, roadR};
}

SerialTelemetryFrame latestSerialFrame() {
    lock_guard<mutex> lock(g_serialFrameMutex);
    return g_serialFrame;
}

void signalSerialDisconnect(int status = 2) {
    g_connectionStopRequested.store(true, memory_order_release);
    isConnected.store(false, memory_order_release);
    isRunning.store(false, memory_order_release);
    g_serialStatus.store(status, memory_order_release);
    g_reconnectNotBefore.store(GetTickCount64() + 500, memory_order_release);
}

bool deviceHeartbeatExpired(ULONGLONG now, ULONGLONG lastHeartbeat) {
    return lastHeartbeat != 0 && now - lastHeartbeat > 5000;
}

void serialSenderWorker() {
    auto nextSend = chrono::steady_clock::now();
    while (isRunning) {
        nextSend += SERIAL_DURATION;
        SerialTelemetryFrame frame = latestSerialFrame();
        sendDataToESP32(frame.absVal, frame.slipRatioL, frame.slipRatioR,
                        frame.roadL, frame.roadR);
        if (g_connectionStopRequested.load(memory_order_acquire)) {
            signalSerialDisconnect();
            break;
        }

        auto now = chrono::steady_clock::now();
        if (nextSend < now - SERIAL_DURATION) nextSend = now;
        this_thread::sleep_until(nextSend);
    }

    sendDataToESP32(0, 0, 0, 0, 0);
}

// Enumerate available COM ports from Windows Registry
vector<string> getAvailableCOMPorts() {
    vector<string> portList;
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char valueName[256];
        BYTE data[256];
        DWORD valLen, dataLen, type;
        DWORD index = 0;
        while (true) {
            valLen = sizeof(valueName);
            dataLen = sizeof(data);
            LONG res = RegEnumValueA(hKey, index, valueName, &valLen, NULL, &type, data, &dataLen);
            if (res != ERROR_SUCCESS) break;
            portList.push_back((char*)data);
            index++;
        }
        RegCloseKey(hKey);
    }
    if (portList.empty()) {
        for (int i = 1; i <= 16; i++) {
            string name = "COM" + to_string(i);
            string full = "\\\\.\\" + name;
            HANDLE h = CreateFileA(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                portList.push_back(name);
            }
        }
    }
    return portList;
}

// Serial Monitor Thread: reads lines from ESP32 and detects fatal panics
bool writeHeartbeat(HANDLE expectedSerial) {
    lock_guard<mutex> writeLock(g_serialWriteMutex);
    HANDLE serial = hSerial.load(memory_order_acquire);
    if (serial == INVALID_HANDLE_VALUE || serial != expectedSerial) return false;
    DWORD bytesWritten = 0;
    const DWORD requestLength = static_cast<DWORD>(strlen(HAPTIC_HANDSHAKE_REQUEST));
    return WriteFile(serial, HAPTIC_HANDSHAKE_REQUEST, requestLength,
                     &bytesWritten, nullptr)
        && bytesWritten == requestLength;
}

void serialMonitorWorker() {
    char readBuf[512];
    char lineBuf[512];
    int linePos = 0;
    bool isCapturing = false;
    int captureLines = 0;
    HANDLE monitoredSerial = INVALID_HANDLE_VALUE;
    ULONGLONG lastHeartbeatSent = 0;

    while (isRunning) {
        HANDLE serial = hSerial.load(memory_order_acquire);
        if (serial == INVALID_HANDLE_VALUE) {
            this_thread::sleep_for(chrono::milliseconds(100));
            continue;
        }

        const ULONGLONG now = GetTickCount64();
        if (serial != monitoredSerial) {
            monitoredSerial = serial;
            lastHeartbeatSent = now;
            g_lastDeviceHeartbeat.store(now, memory_order_release);
        } else if (now - lastHeartbeatSent >= 2000) {
            if (!writeHeartbeat(serial)) {
                signalSerialDisconnect();
                break;
            }
            lastHeartbeatSent = now;
        }

        DWORD bytesRead = 0;
        const BOOL readSucceeded = ReadFile(
            serial, readBuf, sizeof(readBuf) - 1, &bytesRead, NULL);
        if (!readSucceeded) {
            signalSerialDisconnect();
            break;
        }
        if (bytesRead > 0) {
            for (DWORD i = 0; i < bytesRead; i++) {
                char c = readBuf[i];
                if (c == '\n' || c == '\r') {
                    if (linePos > 0) {
                        lineBuf[linePos] = '\0';
                        if (strncmp(lineBuf, HAPTIC_HANDSHAKE_PREFIX,
                                    strlen(HAPTIC_HANDSHAKE_PREFIX)) == 0) {
                            const char* identity = lineBuf + strlen(HAPTIC_HANDSHAKE_PREFIX);
                            if (g_hapticDeviceId[0] != '\0'
                                && strcmp(identity, g_hapticDeviceId) != 0) {
                                signalSerialDisconnect();
                                linePos = 0;
                                break;
                            }
                            g_lastDeviceHeartbeat.store(GetTickCount64(), memory_order_release);
                            linePos = 0;
                            continue;
                        }
                        // Check for ESP32 fatal panic keywords
                        if (!isCapturing) {
                            if (strstr(lineBuf, "Guru Meditation") || strstr(lineBuf, "panic") ||
                                strstr(lineBuf, "abort()") || strstr(lineBuf, "LoadProhibited") ||
                                strstr(lineBuf, "StoreProhibited") || strstr(lineBuf, "InstrFetchProhibited")) {
                                lock_guard<mutex> lock(g_panicMutex);
                                strncpy(g_panicMessage, lineBuf, sizeof(g_panicMessage) - 1);
                                g_panicMessage[sizeof(g_panicMessage) - 1] = '\0';
                                g_pauseSerialWrites.store(true, memory_order_release);
                                g_esp32Panicked = true;
                                signalSerialDisconnect(5);
                                isCapturing = true;
                                captureLines = 0;
                            }
                        } else {
                            if (captureLines < 20) {
                                lock_guard<mutex> lock(g_panicMutex);
                                strncat(g_panicMessage, "\n", sizeof(g_panicMessage) - strlen(g_panicMessage) - 1);
                                strncat(g_panicMessage, lineBuf, sizeof(g_panicMessage) - strlen(g_panicMessage) - 1);
                                captureLines++;
                            }
                        }
                        linePos = 0;
                    }
                } else if (c >= 32 && c <= 126 && linePos < (int)sizeof(lineBuf) - 1) {
                    lineBuf[linePos++] = c;
                }
            }
        } else {
            // No data available, small sleep to avoid busy-wait
            this_thread::sleep_for(chrono::milliseconds(10));
        }


        if (deviceHeartbeatExpired(
                GetTickCount64(), g_lastDeviceHeartbeat.load(memory_order_acquire))) {
            signalSerialDisconnect();
            break;
        }
    }
}

void clearLiveTelemetry() {
    g_live.brake = 0.0f;
    g_live.absVal = 0.0f;
    g_live.slipL = 0.0f;
    g_live.slipR = 0.0f;
    g_live.susL = 0.0f;
    g_live.susR = 0.0f;
    g_live.ndSlipL = 0.0f;
    g_live.ndSlipR = 0.0f;
    publishSerialFrame(0, 0, 0, 0, 0);
}

// AC uses the in-game Python bridge for physical SlipRatio. ACC publishes
// SlipRatio and the live ABS signal directly in its shared-memory physics page.
// Both sources are normalized into the same five-field ESP32 packet.
void telemetryWorker() {
    if (!initSerialAuto(g_serialPortName, sizeof(g_serialPortName), g_hapticDeviceId, sizeof(g_hapticDeviceId))) {
        isConnected = false;
        isRunning = false;
        g_serialStatus = g_connectionStopRequested.load(memory_order_acquire) ? 3 : 4;
        return;
    }

    if (g_connectionStopRequested.load(memory_order_acquire) || !isRunning.load()) {
        isConnected = false;
        return;
    }

    isConnected = true;
    g_serialStatus = 1;

    // Shared-memory capture is latency-sensitive; serial output has its own
    // thread and remains at 60 Hz.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    publishSerialFrame(0, 0, 0, 0, 0);
    thread serialSender(serialSenderWorker);

    GameKind activeGame = GameKind::None;
    NormalizedTelemetry latest;
    auto lastPacketTime = chrono::steady_clock::now() - chrono::seconds(10);
    auto nextGameProbe = chrono::steady_clock::now();
    auto nextPhysicsRetry = chrono::steady_clock::now();
    auto nextStaticRetry = chrono::steady_clock::now();
    auto nextPythonRetry = chrono::steady_clock::now();
    auto nextPoll = chrono::steady_clock::now();
    auto lastFpsTime = chrono::steady_clock::now();
    int packetCount = 0;
    bool absLatched = false;
    bool suspensionBaselineValid = false;
    float previousSuspensionFL = 0.0f;
    float previousSuspensionFR = 0.0f;
    float roadIntensityFL = 0.0f;
    float roadIntensityFR = 0.0f;
    auto previousSuspensionTime = chrono::steady_clock::now();

    while (isRunning) {
        // AC's Python bridge is sampled quickly so no acUpdate publication is
        // missed. ACC retains the original 60 Hz shared-memory polling rate.
        const auto pollDuration = (activeGame == GameKind::AC)
            ? AC_POLL_DURATION
            : ACC_POLL_DURATION;
        nextPoll += pollDuration;
        auto now = chrono::steady_clock::now();

        if (now >= nextGameProbe) {
            GameKind detectedGame = detectRunningGame();
            if (detectedGame != activeGame) {
                dismiss(m_pythonTelemetry);
                dismiss(m_static);
                dismiss(m_physics);
                activeGame = detectedGame;
                latest = NormalizedTelemetry{};
                lastPacketTime = now - chrono::seconds(10);
                absLatched = false;
                suspensionBaselineValid = false;
                roadIntensityFL = 0.0f;
                roadIntensityFR = 0.0f;
                clearLiveTelemetry();
                g_live.gameKind = static_cast<int>(activeGame);
            }
            nextGameProbe = now + chrono::milliseconds(500);
        }

        if (activeGame != GameKind::None) {
            if (!m_physics.mapFileBuffer && now >= nextPhysicsRetry) {
                initPhysics(activeGame);
                nextPhysicsRetry = now + chrono::seconds(1);
            }
            if (!m_static.mapFileBuffer && now >= nextStaticRetry) {
                initStatic(activeGame);
                nextStaticRetry = now + chrono::seconds(1);
            }
            if (activeGame == GameKind::AC
                && !m_pythonTelemetry.mapFileBuffer && now >= nextPythonRetry) {
                initPythonTelemetry();
                nextPythonRetry = now + chrono::seconds(1);
            }
        }

        NormalizedTelemetry candidate;
        bool packetRead = (activeGame == GameKind::AC)
            ? readPythonTelemetry(candidate)
            : (activeGame == GameKind::ACC && readAccTelemetry(candidate));
        if (packetRead && candidate.sequence != latest.sequence) {
            if (suspensionBaselineValid) {
                float deltaTime = chrono::duration<float>(now - previousSuspensionTime).count();
                // Reject session changes and long stalls instead of turning them
                // into a false full-strength kerb hit.
                if (deltaTime >= 0.001f && deltaTime <= 0.10f) {
                    roadIntensityFL = calculateRoadIntensity(
                        candidate.suspensionFL,
                        previousSuspensionFL,
                        suspensionMaxTravel(activeGame, 0),
                        deltaTime
                    );
                    roadIntensityFR = calculateRoadIntensity(
                        candidate.suspensionFR,
                        previousSuspensionFR,
                        suspensionMaxTravel(activeGame, 1),
                        deltaTime
                    );
                } else {
                    roadIntensityFL = 0.0f;
                    roadIntensityFR = 0.0f;
                }
            } else {
                suspensionBaselineValid = true;
                roadIntensityFL = 0.0f;
                roadIntensityFR = 0.0f;
            }
            previousSuspensionFL = candidate.suspensionFL;
            previousSuspensionFR = candidate.suspensionFR;
            previousSuspensionTime = now;
            latest = candidate;
            lastPacketTime = now;
            ++packetCount;
        }

        bool telemetryFresh = activeGame != GameKind::None
            && chrono::duration_cast<chrono::milliseconds>(now - lastPacketTime).count() <= 250;

        if (!telemetryFresh) {
            g_live.acState = (activeGame != GameKind::None) ? 1 : 0;
            absLatched = false;
            suspensionBaselineValid = false;
            roadIntensityFL = 0.0f;
            roadIntensityFR = 0.0f;
            clearLiveTelemetry();
        } else {
            g_live.acState = 2;

            float slipL = min(fabs(latest.slipRatioFL), 2.0f);
            float slipR = min(fabs(latest.slipRatioFR), 2.0f);
            float maxFrontSlip = max(slipL, slipR);
            bool brakeGate = latest.brake > 0.05f && latest.speedKmh > 3.0f;

            if (activeGame == GameKind::ACC) {
                // ACC exposes the real intervention signal; no inferred ABS
                // threshold is needed for this game.
                absLatched = brakeGate && latest.nativeAbsValid && latest.nativeAbsActive;
            } else {
                SPageFilePhysics* pf = reinterpret_cast<SPageFilePhysics*>(m_physics.mapFileBuffer);
                float absSlipLimit = pf ? pf->abs : 0.0f;
                bool absEnabled = absSlipLimit > 0.001f;
                float absThreshold = (absSlipLimit >= 0.03f && absSlipLimit <= 0.30f)
                    ? absSlipLimit
                    : DEFAULT_ABS_SLIP_RATIO;

                if (!brakeGate || !absEnabled) {
                    absLatched = false;
                } else if (!absLatched && maxFrontSlip >= absThreshold) {
                    absLatched = true;
                } else if (absLatched && maxFrontSlip < absThreshold * 0.70f) {
                    absLatched = false;
                }
            }

            // A brake-pedal exciter should not react to lateral drift. Only the
            // longitudinal front-wheel ratios pass through while braking.
            float brakeSlipL = brakeGate ? slipL : 0.0f;
            float brakeSlipR = brakeGate ? slipR : 0.0f;
            float absVal = absLatched ? 1.0f : 0.0f;

            g_live.brake = latest.brake;
            g_live.absVal = absVal;
            g_live.slipL = brakeSlipL;
            g_live.slipR = brakeSlipR;
            g_live.ndSlipL = latest.ndSlipFL;
            g_live.ndSlipR = latest.ndSlipFR;
            g_live.susL = roadIntensityFL;
            g_live.susR = roadIntensityFR;

            publishSerialFrame(
                absVal,
                brakeSlipL,
                brakeSlipR,
                roadIntensityFL,
                roadIntensityFR
            );
        }

        if (chrono::duration_cast<chrono::milliseconds>(now - lastFpsTime).count() >= 1000) {
            g_live.fps = packetCount;
            packetCount = 0;
            lastFpsTime = now;
        }

        auto loopEnd = chrono::steady_clock::now();
        if (nextPoll < loopEnd - pollDuration) nextPoll = loopEnd;
        this_thread::sleep_until(nextPoll);
    }

    if (serialSender.joinable()) serialSender.join();
    dismiss(m_pythonTelemetry);
    dismiss(m_static);
    dismiss(m_physics);
    clearLiveTelemetry();
    isConnected = false;
    g_live.gameKind = 0;
    g_live.acState = 0;
    g_live.fps = 0;
    if (g_serialStatus.load() == 1) g_serialStatus = 3;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), count);
    result.pop_back();
    return result;
}

std::wstring deviceIdentity() {
    std::string raw(g_hapticDeviceId);
    const size_t comma = raw.find(',');
    if (comma != std::string::npos) raw.resize(comma);
    return utf8ToWide(raw);
}

std::wstring installedFirmwareVersion() {
    std::string raw(g_hapticDeviceId);
    const size_t comma = raw.find(',');
    if (comma == std::string::npos || comma + 1 >= raw.size()) return L"Unknown";
    return utf8ToWide(raw.substr(comma + 1));
}

void applyEffectSettings() {
    g_masterGain = g_settings.masterVolume / 100.0f;
    g_absGain = g_settings.absPulse / 100.0f;
    g_roadGain = g_settings.roadTexture / 100.0f;
    g_slipGain = g_settings.tireSlip / 100.0f;
}

void persistSettings() {
    applyEffectSettings();
    saveAppSettings(g_settings);
}

void refreshPortList() {
    if (!hComboPorts) return;
    char selected[32] = {};
    GetWindowTextA(hComboPorts, selected, sizeof(selected));
    SendMessage(hComboPorts, CB_RESETCONTENT, 0, 0);
    const auto ports = getAvailableCOMPorts();
    int selectedIndex = -1;
    for (size_t index = 0; index < ports.size(); ++index) {
        SendMessageA(hComboPorts, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ports[index].c_str()));
        if (_stricmp(selected, ports[index].c_str()) == 0) selectedIndex = static_cast<int>(index);
    }
    if (g_manualFlashPortSelected && selectedIndex >= 0) {
        SendMessage(hComboPorts, CB_SETCURSEL, selectedIndex, 0);
    } else {
        SendMessage(hComboPorts, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        g_manualFlashPortSelected = false;
    }
}

void updateFirmwareControls() {
    if (!hComboPorts) return;
    if (g_activePage == 2) refreshPortList();
}

void startConnection() {
    if (g_flashing.load() || isRunning.load() || g_appClosing.load()
        || g_powerSuspended.load()) return;
    if (workerThread.joinable()) workerThread.join();
    if (serialMonitorThread.joinable()) serialMonitorThread.join();
    closeSerial();
    g_serialPortName[0] = '\0';
    g_hapticDeviceId[0] = '\0';
    g_serialStatus = 0;
    g_esp32Panicked = false;
    g_panicMsgBoxShown = false;
    g_pauseSerialWrites = false;
    g_connectionStopRequested = false;
    g_lastDeviceHeartbeat = 0;
    {
        lock_guard<mutex> lock(g_panicMutex);
        g_panicMessage[0] = '\0';
    }
    isRunning = true;
    workerThread = thread(telemetryWorker);
    serialMonitorThread = thread(serialMonitorWorker);
}

void stopConnection() {
    g_connectionStopRequested = true;
    isRunning = false;
    if (workerThread.joinable()) workerThread.join();
    if (serialMonitorThread.joinable()) serialMonitorThread.join();
    closeSerial();
    isConnected = false;
}

HICON createWaveLogoIcon() {
    constexpr int iconSize = 64;
    BITMAPV5HEADER bitmapInfo = {};
    bitmapInfo.bV5Size = sizeof(bitmapInfo);
    bitmapInfo.bV5Width = iconSize;
    bitmapInfo.bV5Height = -iconSize;
    bitmapInfo.bV5Planes = 1;
    bitmapInfo.bV5BitCount = 32;
    bitmapInfo.bV5Compression = BI_RGB;

    void* pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bitmapInfo),
        DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !pixels) {
        if (color) DeleteObject(color);
        return nullptr;
    }

    HDC iconDc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldBitmap = SelectObject(iconDc, color);
    RECT area = {0, 0, iconSize, iconSize};
    HBRUSH background = CreateSolidBrush(RGB(8, 25, 38));
    FillRect(iconDc, &area, background);
    DeleteObject(background);

    const int heights[] = {12, 34, 50, 27, 14};
    HPEN wave = CreatePen(PS_SOLID, 5, RGB(24, 190, 246));
    HGDIOBJ oldPen = SelectObject(iconDc, wave);
    SetBkMode(iconDc, TRANSPARENT);
    for (int index = 0; index < 5; ++index) {
        const int x = 12 + index * 10;
        MoveToEx(iconDc, x, 32 - heights[index] / 2, nullptr);
        LineTo(iconDc, x, 32 + heights[index] / 2);
    }
    SelectObject(iconDc, oldPen);
    DeleteObject(wave);
    SelectObject(iconDc, oldBitmap);
    DeleteDC(iconDc);

    BYTE maskBits[(iconSize * iconSize) / 8] = {0};
    HBITMAP mask = CreateBitmap(iconSize, iconSize, 1, 1, maskBits);
    ICONINFO info = {};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON result = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return result;
}

void addTrayIcon(HWND window) {
    if (g_trayIconVisible) return;
    g_trayIcon = {};
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = window;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    g_trayIcon.hIcon = g_appIcon ? g_appIcon : LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_trayIcon.szTip, L"Haptic Brake Control");
    g_trayIconVisible = Shell_NotifyIconW(NIM_ADD, &g_trayIcon) != FALSE;
}

void removeTrayIcon() {
    if (!g_trayIconVisible) return;
    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    g_trayIconVisible = false;
}

modern_ui::DrawModel currentDrawModel() {
    modern_ui::DrawModel model;
    model.page = g_activePage;
    model.settings = g_settings;
    model.connected = isConnected.load();
    model.panicked = g_esp32Panicked.load();
    model.checkingUpdate = g_updateChecking.load();
    model.flashing = g_flashing.load();
    const int detectedGame = g_detectedGameForUi.load();
    const int telemetryGame = g_live.gameKind.load();
    const int telemetryState = g_live.acState.load();
    model.gameDetected = detectedGame != static_cast<int>(GameKind::None);
    model.telemetryActive = model.gameDetected && model.connected
        && telemetryGame == detectedGame && telemetryState == 2;
    if (detectedGame == static_cast<int>(GameKind::AC)) {
        model.gameName = L"Assetto Corsa";
    } else if (detectedGame == static_cast<int>(GameKind::ACC)) {
        model.gameName = L"Assetto Corsa Competizione";
    }
    if (model.telemetryActive) {
        model.gameStatus = L"Telemetry active";
    } else if (model.gameDetected && !model.connected) {
        model.gameStatus = L"Controller not connected";
    } else if (model.gameDetected) {
        model.gameStatus = L"Waiting for telemetry";
    }
    model.firmwareStage = g_firmwareStage.load();
    model.downloadProgress = g_downloadProgress.load();
    const int portCount = hComboPorts
        ? static_cast<int>(SendMessage(hComboPorts, CB_GETCOUNT, 0, 0)) : 0;
    model.portSelected = g_manualFlashPortSelected && hComboPorts
        && SendMessage(hComboPorts, CB_GETCURSEL, 0, 0) != CB_ERR;
    if (model.portSelected) {
        wchar_t port[32] = {};
        GetWindowTextW(hComboPorts, port, 32);
        model.selectedPort = port;
    } else if (portCount > 0) {
        model.selectedPort = L"Select a COM port";
    }
    model.deviceId = deviceIdentity();
    model.currentFirmware = model.connected ? installedFirmwareVersion() : L"Not detected";
    {
        lock_guard<mutex> lock(g_firmwareMutex);
        model.latestFirmware = g_latestFirmware;
        model.firmwareStatus = g_firmwareStatus;
        model.binaryAvailable = !g_latestFirmwareUrl.empty()
            || !g_latestFirmwareSketchUrl.empty();
        model.manualBinaryAvailable = (!g_latestFirmwareUrl.empty()
            && g_latestSupportsBlankBoard) || !g_latestFirmwareSketchUrl.empty();
    }
    return model;
}

void beginUpdateCheck(HWND window) {
    if (g_updateChecking.exchange(true)) return;
    if (g_updateThread.joinable()) g_updateThread.join();
    {
        lock_guard<mutex> lock(g_firmwareMutex);
        g_firmwareStatus = L"Contacting GitHub Releases...";
    }
    InvalidateRect(window, nullptr, FALSE);
    g_updateThread = thread([window] {
        FirmwareRelease release = fetchLatestFirmwareRelease();
        {
            lock_guard<mutex> lock(g_firmwareMutex);
            if (release.ok) {
                g_latestFirmware = utf8ToWide(release.version);
                g_latestFirmwareUrl = release.binaryUrl;
                g_latestFirmwareSketchUrl = release.sketchUrl;
                g_latestFirmwareOffset = release.flashOffset;
                g_latestSupportsBlankBoard = release.supportsBlankBoard;
                std::wstring installed = installedFirmwareVersion();
                if (release.binaryUrl.empty() && release.sketchUrl.empty()) {
                    g_firmwareStatus = L"Latest release has no .bin or .ino firmware asset.";
                } else if (release.binaryUrl.empty()) {
                    g_firmwareStatus = L"Arduino source found; Arduino CLI will compile it before flashing.";
                } else if (!release.supportsBlankBoard) {
                    g_firmwareStatus = L"Update image found; blank-board flashing needs a merged .bin asset.";
                } else {
                    g_firmwareStatus = installed == L"Unknown" || installed == L"Not detected"
                        ? L"Latest firmware is ready to download and flash."
                        : (installed == g_latestFirmware ? L"Your device is up to date."
                                                         : L"A firmware update is available.");
                }
            } else {
                g_latestFirmware = L"Unavailable";
                g_firmwareStatus = L"Update check failed: " + utf8ToWide(release.error);
            }
        }
        g_updateChecking = false;
        if (!g_appClosing.load()) PostMessage(window, WM_APP + 21, 0, 0);
    });
}

void showPortSelector(HWND window) {
    if (g_flashing.load()) return;
    refreshPortList();
    const int count = static_cast<int>(SendMessage(hComboPorts, CB_GETCOUNT, 0, 0));
    HMENU menu = CreatePopupMenu();
    if (count <= 0) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No COM ports found");
    } else {
        for (int index = 0; index < count; ++index) {
            wchar_t port[32] = {};
            SendMessageW(hComboPorts, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(port));
            AppendMenuW(menu, MF_STRING, 9100 + index, port);
        }
    }
    RECT client = {};
    GetClientRect(window, &client);
    POINT position{
        MulDiv(55, client.right, UI_DESIGN_WIDTH),
        MulDiv(637, client.bottom, UI_DESIGN_HEIGHT)
    };
    ClientToScreen(window, &position);
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                   position.x, position.y, 0, window, nullptr);
    DestroyMenu(menu);
}

void launchEsptoolFlash(HWND window, const std::wstring& selectedPort,
                        const std::wstring& selectedFile, DWORD flashOffset,
                        bool deleteAfterFlash) {
    stopConnection();
    g_firmwareStage = 2;
    updateFirmwareControls();
    if (g_flashThread.joinable()) g_flashThread.join();
    {
        lock_guard<mutex> lock(g_firmwareMutex);
        g_firmwareStatus = L"Flashing firmware on " + selectedPort + L"...";
    }
    g_flashThread = thread([
        window, selectedPort, selectedFile, flashOffset, deleteAfterFlash] {
        DWORD exitCode = 0;
        std::string error;
        bool success = flashFirmwareWithEsptool(
            selectedPort, selectedFile, flashOffset, exitCode, error);
        if (deleteAfterFlash) DeleteFileW(selectedFile.c_str());
        {
            lock_guard<mutex> lock(g_firmwareMutex);
            g_firmwareStatus = success ? L"Firmware flash completed. Reconnecting..."
                                       : L"Firmware flash failed: " + utf8ToWide(error);
        }
        g_firmwareStage = 0;
        g_flashing = false;
        if (!g_appClosing.load()) PostMessage(window, WM_APP + 22, 1, success ? 1 : 0);
    });
}

void launchArduinoCliFlash(HWND window, const std::wstring& selectedPort,
                           const std::wstring& sketchPath,
                           bool deleteAfterFlash) {
    stopConnection();
    g_firmwareStage = 3;
    updateFirmwareControls();
    if (g_flashThread.joinable()) g_flashThread.join();
    {
        lock_guard<mutex> lock(g_firmwareMutex);
        g_firmwareStatus = L"Checking Arduino CLI and the ESP32 board package...";
    }
    g_flashThread = thread([window, selectedPort, sketchPath, deleteAfterFlash] {
        DWORD exitCode = 0;
        std::string error;
        const std::wstring cli = findArduinoCli();
        bool success = !cli.empty();
        if (!success) {
            error = "Arduino CLI was not found. Install it or put arduino-cli.exe next to the app.";
        } else {
            success = ensureEsp32ArduinoCore(cli, error);
        }
        if (success) {
            g_firmwareStage = 4;
            {
                lock_guard<mutex> lock(g_firmwareMutex);
                g_firmwareStatus = L"Compiling the .ino and uploading it to "
                    + selectedPort + L"...";
            }
            if (!g_appClosing.load()) PostMessage(window, WM_APP + 21, 0, 0);
            success = compileAndUploadSketchWithArduinoCli(
                cli, selectedPort, sketchPath, exitCode, error);
        }
        if (deleteAfterFlash) {
            DeleteFileW(sketchPath.c_str());
            const size_t slash = sketchPath.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                RemoveDirectoryW(sketchPath.substr(0, slash).c_str());
            }
        }
        {
            lock_guard<mutex> lock(g_firmwareMutex);
            g_firmwareStatus = success
                ? L"Firmware compiled and flashed. Reconnecting..."
                : L"Arduino CLI flash failed: " + utf8ToWide(error);
        }
        g_firmwareStage = 0;
        g_flashing = false;
        if (!g_appClosing.load()) PostMessage(window, WM_APP + 22, 1, success ? 1 : 0);
    });
}

std::wstring downloadedFirmwarePath() {
    wchar_t temporaryDirectory[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, temporaryDirectory)) return L"haptic_latest_firmware.bin";
    return std::wstring(temporaryDirectory) + L"haptic_latest_firmware.bin";
}

std::wstring downloadedSketchPath() {
    wchar_t temporaryDirectory[MAX_PATH] = {};
    std::wstring directory = GetTempPathW(MAX_PATH, temporaryDirectory)
        ? std::wstring(temporaryDirectory) + L"HapticBrakeRelease"
        : L"HapticBrakeRelease";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\HapticBrakeRelease.ino";
}

void beginReleaseFlash(HWND window, bool updateConnectedDevice) {
    if (g_updateChecking.load() || g_flashing.exchange(true)) return;

    std::wstring selectedPort;
    if (updateConnectedDevice) {
        if (!isConnected.load() || g_serialPortName[0] == '\0') {
            g_flashing = false;
            MessageBoxW(window, L"Connect a recognized haptic controller first.",
                        L"Firmware Update", MB_ICONWARNING);
            return;
        }
        selectedPort = utf8ToWide(g_serialPortName);
    } else {
        wchar_t port[32] = {};
        GetWindowTextW(hComboPorts, port, 32);
        if (!g_manualFlashPortSelected || port[0] == L'\0') {
            g_flashing = false;
            MessageBoxW(window, L"Select an available COM port first.",
                        L"Manual Flash", MB_ICONWARNING);
            return;
        }
        selectedPort = port;
    }

    {
        lock_guard<mutex> lock(g_firmwareMutex);
        const bool metadataAlreadyLoaded = g_latestFirmware != L"Not checked";
        const bool hasSketch = !g_latestFirmwareSketchUrl.empty();
        const bool releaseCannotFlash = (g_latestFirmwareUrl.empty() && !hasSketch)
            || (!updateConnectedDevice && !g_latestSupportsBlankBoard && !hasSketch);
        if (metadataAlreadyLoaded && releaseCannotFlash) {
            g_firmwareStatus = g_latestFirmwareUrl.empty() && !hasSketch
                ? L"Latest release has no .bin or .ino firmware asset."
                : L"Blank-board flashing needs a full .bin or an .ino release asset.";
            g_flashing = false;
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
    }

    if (g_flashThread.joinable()) g_flashThread.join();
    g_firmwareStage = 1;
    g_downloadProgress = 0;
    {
        lock_guard<mutex> lock(g_firmwareMutex);
        g_firmwareStatus = L"Loading the latest firmware release...";
    }
    g_flashThread = thread([window, selectedPort, updateConnectedDevice] {
        std::string binaryUrl;
        std::string sketchUrl;
        DWORD flashOffset = 0;
        bool supportsBlankBoard = false;
        {
            lock_guard<mutex> lock(g_firmwareMutex);
            binaryUrl = g_latestFirmwareUrl;
            sketchUrl = g_latestFirmwareSketchUrl;
            flashOffset = g_latestFirmwareOffset;
            supportsBlankBoard = g_latestSupportsBlankBoard;
        }
        if (binaryUrl.empty() && sketchUrl.empty()) {
            FirmwareRelease release = fetchLatestFirmwareRelease();
            lock_guard<mutex> lock(g_firmwareMutex);
            if (release.ok) {
                g_latestFirmware = utf8ToWide(release.version);
                g_latestFirmwareUrl = release.binaryUrl;
                g_latestFirmwareSketchUrl = release.sketchUrl;
                g_latestFirmwareOffset = release.flashOffset;
                g_latestSupportsBlankBoard = release.supportsBlankBoard;
                binaryUrl = release.binaryUrl;
                sketchUrl = release.sketchUrl;
                flashOffset = release.flashOffset;
                supportsBlankBoard = release.supportsBlankBoard;
            } else {
                g_firmwareStatus = L"Update check failed: " + utf8ToWide(release.error);
            }
        }

        const bool useSketch = binaryUrl.empty()
            || (!updateConnectedDevice && !supportsBlankBoard);
        std::string error;
        const std::wstring destination = useSketch
            ? downloadedSketchPath() : downloadedFirmwarePath();
        if (useSketch && sketchUrl.empty()) {
            binaryUrl.clear();
            error = "Blank-board flashing requires a full .bin or an .ino asset.";
        }
        const std::string selectedUrl = useSketch ? sketchUrl : binaryUrl;
        bool downloaded = !selectedUrl.empty()
            && downloadFirmwareBinary(selectedUrl, destination, &g_downloadProgress, error);
        if (!downloaded) {
            lock_guard<mutex> lock(g_firmwareMutex);
            if (selectedUrl.empty()) {
                g_firmwareStatus = L"Latest release has no usable .bin or .ino firmware asset.";
            } else {
                g_firmwareStatus = L"Firmware download failed: " + utf8ToWide(error);
            }
            g_firmwareStage = 0;
            g_flashing = false;
            if (!g_appClosing.load()) PostMessage(window, WM_APP + 21, 0, 0);
            return;
        }

        if (g_appClosing.load()) {
            DeleteFileW(destination.c_str());
            g_firmwareStage = 0;
            g_flashing = false;
            return;
        }
        {
            lock_guard<mutex> lock(g_firmwareMutex);
            g_pendingFirmwarePath = destination;
            g_pendingFirmwarePort = selectedPort;
            g_pendingFirmwareIsTemporary = true;
            g_pendingFirmwareUsesArduinoCli = useSketch;
            g_pendingFirmwareOffset = flashOffset;
            g_firmwareStatus = useSketch
                ? L"Arduino source downloaded. Preparing Arduino CLI..."
                : L"Firmware downloaded. Preparing the ESP32...";
        }
        PostMessage(window, WM_APP + 23, 0, 0);
    });
}

int sliderValueFromX(int x) {
    return clamp((x - 58) * 100 / (657 - 58), 0, 100);
}

void setSliderValue(int slider, int x) {
    const int value = sliderValueFromX(x);
    if (slider == 0) g_settings.masterVolume = value;
    else if (slider == 1) g_settings.absPulse = value;
    else if (slider == 2) g_settings.roadTexture = value;
    else if (slider == 3) g_settings.tireSlip = value;
    applyEffectSettings();
}

void enablePerMonitorDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using SetDpiContext = BOOL(WINAPI*)(HANDLE);
    SetDpiContext setContext = nullptr;
    FARPROC setContextAddress = GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    static_assert(sizeof(setContext) == sizeof(setContextAddress));
    memcpy(&setContext, &setContextAddress, sizeof(setContext));
    if (!setContext || !setContext(reinterpret_cast<HANDLE>(-4))) {
        SetProcessDPIAware();
    }
}

UINT dpiForWindowOrSystem(HWND window = nullptr) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using GetWindowDpi = UINT(WINAPI*)(HWND);
    GetWindowDpi getWindowDpi = nullptr;
    FARPROC getDpiAddress = GetProcAddress(user32, "GetDpiForWindow");
    static_assert(sizeof(getWindowDpi) == sizeof(getDpiAddress));
    memcpy(&getWindowDpi, &getDpiAddress, sizeof(getWindowDpi));
    if (window && getWindowDpi) return getWindowDpi(window);

    HDC screen = GetDC(nullptr);
    const UINT dpi = screen ? static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX)) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    return dpi > 0 ? dpi : 96;
}

SIZE scaledWindowSize(UINT dpi, const RECT& workArea) {
    SIZE result{
        MulDiv(UI_LOGICAL_WIDTH, static_cast<int>(dpi), 96),
        MulDiv(UI_LOGICAL_HEIGHT, static_cast<int>(dpi), 96)
    };
    const int maximumWidth = (workArea.right - workArea.left) * 9 / 10;
    const int maximumHeight = (workArea.bottom - workArea.top) * 9 / 10;
    if (result.cx > maximumWidth || result.cy > maximumHeight) {
        const double fit = min(static_cast<double>(maximumWidth) / result.cx,
                               static_cast<double>(maximumHeight) / result.cy);
        result.cx = max(1, static_cast<int>(result.cx * fit));
        result.cy = max(1, static_cast<int>(result.cy * fit));
    }
    return result;
}

POINT clientToDesign(HWND window, int x, int y) {
    RECT client = {};
    GetClientRect(window, &client);
    return POINT{
        client.right > 0 ? MulDiv(x, UI_DESIGN_WIDTH, client.right) : x,
        client.bottom > 0 ? MulDiv(y, UI_DESIGN_HEIGHT, client.bottom) : y
    };
}

// Window Procedure

LRESULT CALLBACK ModernWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES};
            InitCommonControlsEx(&controls);
            BOOL darkMode = TRUE;
            DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                  &darkMode, sizeof(darkMode));

            hFontRegular = CreateFontW(-21, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, regularUiFontFamily());
            hFontBold = CreateFontW(-21, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, mediumUiFontFamily());
            hFontSmall = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, regularUiFontFamily());
            hFontPageTitle = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, mediumUiFontFamily());
            hFontHero = CreateFontW(-32, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, mediumUiFontFamily());
            hFontValue = CreateFontW(-23, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, mediumUiFontFamily());

            g_settings = loadAppSettings();
            applyEffectSettings();
            hComboPorts = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | CBS_DROPDOWNLIST,
                -1000, -1000, 10, 10, window, reinterpret_cast<HMENU>(IDC_COMBO_PORTS),
                hInst, nullptr);
            SendMessage(hComboPorts, WM_SETFONT, reinterpret_cast<WPARAM>(hFontRegular), TRUE);
            SetWindowTheme(hComboPorts, L"DarkMode_CFD", nullptr);
            updateFirmwareControls();
            addTrayIcon(window);
            SetTimer(window, 1, 33, nullptr);
            startConnection();
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = BeginPaint(window, &paint);
            RECT client = {};
            GetClientRect(window, &client);
            const int clientWidth = max(1L, client.right - client.left);
            const int clientHeight = max(1L, client.bottom - client.top);
            HDC buffer = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, clientWidth, clientHeight);
            HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
            SetMapMode(buffer, MM_ANISOTROPIC);
            SetWindowExtEx(buffer, UI_DESIGN_WIDTH, UI_DESIGN_HEIGHT, nullptr);
            SetViewportExtEx(buffer, clientWidth, clientHeight, nullptr);
            modern_ui::Fonts fonts{hFontRegular, hFontBold, hFontSmall,
                                   hFontPageTitle, hFontHero, hFontValue};
            modern_ui::draw(buffer, currentDrawModel(), fonts);
            SetMapMode(buffer, MM_TEXT);
            BitBlt(dc, 0, 0, clientWidth, clientHeight, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(buffer);
            EndPaint(window, &paint);
            return 0;
        }

        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            point = clientToDesign(window, point.x, point.y);
            if (point.y >= 0 && point.y < 72
                && !modern_ui::contains(modern_ui::rect(610, 10, 710, 65), point.x, point.y)) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_LBUTTONDOWN: {
            SetCapture(window);
            const POINT design = clientToDesign(
                window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            const int x = design.x;
            const int y = design.y;
            if (modern_ui::contains(modern_ui::rect(610, 10, 662, 65), x, y)) {
                ReleaseCapture();
                ShowWindow(window, SW_MINIMIZE);
                return 0;
            }
            if (modern_ui::contains(modern_ui::rect(665, 10, 715, 65), x, y)) {
                ReleaseCapture();
                SendMessage(window, WM_CLOSE, 0, 0);
                return 0;
            }
            if (y >= 72 && y < 132) {
                g_activePage = clamp(x / 180, 0, 3);
                updateFirmwareControls();
                InvalidateRect(window, nullptr, FALSE);
                ReleaseCapture();
                return 0;
            }

            if (g_activePage == 0
                && modern_ui::contains(modern_ui::rect(350, 705, 678, 785), x, y)) {
                g_activePage = 2;
                updateFirmwareControls();
                InvalidateRect(window, nullptr, FALSE);
            } else if (g_activePage == 1) {
                const int sliderY[] = {260, 422, 584, 746};
                for (int index = 0; index < 4; ++index) {
                    if (x >= 45 && x <= 670 && abs(y - sliderY[index]) <= 24) {
                        g_dragSlider = index;
                        setSliderValue(index, x);
                        InvalidateRect(window, nullptr, FALSE);
                        break;
                    }
                }
            } else if (g_activePage == 2) {
                if (modern_ui::contains(modern_ui::rect(385, 360, 510, 397), x, y)) {
                    beginUpdateCheck(window);
                } else if (modern_ui::contains(modern_ui::rect(520, 360, 665, 397), x, y)) {
                    beginReleaseFlash(window, true);
                } else if (modern_ui::contains(modern_ui::rect(55, 594, 665, 637), x, y)) {
                    showPortSelector(window);
                } else if (modern_ui::contains(modern_ui::rect(385, 745, 665, 805), x, y)) {
                    beginReleaseFlash(window, false);
                }
            } else if (g_activePage == 3) {
                if (modern_ui::contains(modern_ui::rect(575, 260, 680, 338), x, y)) {
                    const bool next = !g_settings.startWithWindows;
                    if (setStartWithWindows(next)) {
                        g_settings.startWithWindows = next;
                        persistSettings();
                    } else {
                        MessageBoxW(window, L"Windows startup registration could not be changed.",
                                    L"Settings", MB_ICONERROR);
                    }
                } else if (modern_ui::contains(modern_ui::rect(575, 385, 680, 465), x, y)) {
                    g_settings.minimizeToTray = !g_settings.minimizeToTray;
                    persistSettings();
                } else if (modern_ui::contains(modern_ui::rect(485, 535, 665, 585), x, y)) {
                    ShellExecuteW(window, L"open",
                        L"https://github.com/tminh-U/DIY-Haptic-Motor-for-Pedals",
                        nullptr, nullptr, SW_SHOWNORMAL);
                }
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEMOVE:
            if (g_dragSlider >= 0 && (wParam & MK_LBUTTON)) {
                const POINT design = clientToDesign(
                    window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                setSliderValue(g_dragSlider, design.x);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_DPICHANGED: {
            const UINT dpi = HIWORD(wParam);
            const RECT suggested = *reinterpret_cast<RECT*>(lParam);
            HMONITOR monitor = MonitorFromRect(&suggested, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info = {};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(monitor, &info);
            const SIZE size = scaledWindowSize(dpi, info.rcWork);
            const int x = clamp(suggested.left, info.rcWork.left,
                                max(info.rcWork.left, info.rcWork.right - size.cx));
            const int y = clamp(suggested.top, info.rcWork.top,
                                max(info.rcWork.top, info.rcWork.bottom - size.cy));
            SetWindowPos(window, nullptr, x, y, size.cx, size.cy,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP:
            if (g_dragSlider >= 0) saveAppSettings(g_settings);
            g_dragSlider = -1;
            ReleaseCapture();
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_COMBO_PORTS && HIWORD(wParam) == CBN_DROPDOWN) {
                refreshPortList();
            } else if (LOWORD(wParam) == 9001) {
                ShowWindow(window, SW_SHOW);
                SetForegroundWindow(window);
            } else if (LOWORD(wParam) == 9002) {
                g_settings.minimizeToTray = false;
                DestroyWindow(window);
            } else if (LOWORD(wParam) >= 9100 && LOWORD(wParam) < 9200) {
                const int selection = LOWORD(wParam) - 9100;
                SendMessage(hComboPorts, CB_SETCURSEL, selection, 0);
                g_manualFlashPortSelected = true;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMSUSPEND) {
                g_powerSuspended = true;
                stopConnection();
                clearLiveTelemetry();
                g_serialStatus = 3;
                InvalidateRect(window, nullptr, FALSE);
                return TRUE;
            }
            if (wParam == PBT_APMRESUMEAUTOMATIC
                || wParam == PBT_APMRESUMESUSPEND
                || wParam == PBT_APMRESUMECRITICAL) {
                g_powerSuspended = false;
                g_reconnectNotBefore = GetTickCount64() + 1500;
                SetTimer(window, 2, 1500, nullptr);
                InvalidateRect(window, nullptr, FALSE);
                return TRUE;
            }
            return TRUE;

        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVNODES_CHANGED
                || wParam == DBT_DEVICEREMOVECOMPLETE
                || wParam == DBT_DEVICEARRIVAL) {
                bool connectedPortPresent = false;
                if (g_serialPortName[0] != '\0') {
                    for (const std::string& port : getAvailableCOMPorts()) {
                        if (_stricmp(port.c_str(), g_serialPortName) == 0) {
                            connectedPortPresent = true;
                            break;
                        }
                    }
                }
                if (isConnected.load() && !connectedPortPresent) {
                    signalSerialDisconnect();
                }
                if (!g_powerSuspended.load()) {
                    g_reconnectNotBefore = GetTickCount64() + 500;
                    SetTimer(window, 2, 500, nullptr);
                }
                InvalidateRect(window, nullptr, FALSE);
            }
            return TRUE;

        case WM_TIMER: {
            if (wParam == 2) {
                KillTimer(window, 2);
            }
            if (g_esp32Panicked.load() && !g_panicMsgBoxShown.exchange(true)) {
                std::string panic;
                {
                    lock_guard<mutex> lock(g_panicMutex);
                    panic = g_panicMessage;
                }
                std::wstring messageText = L"ESP32 kernel panic detected.\n\n"
                    + utf8ToWide(panic) + L"\n\nReset the ESP32 before continuing.";
                MessageBoxW(window, messageText.c_str(), L"ESP32 Fatal Panic", MB_ICONERROR);
            }
            static ULONGLONG lastReconnectAttempt = 0;
            static ULONGLONG lastGameProbe = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - lastGameProbe >= 1000) {
                g_detectedGameForUi = static_cast<int>(detectRunningGame());
                lastGameProbe = now;
            }
            if (!isRunning.load() && !isConnected.load() && !g_flashing.load()
                && !g_powerSuspended.load()
                && (!g_esp32Panicked.load() || g_panicMsgBoxShown.load())
                && now >= g_reconnectNotBefore.load()
                && now - lastReconnectAttempt >= 3000) {
                lastReconnectAttempt = now;
                startConnection();
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_APP + 21:
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_APP + 22:
            updateFirmwareControls();
            if (wParam) startConnection();
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_APP + 23: {
            if (g_flashThread.joinable()) g_flashThread.join();
            std::wstring downloadedPath;
            std::wstring selectedPort;
            bool temporary = false;
            bool useArduinoCli = false;
            DWORD flashOffset = 0;
            {
                lock_guard<mutex> lock(g_firmwareMutex);
                downloadedPath = g_pendingFirmwarePath;
                selectedPort = g_pendingFirmwarePort;
                temporary = g_pendingFirmwareIsTemporary;
                useArduinoCli = g_pendingFirmwareUsesArduinoCli;
                flashOffset = g_pendingFirmwareOffset;
                g_pendingFirmwarePath.clear();
                g_pendingFirmwarePort.clear();
                g_pendingFirmwareIsTemporary = false;
                g_pendingFirmwareUsesArduinoCli = false;
                g_pendingFirmwareOffset = 0;
            }
            if (!downloadedPath.empty() && !selectedPort.empty()) {
                if (useArduinoCli) {
                    launchArduinoCliFlash(
                        window, selectedPort, downloadedPath, temporary);
                } else {
                    launchEsptoolFlash(
                        window, selectedPort, downloadedPath, flashOffset, temporary);
                }
            } else {
                g_firmwareStage = 0;
                g_flashing = false;
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_TRAYICON:
            if (lParam == WM_LBUTTONDBLCLK) {
                ShowWindow(window, SW_SHOW);
                ShowWindow(window, SW_RESTORE);
                SetForegroundWindow(window);
            } else if (lParam == WM_RBUTTONUP) {
                POINT cursor = {};
                GetCursorPos(&cursor);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 9001, L"Open Haptic Brake Control");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 9002, L"Exit");
                SetForegroundWindow(window);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
                DestroyMenu(menu);
            }
            return 0;

        case WM_CLOSE:
            if (g_settings.minimizeToTray) {
                addTrayIcon(window);
                ShowWindow(window, SW_HIDE);
            } else {
                DestroyWindow(window);
            }
            return 0;

        case WM_DESTROY:
            g_appClosing = true;
            KillTimer(window, 1);
            stopConnection();
            if (g_updateThread.joinable()) g_updateThread.join();
            if (g_flashThread.joinable()) g_flashThread.join();
            removeTrayIcon();
            DeleteObject(hFontRegular);
            DeleteObject(hFontBold);
            DeleteObject(hFontSmall);
            DeleteObject(hFontPageTitle);
            DeleteObject(hFontHero);
            DeleteObject(hFontValue);
            unloadPrivateUiFonts();
            if (g_appIconOwned && g_appIcon) DestroyIcon(g_appIcon);
            if (g_singleInstanceMutex) CloseHandle(g_singleInstanceMutex);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int nCmdShow) {
    if (lpCmdLine && strstr(lpCmdLine, "--self-test")) {
        NormalizedTelemetry testPacket;
        testPacket.sequence = 42;
        testPacket.brake = 0.75f;
        testPacket.speedKmh = 123.4f;
        testPacket.slipRatioFL = 0.12f;
        testPacket.slipRatioFR = 0.15f;
        testPacket.ndSlipFL = 0.8f;
        testPacket.ndSlipFR = 0.9f;
        testPacket.suspensionFL = 0.045f;
        testPacket.suspensionFR = 0.046f;
        float roadTest = calculateRoadIntensity(0.051f, 0.050f, 0.10f, 1.0f / 60.0f);
        const std::string jsonTest = R"({"tag_name":"v1.2.3"})";
        const RECT desktopTest = {0, 0, 1920, 1080};
        const SIZE dpi100 = scaledWindowSize(96, desktopTest);
        const SIZE dpi150 = scaledWindowSize(144, desktopTest);
        const FirmwareRelease mergedRelease = parseFirmwareReleaseJson(
            R"({"tag_name":"V2.0","assets":[{"browser_download_url":"https://example/firmware.bin"},{"browser_download_url":"https://example/firmware.merged.bin"}]})");
        const FirmwareRelease applicationRelease = parseFirmwareReleaseJson(
            R"({"tag_name":"V2.0","assets":[{"browser_download_url":"https://example/firmware.bin"}]})");
        const FirmwareRelease sourceRelease = parseFirmwareReleaseJson(
            R"({"tag_name":"V2.0","assets":[{"browser_download_url":"https://example/haptic_firmware.ino"}]})");
        const bool uiFontTest = privateUiFontSmokeTest();
        isRunning = true;
        isConnected = true;
        g_connectionStopRequested = false;
        signalSerialDisconnect();
        const bool disconnectStateTest = !isRunning.load() && !isConnected.load()
            && g_connectionStopRequested.load() && g_serialStatus.load() == 2;
        return (validateTelemetry(testPacket) && fabs(roadTest - 0.60f) < 0.001f
            && jsonStringValue(jsonTest, "tag_name") == "v1.2.3"
            && dpi100.cx == 480 && dpi100.cy == 600
            && dpi150.cx == 720 && dpi150.cy == 900
            && mergedRelease.ok && mergedRelease.supportsBlankBoard
            && mergedRelease.flashOffset == 0x0
            && applicationRelease.ok && !applicationRelease.supportsBlankBoard
            && applicationRelease.flashOffset == 0x10000
            && sourceRelease.ok && !sourceRelease.sketchUrl.empty()
            && uiFontTest
            && !deviceHeartbeatExpired(10000, 5000)
            && deviceHeartbeatExpired(10001, 5000)
            && disconnectStateTest) ? 0 : 2;
    }

    enablePerMonitorDpiAwareness();
    timeBeginPeriod(1);
    hInst = hInstance;
    const bool requestedTray = lpCmdLine && strstr(lpCmdLine, "--tray");
    g_singleInstanceMutex = CreateMutexW(
        nullptr, TRUE, L"Local\\HapticBrakeControl.SingleInstance");
    if (g_singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"HapticBrakeControlWindow", nullptr);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        MessageBoxW(nullptr,
            L"Haptic Brake Control is already running.\n\n"
            L"The existing window has been brought to the front. Check the system tray if you cannot see it.",
            L"Haptic Brake Control", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
        timeEndPeriod(1);
        return 0;
    }
    g_appIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (!g_appIcon) {
        g_appIcon = createWaveLogoIcon();
        g_appIconOwned = g_appIcon != nullptr;
    }
    loadPrivateUiFonts();

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = ModernWndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"HapticBrakeControlWindow";
    wc.hbrBackground = NULL;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = g_appIcon ? g_appIcon : LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassW(&wc)) {
        unloadPrivateUiFonts();
        if (g_appIconOwned && g_appIcon) DestroyIcon(g_appIcon);
        if (g_singleInstanceMutex) CloseHandle(g_singleInstanceMutex);
        timeEndPeriod(1);
        return 0;
    }

    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const SIZE initialSize = scaledWindowSize(dpiForWindowOrSystem(), workArea);
    const int x = workArea.left + (workArea.right - workArea.left - initialSize.cx) / 2;
    const int y = workArea.top + (workArea.bottom - workArea.top - initialSize.cy) / 2;
    hWndMain = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"HapticBrakeControlWindow",
        L"Haptic Brake Control",
        WS_POPUP | WS_MINIMIZEBOX,
        x, y, initialSize.cx, initialSize.cy,
        NULL, NULL, hInstance, NULL);
    SendMessageW(hWndMain, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
    SendMessageW(hWndMain, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));

    if (!hWndMain) {
        unloadPrivateUiFonts();
        if (g_appIconOwned && g_appIcon) DestroyIcon(g_appIcon);
        if (g_singleInstanceMutex) CloseHandle(g_singleInstanceMutex);
        timeEndPeriod(1);
        return 0;
    }

    ShowWindow(hWndMain, requestedTray ? SW_HIDE : nCmdShow);
    UpdateWindow(hWndMain);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    timeEndPeriod(1);
    return (int)msg.wParam;
}
