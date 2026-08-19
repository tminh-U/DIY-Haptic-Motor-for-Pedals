#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include "structed_file.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// ==============================================================================
// 1. PHẦN CORE TELEMETRY (GIỮ NGUYÊN 100% CẤU TRÚC NHƯ TRONG READ_AND_SEND.CPP)
// ==============================================================================
using namespace std;

const int HZ = 120;
const auto DURATION = chrono::microseconds(1000000 / HZ);

HANDLE hSerial = INVALID_HANDLE_VALUE;

bool initSerial(const char* portName) {
    hSerial = CreateFileA(portName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hSerial == INVALID_HANDLE_VALUE) {
        return false;
    }

    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    GetCommState(hSerial, &dcbSerialParams);
    dcbSerialParams.BaudRate = CBR_115200;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity   = NOPARITY;
    SetCommState(hSerial, &dcbSerialParams);

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.WriteTotalTimeoutConstant = 5;
    SetCommTimeouts(hSerial, &timeouts);
    return true;
}

struct SMElement {
    HANDLE hMapFile;
    unsigned char* mapFileBuffer;
};

SMElement m_physics;

// clean
void dismiss(SMElement element) {
    UnmapViewOfFile(element.mapFileBuffer);
    CloseHandle(element.hMapFile);
}

// get data from shared memory
bool initPhysics() {
    TCHAR szName[] = TEXT("Local\\acpmf_physics");
    m_physics.hMapFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SPageFilePhysics), szName);
    if (!m_physics.hMapFile) return false;
    m_physics.mapFileBuffer = (unsigned char*)MapViewOfFile(m_physics.hMapFile, FILE_MAP_READ, 0, 0, sizeof(SPageFilePhysics));
    if (!m_physics.mapFileBuffer) return false;
    return true;
}

void sendDataToESP32(float absActive, float brake, float wheelSlipL, float wheelSlipR, float SusL, float SusR) {
    if (hSerial == INVALID_HANDLE_VALUE) return;
    char buffer[64];
    int len = sprintf_s(buffer, "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n", absActive, brake, wheelSlipL, wheelSlipR, SusL, SusR);
    DWORD bytesWritten;
    WriteFile(hSerial, buffer, len, &bytesWritten, NULL);
}

// ==============================================================================
// 2. PHẦN ĐIỀU KHIỂN LUỒNG & GIAO DIỆN GUI
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

HINSTANCE hInst;
HWND hWndMain;
HWND hComboPorts, hBtnRefresh, hBtnConnect;
HWND hLblSerialStat, hLblGameStat, hLblFPS;
HWND hPrgBrake, hLblBrakeVal, hLblAbsStat;
HWND hPrgSlipL, hPrgSlipR, hLblSlipVal, hLblSusVal;

HBRUSH hBrushBg;
HBRUSH hBrushCard;
HBRUSH hBrushBtnDark;
HBRUSH hBrushActiveAbs;
HBRUSH hBrushInactiveAbs;
HFONT hFontRegular, hFontBold, hFontTitle, hFontStatus;

std::atomic<bool> isRunning(false);
std::atomic<bool> isConnected(false);
std::thread workerThread;

// Live Telemetry Cache cho GUI
struct LiveData {
    std::atomic<float> brake{0.0f};
    std::atomic<float> absVal{0.0f};
    std::atomic<float> slipL{0.0f};
    std::atomic<float> slipR{0.0f};
    std::atomic<float> susL{0.0f};
    std::atomic<float> susR{0.0f};
    std::atomic<int>   acState{0}; // 0 = Closed, 1 = Menu, 2 = Driving
    std::atomic<int>   fps{0};
} g_live;

int g_lastCheckPacketId = -1;
int g_idleAcCount = 0;

