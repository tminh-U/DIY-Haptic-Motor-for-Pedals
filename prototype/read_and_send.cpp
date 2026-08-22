#include <windows.h>
#include <timeapi.h>
#include <iostream>
#include <string>
#include <cmath>
#include <chrono>
#include <thread>
#include "structed_file.h"

#pragma comment(lib, "winmm.lib")

using namespace std;

const int HZ = 60;
const auto DURATION = chrono::microseconds(1000000 / HZ);

HANDLE hSerial = INVALID_HANDLE_VALUE;

bool initSerial(const char* portName) {
    hSerial = CreateFileA(portName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hSerial == INVALID_HANDLE_VALUE) {
        cout << "[Error] Failed to open port " << portName << " (Check Device Manager!)" << endl;
        return false;
    }

    // Configure Baudrate to 115200
    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    GetCommState(hSerial, &dcbSerialParams);
    dcbSerialParams.BaudRate = CBR_115200;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity   = NOPARITY;
    SetCommState(hSerial, &dcbSerialParams);

    // Set timeouts to prevent blocking
    COMMTIMEOUTS timeouts = { 0 };
    timeouts.WriteTotalTimeoutConstant = 5;
    SetCommTimeouts(hSerial, &timeouts);

    cout << "[OK] Successfully connected to ESP32 on " << portName << endl;
    return true;
}

template <typename T, unsigned S>
inline unsigned sze(const T(&v)[S]) {
	return S;
}

struct SMElement {
	HANDLE hMapFile;
	unsigned char* mapFileBuffer;
};

SMElement m_physics;

//clean
void dismiss(SMElement element) {
	UnmapViewOfFile(element.mapFileBuffer);
	CloseHandle(element.hMapFile);
}

//get data from shared memory (open only, do not create)
bool initPhysics() {
	m_physics.hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_physics");
	if (!m_physics.hMapFile) return false;
	m_physics.mapFileBuffer = (unsigned char*)MapViewOfFile(m_physics.hMapFile, FILE_MAP_READ, 0, 0, sizeof(SPageFilePhysics));
	if (!m_physics.mapFileBuffer) return false;
    return true;
}

void sendDataToESP32(float absActive, float wheelSlipL, float wheelSlipR, float SusL, float SusR) {
    if (hSerial == INVALID_HANDLE_VALUE) return;
    char buffer[64];
    int len = sprintf_s(buffer, "%.4f,%.4f,%.4f,%.4f,%.4f\n", absActive, wheelSlipL, wheelSlipR, SusL, SusR);
    DWORD bytesWritten;
    WriteFile(hSerial, buffer, len, &bytesWritten, NULL);
}

int main() {
    timeBeginPeriod(1);
    string com;
    // Connect to ESP32 (change "\\\\.\\COM3" to your actual COM port if needed)

    
    cout << "COM INPUT (e.g COM3) :";
    cin >> com;

    string fullPort = "\\\\.\\" + com;
    initSerial(fullPort.c_str());

    while(true) {

        if (!initPhysics()) {
            this_thread::sleep_for(chrono::seconds(1));
            continue;
        }
        

        cout << "Game detected" << endl;
        SPageFilePhysics* pf = (SPageFilePhysics*)m_physics.mapFileBuffer;
        auto next_frame = chrono::steady_clock::now();

        int lstid = pf->packetId, cnt = 0;
        bool isFresh = false;

        while (true) {
            next_frame += DURATION;

            if (pf->packetId == lstid) {
                ++cnt;
                if (cnt > 240) {
                    cout << "Waiting of acs.exe" << endl;
                    sendDataToESP32(0, 0, 0, 0, 0);
                    break;
                }
            } else {
                cnt = 0;
                lstid = pf->packetId;
                isFresh = true;
            }

            if (!isFresh || cnt > 20) {
                // Game paused or just connected
                sendDataToESP32(0, 0, 0, 0, 0);
            } else {
                // Read telemetry values
                float speedKmh = pf->speedKmh;
                // Force slip to 0 if speed < 3 km/h to eliminate physics noise at a standstill
                float slipValL = (speedKmh > 3.0f) ? pf->wheelSlip[0] : 0.0f; 
                float slipValR = (speedKmh > 3.0f) ? pf->wheelSlip[1] : 0.0f; 
                float SusL = pf->suspensionTravel[0]; // FL suspension
                float SusR = pf->suspensionTravel[1]; // FR suspension



                // bool flag = true;
                // for (int i = 0; i < 4; ++i) if (pf->wheelAngularSpeed[i] > 1.0f) flag = false;


                // Detect ABS via wheelSlip + brake (AC1's abs field is unreliable)
                float absActive = ((pf->brake > 0.05f) && (max(slipValL, slipValR) >= 0.8f) && (pf->abs > 0.0f)) ? 1.0f : 0.0f;

                // Send packet to ESP32
                sendDataToESP32(absActive, slipValL, slipValR, SusL, SusR);
            }

            this_thread::sleep_until(next_frame);
        }

        dismiss(m_physics);
    }
    if (hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(hSerial);
    }

    timeEndPeriod(1);
    return 0;
}