#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <timeapi.h>
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
#include "structed_file_AC.h"
#include "structed_file_ACC.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

//Compile arg : g++ -std=c++17 -O2 app_gui.cpp -o get_telemetry.exe -mwindows -static -static-libgcc -static-libstdc++ -lcomctl32 -luxtheme -ldwmapi -lgdi32 -lwinmm -s



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
const char* HAPTIC_HANDSHAKE_REQUEST = "ID?\n";
const char* HAPTIC_HANDSHAKE_PREFIX = "HAPTIC_PEDAL,1,";

vector<string> getAvailableCOMPorts();

bool configureSerial(HANDLE serial) {
    DCB dcbSerialParams = { 0 };
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

    COMMTIMEOUTS timeouts = { 0 };
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
    Sleep(1800);
    if (!PurgeComm(serial, PURGE_RXCLEAR | PURGE_TXCLEAR)) {
        CloseHandle(serial);
        return false;
    }

    if (!readHapticIdentity(serial, deviceId, deviceIdSize)) {
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
    HANDLE serial = hSerial.load(memory_order_acquire);
    if (serial == INVALID_HANDLE_VALUE) return;
    char buffer[64];
    int len = sprintf_s(buffer, "%.2f,%.4f,%.4f,%.4f,%.4f\n", absVal, slipRatioL, slipRatioR, roadL, roadR);
    DWORD bytesWritten;
    WriteFile(serial, buffer, len, &bytesWritten, NULL);
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

// Control IDs
#define IDC_COMBO_PORTS     1001
#define IDC_BTN_REFRESH     1002
#define IDC_BTN_CONNECT     1003
#define IDC_LBL_SERIAL_STAT 1004
#define IDC_LBL_GAME_STAT   1005
#define IDC_PRG_BRAKE       1006
#define IDC_LBL_BRAKE_VAL   1007
#define IDC_LBL_ABS_STAT    1008
#define IDC_PRG_SLIP_L      1009
#define IDC_PRG_SLIP_R      1010
#define IDC_LBL_SLIP_VAL    1011
#define IDC_LBL_SUS_VAL     1012
#define IDC_LBL_FPS         1013
#define IDC_LBL_PANIC_STAT  1014

HINSTANCE hInst;
HWND hWndMain;
HWND hComboPorts, hBtnRefresh, hBtnConnect;
HWND hLblSerialStat, hLblGameStat, hLblFPS;
HWND hPrgBrake, hLblBrakeVal, hLblAbsStat;
HWND hPrgSlipL, hPrgSlipR, hLblSlipVal, hLblSusVal;
HWND hLblRawBrake, hLblRawSlipL, hLblRawSlipR, hLblRawSusL, hLblRawSusR, hLblRawAbs;
HWND hLblPanicStat;

HBRUSH hBrushBg;
HBRUSH hBrushCard;
HBRUSH hBrushBtnDark;
HBRUSH hBrushActiveAbs;
HBRUSH hBrushInactiveAbs;
HBRUSH hBrushPanicBg;
HFONT hFontRegular, hFontBold, hFontTitle, hFontStatus;

atomic<bool> isRunning(false);
atomic<bool> isConnected(false);
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

void serialSenderWorker() {
    auto nextSend = chrono::steady_clock::now();
    while (isRunning) {
        nextSend += SERIAL_DURATION;
        SerialTelemetryFrame frame = latestSerialFrame();
        sendDataToESP32(frame.absVal, frame.slipRatioL, frame.slipRatioR,
                        frame.roadL, frame.roadR);

        auto now = chrono::steady_clock::now();
        if (nextSend < now - SERIAL_DURATION) nextSend = now;
        this_thread::sleep_until(nextSend);
    }

    sendDataToESP32(0, 0, 0, 0, 0);
}

int g_prevSerialStatus = -1; // Track previous status to avoid redundant GUI updates

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

void refreshPortList() {
    SendMessage(hComboPorts, CB_RESETCONTENT, 0, 0);
    auto ports = getAvailableCOMPorts();
    for (const auto& p : ports) {
        SendMessageA(hComboPorts, CB_ADDSTRING, 0, (LPARAM)p.c_str());
    }
    if (!ports.empty()) {
        SendMessage(hComboPorts, CB_SETCURSEL, 0, 0);
    }
}

// Serial Monitor Thread: reads lines from ESP32 and detects fatal panics
void serialMonitorWorker() {
    char readBuf[512];
    char lineBuf[512];
    int linePos = 0;
    bool isCapturing = false;
    int captureLines = 0;

    while (isRunning) {
        HANDLE serial = hSerial.load(memory_order_acquire);
        if (serial == INVALID_HANDLE_VALUE) {
            this_thread::sleep_for(chrono::milliseconds(100));
            continue;
        }

        DWORD bytesRead = 0;
        if (ReadFile(serial, readBuf, sizeof(readBuf) - 1, &bytesRead, NULL) && bytesRead > 0) {
            for (DWORD i = 0; i < bytesRead; i++) {
                char c = readBuf[i];
                if (c == '\n' || c == '\r') {
                    if (linePos > 0) {
                        lineBuf[linePos] = '\0';
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
        g_serialStatus = 4;
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
    g_serialStatus = 3;
}

// Window Procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_PROGRESS_CLASS };
            InitCommonControlsEx(&icex);

            // Dark Titlebar Windows 10/11
            BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

            // Fonts
            hFontRegular = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            hFontBold    = CreateFontA(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            hFontTitle   = CreateFontA(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            hFontStatus  = CreateFontA(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

            // Brushes
            hBrushBg          = CreateSolidBrush(RGB(18, 18, 20));    // #121214
            hBrushCard        = CreateSolidBrush(RGB(24, 24, 28));    // #18181C
            hBrushBtnDark     = CreateSolidBrush(RGB(42, 42, 50));    // #2A2A32
            hBrushActiveAbs   = CreateSolidBrush(RGB(255, 23, 68));   // #FF1744
            hBrushInactiveAbs = CreateSolidBrush(RGB(30, 30, 36));   // #1E1E24
            hBrushPanicBg     = CreateSolidBrush(RGB(180, 0, 0));    // Dark Red for panic

            // --- Card 1: Connection Settings ---
            HWND hTitle1 = CreateWindowA("STATIC", "  ESP32 + Simulator Connection", WS_CHILD | WS_VISIBLE, 20, 14, 460, 22, hWnd, NULL, hInst, NULL);
            SendMessage(hTitle1, WM_SETFONT, (WPARAM)hFontTitle, TRUE);

            HWND hLblCom = CreateWindowA("STATIC", "Auto scan:", WS_CHILD | WS_VISIBLE, 32, 48, 75, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblCom, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            hComboPorts = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 115, 45, 115, 150, hWnd, (HMENU)IDC_COMBO_PORTS, hInst, NULL);
            SendMessage(hComboPorts, WM_SETFONT, (WPARAM)hFontBold, TRUE);
            
            hBtnRefresh = CreateWindowA("BUTTON", "Refresh", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 240, 44, 85, 28, hWnd, (HMENU)IDC_BTN_REFRESH, hInst, NULL);
            hBtnConnect = CreateWindowA("BUTTON", "Connect", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 335, 44, 130, 28, hWnd, (HMENU)IDC_BTN_CONNECT, hInst, NULL);

            hLblSerialStat = CreateWindowA("STATIC", "ESP32: [DISCONNECTED]", WS_CHILD | WS_VISIBLE, 32, 84, 420, 20, hWnd, (HMENU)IDC_LBL_SERIAL_STAT, hInst, NULL);
            SendMessage(hLblSerialStat, WM_SETFONT, (WPARAM)hFontStatus, TRUE);

            hLblGameStat   = CreateWindowA("STATIC", "AC / ACC: [CHECKING...]", WS_CHILD | WS_VISIBLE, 32, 108, 290, 20, hWnd, (HMENU)IDC_LBL_GAME_STAT, hInst, NULL);
            SendMessage(hLblGameStat, WM_SETFONT, (WPARAM)hFontStatus, TRUE);

            hLblFPS        = CreateWindowA("STATIC", "Stream: 0 Hz", WS_CHILD | WS_VISIBLE | SS_RIGHT, 330, 108, 135, 20, hWnd, (HMENU)IDC_LBL_FPS, hInst, NULL);
            SendMessage(hLblFPS, WM_SETFONT, (WPARAM)hFontStatus, TRUE);

            // --- Card 2: Live Telemetry Dashboard ---
            HWND hTitle2 = CreateWindowA("STATIC", "  AC High-rate / ACC 60 Hz / Serial 60 Hz", WS_CHILD | WS_VISIBLE, 20, 150, 460, 22, hWnd, NULL, hInst, NULL);
            SendMessage(hTitle2, WM_SETFONT, (WPARAM)hFontTitle, TRUE);

            // Brake
            HWND hLblBrk = CreateWindowA("STATIC", "Brake Input:", WS_CHILD | WS_VISIBLE, 32, 182, 85, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblBrk, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            hPrgBrake = CreateWindowA(PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 125, 182, 260, 18, hWnd, (HMENU)IDC_PRG_BRAKE, hInst, NULL);
            SetWindowTheme(hPrgBrake, L"", L"");
            SendMessage(hPrgBrake, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SendMessage(hPrgBrake, PBM_SETBARCOLOR, 0, (LPARAM)RGB(255, 107, 0)); // Neon Orange
            SendMessage(hPrgBrake, PBM_SETBKCOLOR, 0, (LPARAM)RGB(35, 35, 42));

            hLblBrakeVal = CreateWindowA("STATIC", "0%", WS_CHILD | WS_VISIBLE | SS_RIGHT, 395, 182, 70, 20, hWnd, (HMENU)IDC_LBL_BRAKE_VAL, hInst, NULL);
            SendMessage(hLblBrakeVal, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            // ABS
            HWND hLblAbs = CreateWindowA("STATIC", "ABS Pulse:", WS_CHILD | WS_VISIBLE, 32, 214, 85, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblAbs, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            hLblAbsStat = CreateWindowA("STATIC", "  [ OFF ]  ", WS_CHILD | WS_VISIBLE | SS_CENTER, 125, 212, 130, 24, hWnd, (HMENU)IDC_LBL_ABS_STAT, hInst, NULL);
            SendMessage(hLblAbsStat, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            // Tire Slip
            HWND hLblSlp = CreateWindowA("STATIC", "Long SlipRatio:", WS_CHILD | WS_VISIBLE, 32, 248, 92, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblSlp, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            hPrgSlipL = CreateWindowA(PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 125, 248, 125, 16, hWnd, (HMENU)IDC_PRG_SLIP_L, hInst, NULL);
            hPrgSlipR = CreateWindowA(PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 260, 248, 125, 16, hWnd, (HMENU)IDC_PRG_SLIP_R, hInst, NULL);
            SetWindowTheme(hPrgSlipL, L"", L"");
            SetWindowTheme(hPrgSlipR, L"", L"");
            SendMessage(hPrgSlipL, PBM_SETRANGE, 0, MAKELPARAM(0, 200));
            SendMessage(hPrgSlipR, PBM_SETRANGE, 0, MAKELPARAM(0, 200));
            SendMessage(hPrgSlipL, PBM_SETBARCOLOR, 0, (LPARAM)RGB(0, 229, 255)); // Cyan
            SendMessage(hPrgSlipR, PBM_SETBARCOLOR, 0, (LPARAM)RGB(0, 229, 255));
            SendMessage(hPrgSlipL, PBM_SETBKCOLOR, 0, (LPARAM)RGB(35, 35, 42));
            SendMessage(hPrgSlipR, PBM_SETBKCOLOR, 0, (LPARAM)RGB(35, 35, 42));

            hLblSlipVal = CreateWindowA("STATIC", "0.00 / 0.00", WS_CHILD | WS_VISIBLE | SS_RIGHT, 395, 248, 70, 20, hWnd, (HMENU)IDC_LBL_SLIP_VAL, hInst, NULL);
            SendMessage(hLblSlipVal, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            // Road intensity derived from normalized suspension velocity
            HWND hLblSus = CreateWindowA("STATIC", "Road effect:", WS_CHILD | WS_VISIBLE, 32, 282, 85, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblSus, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            hLblSusVal = CreateWindowA("STATIC", "FL: 0.000   |   FR: 0.000", WS_CHILD | WS_VISIBLE, 125, 282, 340, 20, hWnd, (HMENU)IDC_LBL_SUS_VAL, hInst, NULL);
            SendMessage(hLblSusVal, WM_SETFONT, (WPARAM)hFontRegular, TRUE);

            // --- Card 3: ESP32 Health ---
            HWND hTitle3 = CreateWindowA("STATIC", "  ESP32 Health Monitor", WS_CHILD | WS_VISIBLE, 20, 325, 460, 22, hWnd, NULL, hInst, NULL);
            SendMessage(hTitle3, WM_SETFONT, (WPARAM)hFontTitle, TRUE);

            hLblPanicStat = CreateWindowA("STATIC", "  ESP32 Status: OK", WS_CHILD | WS_VISIBLE, 32, 355, 430, 22, hWnd, (HMENU)IDC_LBL_PANIC_STAT, hInst, NULL);
            SendMessage(hLblPanicStat, WM_SETFONT, (WPARAM)hFontStatus, TRUE);

            // --- Card 4: Raw Telemetry Data ---
            HWND hTitle4 = CreateWindowA("STATIC", "  Raw Telemetry Data", WS_CHILD | WS_VISIBLE, 20, 530, 460, 22, hWnd, NULL, hInst, NULL);
            SendMessage(hTitle4, WM_SETFONT, (WPARAM)hFontTitle, TRUE);

            hLblRawBrake = CreateWindowA("STATIC", "Brake: 0.0000", WS_CHILD | WS_VISIBLE, 32, 560, 140, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblRawBrake, WM_SETFONT, (WPARAM)hFontRegular, TRUE);

            hLblRawAbs = CreateWindowA("STATIC", "ABS signal: 0.0000", WS_CHILD | WS_VISIBLE, 180, 560, 140, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblRawAbs, WM_SETFONT, (WPARAM)hFontRegular, TRUE);

            hLblRawSlipL = CreateWindowA("STATIC", "Ratio FL: 0.0000", WS_CHILD | WS_VISIBLE, 32, 585, 140, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblRawSlipL, WM_SETFONT, (WPARAM)hFontRegular, TRUE);

            hLblRawSlipR = CreateWindowA("STATIC", "Ratio FR: 0.0000", WS_CHILD | WS_VISIBLE, 180, 585, 140, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblRawSlipR, WM_SETFONT, (WPARAM)hFontRegular, TRUE);

            hLblRawSusL = CreateWindowA("STATIC", "RoadL: 0.0000", WS_CHILD | WS_VISIBLE, 32, 610, 140, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblRawSusL, WM_SETFONT, (WPARAM)hFontRegular, TRUE);

            hLblRawSusR = CreateWindowA("STATIC", "RoadR: 0.0000", WS_CHILD | WS_VISIBLE, 180, 610, 140, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblRawSusR, WM_SETFONT, (WPARAM)hFontRegular, TRUE);

            refreshPortList();
            SetTimer(hWnd, 1, 33, NULL);
            break;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            SetBkMode(hdc, TRANSPARENT);

            if (hCtl == hLblPanicStat) {
                if (g_esp32Panicked.load()) {
                    SetTextColor(hdc, RGB(255, 255, 255));
                    return (INT_PTR)hBrushPanicBg;
                } else {
                    SetTextColor(hdc, RGB(0, 230, 118));
                    return (INT_PTR)hBrushCard;
                }
            } else if (hCtl == hLblAbsStat) {
                if (g_live.absVal.load() >= 0.5f) {
                    SetTextColor(hdc, RGB(255, 255, 255));
                    return (INT_PTR)hBrushActiveAbs;
                } else {
                    SetTextColor(hdc, RGB(90, 90, 100));
                    return (INT_PTR)hBrushInactiveAbs;
                }
            } else if (hCtl == hLblBrakeVal) {
                SetTextColor(hdc, RGB(255, 107, 0));
            } else if (hCtl == hLblSlipVal || hCtl == hLblFPS) {
                SetTextColor(hdc, RGB(0, 229, 255));
            } else if (hCtl == hLblSerialStat) {
                SetTextColor(hdc, isConnected ? RGB(0, 230, 118) : RGB(140, 140, 150));
            } else if (hCtl == hLblGameStat) {
                int st = g_live.acState.load();
                SetTextColor(hdc, (st == 2) ? RGB(0, 230, 118) : ((st == 1) ? RGB(255, 214, 0) : RGB(140, 140, 150)));
            } else {
                SetTextColor(hdc, RGB(220, 220, 230));
            }
            return (INT_PTR)hBrushCard;
        }

        case WM_CTLCOLORDLG:
            return (INT_PTR)hBrushBg;

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
            HDC hdc = pDIS->hDC;
            RECT rc = pDIS->rcItem;

            if (pDIS->CtlID == IDC_BTN_CONNECT) {
                COLORREF btnColor = isRunning ? RGB(211, 47, 47) : RGB(255, 107, 0);
                HBRUSH hBtnBrush = CreateSolidBrush(btnColor);
                FillRect(hdc, &rc, hBtnBrush);
                DeleteObject(hBtnBrush);

                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));
                SelectObject(hdc, hFontBold);

                const wchar_t* text = isRunning ? L"[X] DISCONNECT" : L"[>] CONNECT";
                DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (pDIS->CtlID == IDC_BTN_REFRESH) {
                FillRect(hdc, &rc, hBrushBtnDark);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(220, 220, 230));
                SelectObject(hdc, hFontBold);
                DrawTextW(hdc, L"Refresh", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            FillRect(hdc, &rcClient, hBrushBg);

            RECT rcCard1 = { 16, 12, 480, 135 };
            FillRect(hdc, &rcCard1, hBrushCard);
            FrameRect(hdc, &rcCard1, hBrushBtnDark);

            RECT rcCard2 = { 16, 145, 480, 315 };
            FillRect(hdc, &rcCard2, hBrushCard);
            FrameRect(hdc, &rcCard2, hBrushBtnDark);

            RECT rcCard3 = { 16, 320, 480, 390 };
            FillRect(hdc, &rcCard3, hBrushCard);
            FrameRect(hdc, &rcCard3, hBrushBtnDark);

            RECT rcCard4 = { 16, 525, 480, 640 };
            FillRect(hdc, &rcCard4, hBrushCard);
            FrameRect(hdc, &rcCard4, hBrushBtnDark);

            EndPaint(hWnd, &ps);
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == IDC_BTN_REFRESH) {
                refreshPortList();
            } else if (wmId == IDC_BTN_CONNECT) {
                if (!isRunning) {
                    g_serialPortName[0] = '\0';
                    g_hapticDeviceId[0] = '\0';
                    isRunning = true;
                    g_serialStatus = 0;
                    g_prevSerialStatus = -1;
                    g_esp32Panicked = false;
                    g_panicMsgBoxShown = false;
                    g_pauseSerialWrites = false;
                    g_panicMessage[0] = '\0';
                    if (workerThread.joinable()) workerThread.join();
                    if (serialMonitorThread.joinable()) serialMonitorThread.join();
                    closeSerial();
                    workerThread = thread(telemetryWorker);
                    serialMonitorThread = thread(serialMonitorWorker);
                } else {
                    isRunning = false;
                    if (workerThread.joinable()) workerThread.join();
                    if (serialMonitorThread.joinable()) serialMonitorThread.join();
                    closeSerial();
                }
            }
            break;
        }

        case WM_TIMER: {
            if (wParam == 1) {
                // --- Handle serial status changes from worker thread (thread-safe) ---
                int ss = g_serialStatus.load();
                if (ss != g_prevSerialStatus) {
                    g_prevSerialStatus = ss;
                    if (ss == 0 && isRunning) { // Discovery in progress
                        SetWindowTextA(hLblSerialStat, "ESP32: [SEARCHING FOR HAPTIC DEVICE...]");
                        EnableWindow(hComboPorts, FALSE);
                        EnableWindow(hBtnRefresh, FALSE);
                    } else if (ss == 1) { // Connected
                        char statusBuf[160];
                        sprintf_s(statusBuf, "ESP32: [AUTO CONNECTED] %s - %s", g_serialPortName, g_hapticDeviceId);
                        SetWindowTextA(hLblSerialStat, statusBuf);
                        EnableWindow(hComboPorts, FALSE);
                        EnableWindow(hBtnRefresh, FALSE);
                    } else if (ss == 2) { // Failed
                        char statusBuf[128];
                        sprintf_s(statusBuf, "ESP32: [FAILED TO OPEN %s]", g_serialPortName);
                        SetWindowTextA(hLblSerialStat, statusBuf);
                        EnableWindow(hComboPorts, TRUE);
                        EnableWindow(hBtnRefresh, TRUE);
                    } else if (ss == 4) { // No matching firmware identity
                        SetWindowTextA(hLblSerialStat, "ESP32: [HAPTIC DEVICE NOT FOUND]");
                        EnableWindow(hComboPorts, TRUE);
                        EnableWindow(hBtnRefresh, TRUE);
                    } else if (ss == 3) { // Disconnected
                        SetWindowTextA(hLblSerialStat, "ESP32: [DISCONNECTED]");
                        EnableWindow(hComboPorts, TRUE);
                        EnableWindow(hBtnRefresh, TRUE);
                    }
                    InvalidateRect(hBtnConnect, NULL, TRUE);
                }

                // Check the simulator even before connecting to ESP32.
                if (!isRunning) {
                    static DWORD lastGameProbeTick = 0;
                    DWORD nowTick = GetTickCount();
                    if (nowTick - lastGameProbeTick >= 500) {
                        GameKind detectedGame = detectRunningGame();
                        g_live.gameKind = static_cast<int>(detectedGame);
                        g_live.acState = (detectedGame == GameKind::None) ? 0 : 1;
                        lastGameProbeTick = nowTick;
                    }
                }

                // Update simulator status.
                int acState = g_live.acState.load();
                GameKind gameKind = static_cast<GameKind>(g_live.gameKind.load());
                if (acState == 2) {
                    SetWindowTextA(hLblGameStat, gameKind == GameKind::ACC
                        ? "ACC Shared Memory: [RECEIVING]"
                        : "AC Python API: [RECEIVING]");
                } else if (acState == 1) {
                    SetWindowTextA(hLblGameStat, gameKind == GameKind::ACC
                        ? "ACC: [WAITING FOR SHARED MEMORY]"
                        : "AC: [WAITING FOR PYTHON APP]");
                } else {
                    SetWindowTextA(hLblGameStat, "AC / ACC: [NOT DETECTED]");
                }

                // Stream Rate
                int fps = g_live.fps.load();
                char fpsBuf[32];
                sprintf_s(fpsBuf, "Stream: %d Hz", fps);
                SetWindowTextA(hLblFPS, fpsBuf);

                // Update Live Telemetry
                float brake = g_live.brake.load();
                float slipL = g_live.slipL.load();
                float slipR = g_live.slipR.load();
                float susL = g_live.susL.load();
                float susR = g_live.susR.load();

                // Brake
                int brakePercent = (int)(brake * 100.0f);
                SendMessage(hPrgBrake, PBM_SETPOS, brakePercent, 0);
                char brakeBuf[16];
                sprintf_s(brakeBuf, "%d%%", brakePercent);
                SetWindowTextA(hLblBrakeVal, brakeBuf);

                // AC derives ABS from SlipRatio; ACC uses its native `abs` signal.
                if (g_live.absVal.load() >= 0.5f) {
                    SetWindowTextA(hLblAbsStat, ">>> ABS ACTIVE <<<");
                } else {
                    SetWindowTextA(hLblAbsStat, "  [ OFF ]  ");
                }
                InvalidateRect(hLblAbsStat, NULL, TRUE);

                // Slip
                SendMessage(hPrgSlipL, PBM_SETPOS, (int)(slipL * 100.0f), 0);
                SendMessage(hPrgSlipR, PBM_SETPOS, (int)(slipR * 100.0f), 0);
                char slipBuf[32];
                sprintf_s(slipBuf, "%.2f / %.2f", slipL, slipR);
                SetWindowTextA(hLblSlipVal, slipBuf);

                // Normalized road intensity
                char susBuf[64];
                sprintf_s(susBuf, "FL: %.3f   |   FR: %.3f", susL, susR);
                SetWindowTextA(hLblSusVal, susBuf);

                // ESP32 Panic Detection
                if (g_esp32Panicked.load()) {
                    char panicBuf[300];
                    char firstLine[200] = {0};
                    {
                        lock_guard<mutex> lock(g_panicMutex);
                        // Extract only the first line for the small UI label
                        const char* newlinePos = strchr(g_panicMessage, '\n');
                        if (newlinePos) {
                            int len = min((int)(newlinePos - g_panicMessage), 199);
                            strncpy(firstLine, g_panicMessage, len);
                            firstLine[len] = '\0';
                        } else {
                            strncpy(firstLine, g_panicMessage, 199);
                        }
                        sprintf_s(panicBuf, "  [!] ESP32 CRASH: %s", firstLine);
                    }
                    SetWindowTextA(hLblPanicStat, panicBuf);
                    InvalidateRect(hLblPanicStat, NULL, TRUE);

                    // Show MessageBox only once per panic event
                    if (!g_panicMsgBoxShown.exchange(true)) {
                        char msgBuf[2500];
                        {
                            lock_guard<mutex> lock(g_panicMutex);
                            sprintf_s(msgBuf, sizeof(msgBuf), "ESP32 has crashed (fatal panic)!\n\n%s\n\nPlease reset the ESP32.", g_panicMessage);
                        }
                        MessageBoxA(hWnd, msgBuf, "ESP32 Fatal Panic", MB_ICONERROR);
                    }
                } else if (isConnected.load()) {
                    SetWindowTextA(hLblPanicStat, "  ESP32 Status: OK");
                    InvalidateRect(hLblPanicStat, NULL, TRUE);
                }

                // Raw Data Update
                float absVal = g_live.absVal.load();
                char rawBuf[64];
                sprintf_s(rawBuf, "Brake: %.4f", brake); SetWindowTextA(hLblRawBrake, rawBuf);
                sprintf_s(rawBuf, "ABS signal: %.4f", absVal); SetWindowTextA(hLblRawAbs, rawBuf);
                sprintf_s(rawBuf, "Ratio FL: %.4f", slipL); SetWindowTextA(hLblRawSlipL, rawBuf);
                sprintf_s(rawBuf, "Ratio FR: %.4f", slipR); SetWindowTextA(hLblRawSlipR, rawBuf);
                sprintf_s(rawBuf, "RoadL: %.4f", susL); SetWindowTextA(hLblRawSusL, rawBuf);
                sprintf_s(rawBuf, "RoadR: %.4f", susR); SetWindowTextA(hLblRawSusR, rawBuf);
            }
            break;
        }

        case WM_DESTROY: {
            KillTimer(hWnd, 1);
            isRunning = false;
            if (workerThread.joinable()) workerThread.join();
            if (serialMonitorThread.joinable()) serialMonitorThread.join();
            closeSerial();

            DeleteObject(hBrushBg);
            DeleteObject(hBrushCard);
            DeleteObject(hBrushBtnDark);
            DeleteObject(hBrushActiveAbs);
            DeleteObject(hBrushInactiveAbs);
            DeleteObject(hBrushPanicBg);
            DeleteObject(hFontRegular);
            DeleteObject(hFontBold);
            DeleteObject(hFontTitle);
            DeleteObject(hFontStatus);

            PostQuitMessage(0);
            break;
        }

        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
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
        return (validateTelemetry(testPacket) && fabs(roadTest - 0.60f) < 0.001f) ? 0 : 2;
    }

    timeBeginPeriod(1);
    hInst = hInstance;

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = "HapticPedalDarkGUI";
    wc.hbrBackground = NULL;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassA(&wc)) return 0;

    hWndMain = CreateWindowA(
        "HapticPedalDarkGUI",
        "get_telemetry",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 510, 700,
        NULL, NULL, hInstance, NULL
    );

    ShowWindow(hWndMain, nCmdShow);
    UpdateWindow(hWndMain);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    timeEndPeriod(1);
    return (int)msg.wParam;
}