// Lấy danh sách cổng COM từ Registry Windows
std::vector<std::string> getAvailableCOMPorts() {
    std::vector<std::string> portList;
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
            std::string name = "COM" + std::to_string(i);
            std::string full = "\\\\.\\" + name;
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

// Background Worker Thread (Chạy đúng logic của hàm main() trong read_and_send.cpp)
void telemetryWorker(std::string com) {
    string fullPort = "\\\\.\\" + com;
    if (!initSerial(fullPort.c_str())) {
        isConnected = false;
        isRunning = false;
        SetWindowTextA(hLblSerialStat, ("ESP32: [FAILED TO OPEN " + com + "]").c_str());
        InvalidateRect(hBtnConnect, NULL, TRUE);
        EnableWindow(hComboPorts, TRUE);
        EnableWindow(hBtnRefresh, TRUE);
        return;
    }

    isConnected = true;
    SetWindowTextA(hLblSerialStat, ("ESP32: [CONNECTED] " + com + " (115200 baud)").c_str());
    InvalidateRect(hBtnConnect, NULL, TRUE);
    EnableWindow(hComboPorts, FALSE);
    EnableWindow(hBtnRefresh, FALSE);

    int frameCount = 0;
    auto lastFpsTime = std::chrono::steady_clock::now();

    while (isRunning) {
        if (!initPhysics()) {
            g_live.acState = 0;
            std::this_thread::sleep_for(chrono::milliseconds(500));
            continue;
        }

        SPageFilePhysics* pf = (SPageFilePhysics*)m_physics.mapFileBuffer;
        auto next_frame = chrono::steady_clock::now();
        int lstid = -1, cnt = 0;

        while (isRunning) {
            next_frame += DURATION;

            if (pf->packetId == lstid) {
                ++cnt;
                if (cnt > 240) {
                    g_live.acState = 1; // In Menu / Paused
                    break;
                }
            } else {
                cnt = 0;
                lstid = pf->packetId;
                g_live.acState = 2; // Active Driving
            }

            // Read telemetry values
            float absVal   = pf->abs;
            float brakeVal = pf->brake;
            float slipValL = pf->wheelSlip[0]; // Front-left wheel slip
            float slipValR = pf->wheelSlip[1]; // Front-right wheel slip
            float SusL     = pf->suspensionTravel[0]; // FL suspension
            float SusR     = pf->suspensionTravel[1]; // FR suspension

            // Cache cho GUI vẽ đồ họa
            g_live.brake = brakeVal;
            g_live.absVal = absVal;
            g_live.slipL = slipValL;
            g_live.slipR = slipValR;
            g_live.susL = SusL;
            g_live.susR = SusR;

            // Send packet to ESP32
            sendDataToESP32(absVal, brakeVal, slipValL, slipValR, SusL, SusR);

            // Đo FPS thực tế
            frameCount++;
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFpsTime).count() >= 1000) {
                g_live.fps = frameCount;
                frameCount = 0;
                lastFpsTime = now;
            }

            this_thread::sleep_until(next_frame);
        }

        dismiss(m_physics);
    }

    if (hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(hSerial);
        hSerial = INVALID_HANDLE_VALUE;
    }
    isConnected = false;
    g_live.fps = 0;
    SetWindowTextA(hLblSerialStat, "ESP32: [DISCONNECTED]");
    InvalidateRect(hBtnConnect, NULL, TRUE);
    EnableWindow(hComboPorts, TRUE);
    EnableWindow(hBtnRefresh, TRUE);
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

            // --- Card 1: Connection Settings ---
            HWND hTitle1 = CreateWindowA("STATIC", "  ESP32 + Simulator Connection", WS_CHILD | WS_VISIBLE, 20, 14, 460, 22, hWnd, NULL, hInst, NULL);
            SendMessage(hTitle1, WM_SETFONT, (WPARAM)hFontTitle, TRUE);

            HWND hLblCom = CreateWindowA("STATIC", "COM Port:", WS_CHILD | WS_VISIBLE, 32, 48, 75, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblCom, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            hComboPorts = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 115, 45, 115, 150, hWnd, (HMENU)IDC_COMBO_PORTS, hInst, NULL);
            SendMessage(hComboPorts, WM_SETFONT, (WPARAM)hFontBold, TRUE);
            
            hBtnRefresh = CreateWindowA("BUTTON", "Refresh", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 240, 44, 85, 28, hWnd, (HMENU)IDC_BTN_REFRESH, hInst, NULL);
            hBtnConnect = CreateWindowA("BUTTON", "Connect", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 335, 44, 130, 28, hWnd, (HMENU)IDC_BTN_CONNECT, hInst, NULL);

            hLblSerialStat = CreateWindowA("STATIC", "ESP32: [DISCONNECTED]", WS_CHILD | WS_VISIBLE, 32, 84, 420, 20, hWnd, (HMENU)IDC_LBL_SERIAL_STAT, hInst, NULL);
            SendMessage(hLblSerialStat, WM_SETFONT, (WPARAM)hFontStatus, TRUE);

            hLblGameStat   = CreateWindowA("STATIC", "Assetto Corsa: [CHECKING...]", WS_CHILD | WS_VISIBLE, 32, 108, 290, 20, hWnd, (HMENU)IDC_LBL_GAME_STAT, hInst, NULL);
            SendMessage(hLblGameStat, WM_SETFONT, (WPARAM)hFontStatus, TRUE);

            hLblFPS        = CreateWindowA("STATIC", "Stream: 0 Hz", WS_CHILD | WS_VISIBLE | SS_RIGHT, 330, 108, 135, 20, hWnd, (HMENU)IDC_LBL_FPS, hInst, NULL);
            SendMessage(hLblFPS, WM_SETFONT, (WPARAM)hFontStatus, TRUE);

            // --- Card 2: Live Telemetry Dashboard ---
            HWND hTitle2 = CreateWindowA("STATIC", "  Live Real-time Telemetry (120 Hz)", WS_CHILD | WS_VISIBLE, 20, 150, 460, 22, hWnd, NULL, hInst, NULL);
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
            HWND hLblSlp = CreateWindowA("STATIC", "Front Slip L/R:", WS_CHILD | WS_VISIBLE, 32, 248, 92, 20, hWnd, NULL, hInst, NULL);
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

            // Suspension Travel
            HWND hLblSus = CreateWindowA("STATIC", "Suspension:", WS_CHILD | WS_VISIBLE, 32, 282, 85, 20, hWnd, NULL, hInst, NULL);
            SendMessage(hLblSus, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            hLblSusVal = CreateWindowA("STATIC", "FL: 0.000m   |   FR: 0.000m", WS_CHILD | WS_VISIBLE, 125, 282, 340, 20, hWnd, (HMENU)IDC_LBL_SUS_VAL, hInst, NULL);
            SendMessage(hLblSusVal, WM_SETFONT, (WPARAM)hFontRegular, TRUE);

            refreshPortList();
            SetTimer(hWnd, 1, 33, NULL);
            break;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            SetBkMode(hdc, TRANSPARENT);

            if (hCtl == hLblAbsStat) {
                if (g_live.absVal.load() > 0.5f) {
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

            EndPaint(hWnd, &ps);
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == IDC_BTN_REFRESH) {
                refreshPortList();
            } else if (wmId == IDC_BTN_CONNECT) {
                if (!isRunning) {
                    char selPort[32] = { 0 };
                    int curSel = SendMessage(hComboPorts, CB_GETCURSEL, 0, 0);
                    if (curSel == CB_ERR) {
                        MessageBoxA(hWnd, "Please select a valid COM port from the list!", "Port Error", MB_ICONWARNING);
                        break;
                    }
                    SendMessageA(hComboPorts, CB_GETLBTEXT, curSel, (LPARAM)selPort);
                    isRunning = true;
                    if (workerThread.joinable()) workerThread.join();
                    workerThread = std::thread(telemetryWorker, std::string(selPort));
                } else {
                    isRunning = false;
                    if (workerThread.joinable()) workerThread.join();
                }
            }
            break;
        }

        case WM_TIMER: {
            if (wParam == 1) {
                // Kiểm tra trạng thái AC khi chưa bấm Connect
                if (!isRunning) {
                    HANDLE hAcTest = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_physics");
                    if (hAcTest) {
                        SPageFilePhysics* testPf = (SPageFilePhysics*)MapViewOfFile(hAcTest, FILE_MAP_READ, 0, 0, sizeof(SPageFilePhysics));
                        if (testPf) {
                            if (testPf->packetId != g_lastCheckPacketId) {
                                g_live.acState = 2; // Active Driving
                                g_lastCheckPacketId = testPf->packetId;
                                g_idleAcCount = 0;
                            } else {
                                g_idleAcCount++;
                                if (g_idleAcCount > 30) g_live.acState = 1; // Menu / Paused
                            }
                            UnmapViewOfFile(testPf);
                        }
                        CloseHandle(hAcTest);
                    } else {
                        g_live.acState = 0; // Game Closed
                    }
                }

                // Update Assetto Corsa Status
                int acState = g_live.acState.load();
                if (acState == 2) {
                    SetWindowTextA(hLblGameStat, "Assetto Corsa: [CONNECTED] Driving on Track");
                } else if (acState == 1) {
                    SetWindowTextA(hLblGameStat, "Assetto Corsa: [CONNECTED] In Menu / Paused");
                } else {
                    SetWindowTextA(hLblGameStat, "Assetto Corsa: [NOT DETECTED] Game Closed");
                }

                // Stream Rate
                int fps = g_live.fps.load();
                char fpsBuf[32];
                sprintf_s(fpsBuf, "Stream: %d Hz", fps);
                SetWindowTextA(hLblFPS, fpsBuf);

                // Update Live Telemetry
                float brake = g_live.brake.load();
                float absVal = g_live.absVal.load();
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

                // ABS Badge
                if (absVal > 0.5f) {
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

                // Sus
                char susBuf[64];
                sprintf_s(susBuf, "FL: %.3fm   |   FR: %.3fm", susL, susR);
                SetWindowTextA(hLblSusVal, susBuf);
            }
            break;
        }

        case WM_DESTROY: {
            KillTimer(hWnd, 1);
            isRunning = false;
            if (workerThread.joinable()) workerThread.join();

            DeleteObject(hBrushBg);
            DeleteObject(hBrushCard);
            DeleteObject(hBrushBtnDark);
            DeleteObject(hBrushActiveAbs);
            DeleteObject(hBrushInactiveAbs);
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
        CW_USEDEFAULT, CW_USEDEFAULT, 510, 365,
        NULL, NULL, hInstance, NULL
    );

    ShowWindow(hWndMain, nCmdShow);
    UpdateWindow(hWndMain);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
