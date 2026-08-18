#include <windows.h>
#include <iostream>
#include <string>
#include <cmath>
#include <chrono>
#include <thread>
#include "structed_file.h"

using namespace std;

const int HZ = 120;
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

//get data from shared memory
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

int main() {
    // Connect to ESP32 (change "\\\\.\\COM3" to your actual COM port if needed)

    string com = "COM3";
    string fullPort = "\\\\.\\" + com;
    initSerial(fullPort.c_str());

    while(true) {

        if (!initPhysics()) this_thread::sleep_for(chrono::seconds(1))
        

        cout << "Game detected" << endl;
        SPageFilePhysics* pf = (SPageFilePhysics*)m_physics.mapFileBuffer;
        auto next_frame = chrono::steady_clock::now();

        int lstid = -1, cnt = 0;
        while (true) {        
            next_frame += DURATION;
            
            if (pf->packetID == lstid) {
                ++cnt;
                if (cnt > 240) {
                    cout << "Waiting of acs.exe" << endl;
                    break;
                }
            }


            // Read telemetry values
            float absVal = pf->abs;
            float brakeVal = pf->brake;
            float slipValL = pf->wheelSlip[0]; // Front-left wheel slip
            float slipValR = pf->wheelSlip[1]; // Front-right wheel slip
            float SusL = pf->suspensionTravel[0]; // FL suspension
            float SusR = pf->suspensionTravel[1]; // FR suspension

            // Send packet to ESP32
            sendDataToESP32(absVal, brakeVal, slipValL, slipValR, SusL, SusR);
            this_thread::sleep_until(next_frame);
        }

        dismiss(m_physics);
        if (hSerial != INVALID_HANDLE_VALUE) {
            CloseHandle(hSerial);
        }
    }

    
    return 0;
}
