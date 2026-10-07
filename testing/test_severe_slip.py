"""Run app/firmware source on the PC without opening COM ports or driving motors.

Requires Python and the same g++ used by get_telemetry/build.ps1.
Arduino UART, time and DAC registers are simulated; waveform math is unchanged.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import runpy
import struct
import sys
import types


ROOT = Path(__file__).resolve().parents[1]
app = (ROOT / "get_telemetry/app_gui.cpp").read_text(encoding="utf-8")
firmware = (ROOT / "haptic_firmware/haptic_firmware.ino").read_text(encoding="utf-8")
# Compile the actual firmware receiver and synthesis, excluding ESP32-only setup/asm.
firmware = firmware[firmware.index("#define SAMPLE_RATE"):firmware.index("void IRAM_ATTR mem_workaround()")]
freshness = app[app.index("bool telemetryFresh ="):app.index("if (!telemetryFresh) {")]
gate = app[app.index("float slipL = min(fabs(latest.slipRatioFL)"):app.index("if (activeGame == GameKind::ACC) {", app.index("float slipL = min(fabs(latest.slipRatioFL)"))]
abs_gate = app[app.index("if (activeGame == GameKind::ACC) {", app.index("float slipL = min(fabs(latest.slipRatioFL)")):app.index("// A brake-pedal exciter should not react")]
worker = app[app.index("void telemetryWorker() {"):app.index("std::wstring utf8ToWide(")]
# Redirect discovery/mappings; run the complete worker and its real serial sender.
worker = worker.replace("void telemetryWorker()", "void auditTelemetryWorker()")
worker = worker.replace("initSerialAuto(", "auditInitSerial(").replace("detectRunningGame()", "GameKind::AC")
worker = worker.replace("initPhysics(", "auditInitPhysics(").replace("initStatic(", "auditInitStatic(")
worker = worker.replace("initPythonTelemetry()", "auditInitPythonTelemetry()")


def check_python_bridge():
    """Run the real AC callback with mock game API and an in-memory byte buffer."""
    cs = types.SimpleNamespace(**{name: name for name in
                                ("Brake", "SpeedKMH", "SlipRatio", "NdSlip", "SuspensionTravel")})
    values = {cs.Brake: .8, cs.SpeedKMH: 100., cs.SlipRatio: [1.] * 4,
              cs.NdSlip: [1.] * 4, cs.SuspensionTravel: [.04] * 4}
    events = []
    mock_ac = types.ModuleType("ac")
    mock_ac.getCarState = lambda car, field: values[field]
    mock_ac.setText = lambda *args: events.append(args)
    mock_ac.log = lambda *args: events.append(args)
    saved = {name: sys.modules.get(name) for name in ("ac", "acsys")}
    sys.modules["ac"] = mock_ac
    sys.modules["acsys"] = types.SimpleNamespace(CS=cs)
    try:
        bridge = runpy.run_path(str(ROOT / "assetto_corsa_python_app/haptic_telemetry/haptic_telemetry.py"))
        update = bridge["acUpdate"]
        state = update.__globals__
        state["_shared_memory"] = bytearray(b"HPT1" + b"\0" * 40)
        update(1 / 60)
        frame = bytes(state["_shared_memory"])
        assert struct.unpack_from("<I", frame, 4)[0] == 2
        assert struct.unpack_from("<I", frame, 40)[0] == 2
        values[cs.NdSlip] = None
        update(1 / 60)
        frame = bytes(state["_shared_memory"])
        assert struct.unpack_from("<I", frame, 4)[0] == 4
        assert struct.unpack_from("<2f", frame, 24) == (0., 0.)
        assert not state["_last_error"]
        values[cs.NdSlip] = [1.] * 4
        values[cs.SlipRatio] = [1e100] * 4
        update(1 / 60)
        assert bytes(state["_shared_memory"]) == frame and state["_last_error"]
        values[cs.SlipRatio] = [1.] * 4
        update(1 / 60)
        assert not state["_last_error"]
        state["_sequence"] = 0xfffffffe
        update(1 / 60)
        assert struct.unpack_from("<I", state["_shared_memory"], 4)[0] == 2
        print("AC Python callback: diagnostic errors preserve publication; pack errors preserve coherent frame; recovery/wrap OK", flush=True)
        return bytes(state["_shared_memory"])
    finally:
        for name, original in saved.items():
            if original is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = original


bridge_frame = check_python_bridge()

source = r'''
#include "app_gui.cpp"
#include <cassert>
#include <deque>
#include <limits>

namespace fw {
#define DRAM_ATTR
#define IRAM_ATTR
#define RTC_IO_PAD_DAC1_REG 0
#define RTC_IO_PDAC1_DAC 0
#define RTC_IO_PDAC1_DAC_S 0
#define HIGH 1
#define LOW 0
#define pdMS_TO_TICKS(x) (x)
int dac = 128;
unsigned long nowMs = 0, stopMs = 0;
int led = LOW;
void SET_PERI_REG_BITS(int, int, uint8_t value, int) { dac = value; }
unsigned long millis() { return nowMs; }
void digitalWrite(int, int value) { led = value; }
struct Stop {};
void advanceTime(int);
struct MockSerial {
    std::deque<std::pair<unsigned long, std::string>> packets;
    std::string pending;
    void begin(int) {}
    void setTimeout(int) {}
    void printf(const char*, ...) {}
    int available() {
        while (!packets.empty() && packets.front().first <= nowMs) {
            pending += packets.front().second;
            packets.pop_front();
        }
        return static_cast<int>(pending.size());
    }
    int read() {
        if (!available()) return -1;
        unsigned char value = pending.front(); pending.erase(0, 1); return value;
    }
} Serial;
struct MockESP { uint64_t getEfuseMac() { return 1; } } ESP;
void vTaskDelay(int);
''' + firmware + r'''
void advanceTime(int delay) {
    for (int ms = 0; ms < delay; ++ms) {
        for (int i = 0; i < SAMPLE_RATE / 1000; ++i) calc_effect();
        if (++nowMs >= stopMs) throw Stop{};
    }
}
void vTaskDelay(int delay) { advanceTime(delay); }
void runUntil(unsigned long stop) {
    stopMs = stop;
    try { serial_read(nullptr); } catch (const Stop&) {}
}
void reset() {
    publishTelemetry(0, 0, 0);
    curg_absVal = curg_slip = curg_road_intensity = 0;
    last_raw_absVal = last_raw_slip = last_raw_road_intensity = 0;
    last_raw_gains = {1, 1, 1, 1};
    curg_weight_absVal = curg_weight_sus = curg_weight_slip = 0;
    phase_abs = phase_slip = phase_sus = 0;
    t_abs = 0; swap_counter = 0; xorshift_state = 67691;
    flag = true; weight_flag = false; bit1 = false;
    nowMs = 0; led = LOW; Serial.packets.clear(); Serial.pending.clear();
}
}

bool freshAt(int elapsedMs) {
    auto now = chrono::steady_clock::now();
    auto lastPacketTime = now - chrono::milliseconds(elapsedMs);
    GameKind activeGame = GameKind::AC;
    ''' + freshness + r'''
    return telemetryFresh;
}
float gatedSlip(const NormalizedTelemetry& latest) {
    ''' + gate + r'''
    return brakeGate ? maxFrontSlip : 0;
}
bool absAt(const NormalizedTelemetry& latest, GameKind activeGame, bool absLatched, SPageFilePhysics* physics) {
    m_physics.mapFileBuffer = reinterpret_cast<unsigned char*>(physics);
    ''' + gate + abs_gate + r'''
    return absLatched;
}

std::pair<HANDLE, HANDLE> mockDuplex() {
    static int counter = 0;
    std::string name = "\\\\.\\pipe\\haptic-audit-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++counter);
    HANDLE server = CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                    1, 1024, 1024, 0, nullptr);
    assert(server != INVALID_HANDLE_VALUE);
    HANDLE client = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    assert(client != INVALID_HANDLE_VALUE);
    assert(ConnectNamedPipe(server, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED);
    return {server, client};
}
void pipeWrite(HANDLE server, const std::string& text) {
    DWORD written = 0;
    assert(WriteFile(server, text.data(), static_cast<DWORD>(text.size()), &written, nullptr));
    assert(written == text.size());
}
void monitorState() {
    isRunning = isConnected = true; g_connectionStopRequested = false;
    g_esp32Panicked = g_pauseSerialWrites = false; g_serialStatus = 1;
    g_hapticDeviceId[0] = 0;
}
bool auditInitSerial(char*, size_t, char*, size_t) {
    return hSerial.load() != INVALID_HANDLE_VALUE;
}
SMElement auditPythonPage{}, auditPhysicsPage{}, auditStaticPage{};
bool auditOpenPage(const SMElement& owner, SMElement& reader, size_t size) {
    assert(DuplicateHandle(GetCurrentProcess(), owner.hMapFile, GetCurrentProcess(),
                           &reader.hMapFile, 0, FALSE, DUPLICATE_SAME_ACCESS));
    reader.mapFileBuffer = static_cast<unsigned char*>(MapViewOfFile(reader.hMapFile, FILE_MAP_READ, 0, 0, size));
    assert(reader.mapFileBuffer);
    return true;
}
bool auditInitPhysics(GameKind) { return auditOpenPage(auditPhysicsPage, m_physics, sizeof(SPageFilePhysics)); }
bool auditInitStatic(GameKind) { return auditOpenPage(auditStaticPage, m_static, sizeof(SPageFileStatic)); }
bool auditInitPythonTelemetry() { return auditOpenPage(auditPythonPage, m_pythonTelemetry, sizeof(PythonTelemetryShared)); }
''' + worker + r'''

void allocatePage(SMElement& page, size_t size) {
    page.hMapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     static_cast<DWORD>(size), nullptr);
    assert(page.hMapFile);
    page.mapFileBuffer = static_cast<unsigned char*>(MapViewOfFile(page.hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, size));
    assert(page.mapFileBuffer);
}
void checkCompleteWorker() {
    std::cout << "Running complete AC worker replay...\n";
    allocatePage(auditPythonPage, sizeof(PythonTelemetryShared));
    allocatePage(auditPhysicsPage, sizeof(SPageFilePhysics));
    allocatePage(auditStaticPage, sizeof(SPageFileStatic));
    reinterpret_cast<SPageFilePhysics*>(auditPhysicsPage.mapFileBuffer)->abs = .1f;
    auto* shared = reinterpret_cast<volatile PythonTelemetryShared*>(auditPythonPage.mapFileBuffer);
    memcpy(auditPythonPage.mapFileBuffer, "HPT1", 4);
    m_pythonTelemetry = {}; m_physics = {}; m_static = {};
    uint32_t sequence = 0;
    auto publish = [&](float slip, float ndSlip, float brake, float speed) {
        shared->sequenceStart = ++sequence; MemoryBarrier();
        shared->brake = brake; shared->speedKmh = speed;
        shared->slipRatioFL = shared->slipRatioFR = slip;
        shared->ndSlipFL = shared->ndSlipFR = ndSlip;
        shared->suspensionFL = shared->suspensionFR = .04f;
        MemoryBarrier(); shared->sequenceEnd = ++sequence;
        MemoryBarrier(); shared->sequenceStart = sequence;
    };
    auto replay = [&](int frames, float slip, float ndSlip, float brake, float speed) {
        for (int i = 0; i < frames; ++i) { publish(slip, ndSlip, brake, speed); Sleep(17); }
    };
    wchar_t directory[MAX_PATH], file[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"hap", 0, file));
    hSerial = CreateFileW(file, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    assert(hSerial != INVALID_HANDLE_VALUE);
    monitorState(); g_live.acState = 0;
    strcpy(g_hapticDeviceId, "00C4D2BD2A58,1.04");
    g_masterGain = .75f; g_slipGain = g_absGain = g_roadGain = 1;
    publish(2, 1, .8f, 100);
    std::thread reader(auditTelemetryWorker);
    replay(10, 2, 1, .8f, 100);
    assert(g_live.acState == 2 && latestSerialFrame().slipRatioL == 2);
    replay(24, 11, 1, .8f, 100);
    assert(g_live.acState == 2 && latestSerialFrame().absVal == 1 && latestSerialFrame().slipRatioL == 2);
    assert(isConnected && isRunning && !g_esp32Panicked);
    replay(4, 2, 1, .8f, 100); assert(g_live.acState == 2 && latestSerialFrame().slipRatioL == 2);
    replay(4, 2, 1, 0, 100); assert(latestSerialFrame().slipRatioL == 0 && latestSerialFrame().absVal == 0);
    replay(4, 2, 1, .8f, 2); assert(latestSerialFrame().slipRatioL == 0);
    replay(24, 2, 101, .8f, 100); assert(g_live.acState == 2 && latestSerialFrame().slipRatioL == 2 && g_live.ndSlipL == 100);
    replay(20, std::numeric_limits<float>::quiet_NaN(), 1, .8f, 100);
    assert(g_live.acState == 1 && latestSerialFrame().slipRatioL == 0);
    replay(4, 2, 1, .8f, 100); assert(g_live.acState == 2);
    Sleep(300); assert(g_live.acState == 1 && latestSerialFrame().slipRatioL == 0);
    replay(4, 2, 1, .8f, 100); assert(g_live.acState == 2);
    isRunning = false; reader.join();
    dismiss(auditPythonPage); dismiss(auditPhysicsPage); dismiss(auditStaticPage);
    HANDLE serial = hSerial.load();
    DWORD length = GetFileSize(serial, nullptr); assert(length > 0);
    std::string sent(length, '\0'); DWORD count = 0;
    SetFilePointer(serial, 0, nullptr, FILE_BEGIN);
    assert(ReadFile(serial, sent.data(), length, &count, nullptr) && count == length);
    assert(sent.find("1.0000,2.0000,2.0000,0.0000,0.0000,0.7500,1.0000,1.0000,1.0000\n") != std::string::npos);
    assert(sent.find("0.0000,0.0000,0.0000,0.0000,0.0000,0.7500,1.0000,1.0000,1.0000\n") != std::string::npos);
    closeSerial(); assert(DeleteFileW(file));
    std::cout << "Complete AC worker + 60Hz sender: extreme slip/diagnostics keep effects; essential NaN/stale data silence; recovery/gates OK\n";
}

int main() {
    std::cout << std::unitbuf;
    for (int i = 0; i < fw::LUT_SIZE; ++i)
        fw::LUT[i] = sinf(6.28318530718f * i / fw::LUT_SIZE);

    // Exercise both actual shared-memory readers at and beyond app cutoffs.
    PythonTelemetryShared shared = {{'H','P','T','1'}, 2, .8f, 100, 1, 1, 1, 1, .04f, .04f, 2};
    acc::SPageFilePhysics accShared{};
    accShared.packetId = 1; accShared.brake = .8f; accShared.speedKmh = 100;
    m_pythonTelemetry.mapFileBuffer = reinterpret_cast<unsigned char*>(&shared);
    m_physics.mapFileBuffer = reinterpret_cast<unsigned char*>(&accShared);
    NormalizedTelemetry out;
    const unsigned char bridgeBytes[] = {''' + ",".join(str(b) for b in bridge_frame) + r'''};
    memcpy(&shared, bridgeBytes, sizeof(shared));
    assert(readPythonTelemetry(out) && out.slipRatioFL == 1 && out.speedKmh == 100);
    shared.sequenceStart = 3; assert(!readPythonTelemetry(out));
    shared.sequenceStart = 2; shared.sequenceEnd = 4; assert(!readPythonTelemetry(out));
    shared.sequenceEnd = 2; shared.magic[0] = 'X'; assert(!readPythonTelemetry(out)); shared.magic[0] = 'H';
    accShared.packetId = 0; assert(!readAccTelemetry(out)); accShared.packetId = 1;
    std::cout << "Actual Python frame -> C++ reader: ABI OK; odd/mismatched/invalid frames rejected\n";
    for (float slip : {0.25f, 1.f, 2.f, 10.f, 10.01f, 100.f}) {
        shared.slipRatioFL = accShared.slipRatio[0] = slip;
        const bool pythonAccepted = readPythonTelemetry(out);
        const bool accAccepted = readAccTelemetry(out);
        assert(pythonAccepted);
        assert(accAccepted == pythonAccepted);
        std::cout << "App SlipRatio=" << slip << ": "
                  << (pythonAccepted ? "accepted" : "whole frame rejected") << '\n';
    }
    shared.slipRatioFL = 1;
    shared.ndSlipFL = 100; assert(readPythonTelemetry(out));
    shared.ndSlipFL = 100.01f; assert(readPythonTelemetry(out) && out.ndSlipFL == 100);
    shared.ndSlipFL = std::numeric_limits<float>::quiet_NaN();
    assert(readPythonTelemetry(out) && out.ndSlipFL == 0);
    std::cout << "AC diagnostic values sanitized without rejecting valid brake data\n";
    shared.ndSlipFL = 1;
    shared.slipRatioFL = -11; assert(readPythonTelemetry(out));
    shared.slipRatioFL = std::numeric_limits<float>::quiet_NaN();
    assert(!readPythonTelemetry(out));
    shared.slipRatioFL = std::numeric_limits<float>::infinity();
    assert(!readPythonTelemetry(out));

    // Replay rejected frames using the exact worker freshness predicate.
    shared.slipRatioFL = 1; assert(readPythonTelemetry(out));
    assert(freshAt(250) && !freshAt(251));
    shared.slipRatioFL = std::numeric_limits<float>::quiet_NaN();
    int firstZeroMs = 0;
    for (int ms = 0; ms <= 800; ms += 4) {
        assert(!readPythonTelemetry(out));
        if (freshAt(ms)) publishSerialFrame(1, 1, 1, .5f, .5f);
        else {
            clearLiveTelemetry();
            if (!firstZeroMs) firstZeroMs = ms;
        }
    }
    assert(firstZeroMs == 252);
    auto zero = latestSerialFrame();
    assert(zero.absVal == 0 && zero.slipRatioL == 0 && zero.slipRatioR == 0
           && zero.roadL == 0 && zero.roadR == 0);
    shared.slipRatioFL = 1; assert(readPythonTelemetry(out)); assert(freshAt(0));
    std::cout << "App rejected-frame replay: all effects zero at " << firstZeroMs << " ms; valid data recovers\n";

    out.slipRatioFL = 100; out.brake = .8f; out.speedKmh = 100;
    assert(gatedSlip(out) == 2);
    out.brake = .05f; assert(gatedSlip(out) == 0);
    out.brake = .8f; out.speedKmh = 3; assert(gatedSlip(out) == 0);
    std::cout << "Brake gate: slip zero when brake <= 5% or speed <= 3 km/h\n";
    SPageFilePhysics physics{}; physics.abs = .10f;
    out.brake = .8f; out.speedKmh = 100; out.slipRatioFR = 0;
    out.slipRatioFL = .10f; assert(absAt(out, GameKind::AC, false, &physics));
    out.slipRatioFL = .08f; assert(absAt(out, GameKind::AC, true, &physics));
    out.slipRatioFL = .069f; assert(!absAt(out, GameKind::AC, true, &physics));
    physics.abs = 0; out.slipRatioFL = 2; assert(!absAt(out, GameKind::AC, true, &physics));
    out.nativeAbsValid = out.nativeAbsActive = true; assert(absAt(out, GameKind::ACC, false, nullptr));
    out.brake = 0; assert(!absAt(out, GameKind::ACC, true, nullptr));
    assert(calculateRoadIntensity(.2f, 0, .1f, .01f) == 1);
    assert(calculateRoadIntensity(.1f, .1f, .1f, .01f) == 0);
    assert(calculateRoadIntensity(.2f, 0, 0, .01f) == 0);
    std::cout << "AC ABS hysteresis / ACC native ABS / road normalization: OK\n";

    // Exercise actual host gain/CSV output against a temporary file, never COM.
    wchar_t tempDirectory[MAX_PATH], tempFile[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, tempDirectory));
    assert(GetTempFileNameW(tempDirectory, L"hap", 0, tempFile));
    HANDLE serial = CreateFileW(tempFile, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    assert(serial != INVALID_HANDLE_VALUE);
    hSerial = serial;
    for (const char* identity : {"00C4D2BD2A58,1.03", "00C4D2BD2A58,1.04"}) {
      strcpy(g_hapticDeviceId, identity);
      for (float gain : {.75f, .78f, 1.f}) {
        g_masterGain = gain; g_absGain = 1;
        g_roadGain = gain < 1 ? .68f : 1;
        g_slipGain = gain < 1 ? .82f : 1;
        SetFilePointer(serial, 0, nullptr, FILE_BEGIN);
        sendDataToESP32(1, 2, 2, 1, 1);
        assert(SetEndOfFile(serial));
        SetFilePointer(serial, 0, nullptr, FILE_BEGIN);
        char line[128]{}; DWORD bytes = 0;
        assert(ReadFile(serial, line, sizeof(line)-1, &bytes, nullptr));
        float a, l, r, rl, rr;
        assert(sscanf(line, "%f,%f,%f,%f,%f", &a, &l, &r, &rl, &rr) == 5);
        assert(fw::check(a, l, r, rl, rr));
        // Feed actual host CSV to the actual firmware parser.
        fw::reset(); fw::Serial.packets = {{0, std::string(line)}}; fw::runUntil(30);
        assert(fw::cur_slip > 0);
        if (std::string(identity).find("1.04") != std::string::npos) {
            assert(fw::cur_slip == 2 && fw::cur_gains.master == gain);
            assert(fw::cur_gains.slip == g_slipGain && fw::cur_gains.road == g_roadGain);
        } else assert(fw::cur_slip == l && fw::cur_gains.master == 1);
        std::cout << "Host " << identity << ", master=" << gain << " -> firmware CSV OK\n";
      }
    }
    closeSerial(); assert(DeleteFileW(tempFile));
    g_connectionStopRequested = false;
    hSerial = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1234));
    sendDataToESP32(1, 2, 2, 1, 1);
    assert(g_connectionStopRequested && !g_esp32Panicked);
    hSerial = INVALID_HANDLE_VALUE;

    // Real Win32 I/O with a simulated device; no COM ports opened.
    const std::string identity = "HAPTIC_PEDAL,1,00C4D2BD2A58,1.03\n";
    auto endpoints = mockDuplex();
    std::thread device([&] {
        char request[64]; DWORD count = 0;
        assert(ReadFile(endpoints.first, request, sizeof(request), &count, nullptr));
        pipeWrite(endpoints.first, identity.substr(0, 20));
        Sleep(30);
        pipeWrite(endpoints.first, identity.substr(20));
    });
    char receivedId[32]{};
    bool identityAccepted = readHapticIdentity(endpoints.second, receivedId, sizeof(receivedId));
    device.join();
    assert(identityAccepted && std::string(receivedId) == "00C4D2BD2A58,1.03");
    monitorState(); strcpy(g_hapticDeviceId, receivedId); hSerial = endpoints.second;
    pipeWrite(endpoints.first, identity + "Guru Meditation Error: heartbeat test\n"); CloseHandle(endpoints.first);
    serialMonitorWorker();
    assert(g_serialStatus == 5 && g_esp32Panicked);
    closeSerial();
    // The following panic is reached only if the complete heartbeat is accepted.
    assert(g_lastDeviceHeartbeat.load() > 0);
    std::cout << "Fragmented handshake waits for full ID; complete heartbeat accepted\n";
    for (const std::string& line : {std::string("Guru Meditation Error: test\n"), std::string("Brownout detector was triggered\n")}) {
        endpoints = mockDuplex(); monitorState(); hSerial = endpoints.second;
        pipeWrite(endpoints.first, line); CloseHandle(endpoints.first);
        serialMonitorWorker();
        assert(g_esp32Panicked && g_pauseSerialWrites && g_serialStatus == 5);
        closeSerial();
    }
    assert(!deviceHeartbeatExpired(10000, 5000) && deviceHeartbeatExpired(10001, 5000));
    std::cout << "Serial: failed writes/heartbeat expiry disconnect; Guru panic and brownout detected\n";
    checkCompleteWorker();

    // At maximum slip, check EVERY 100 ms for loss of AC output for 60 seconds.
    for (int mask : {4, 5, 6, 7}) {
        fw::reset();
        fw::publishTelemetry((mask & 1) ? 1 : 0, (mask & 2) ? 1 : 0, 2);
        int low = 255, high = 0, windowLow = 255, windowHigh = 0, clipped = 0;
        double sumSquares = 0;
        const int duration = mask == 4 ? 900 : 60;
        for (int i = 0; i < fw::SAMPLE_RATE * duration; ++i) {
            fw::calc_effect();
            low = std::min(low, fw::dac); high = std::max(high, fw::dac);
            windowLow = std::min(windowLow, fw::dac); windowHigh = std::max(windowHigh, fw::dac);
            clipped += fw::dac == 0 || fw::dac == 255;
            sumSquares += (fw::dac - 128.) * (fw::dac - 128.);
            if ((i + 1) % (fw::SAMPLE_RATE / 10) == 0) {
                assert(windowHigh - windowLow >= 30);
                windowLow = 255; windowHigh = 0;
            }
        }
        assert(clipped == 0);
        std::cout << "Firmware mix " << mask << ", " << duration << " simulated seconds: DAC "
                  << low << ".." << high << ", RMS " << sqrt(sumSquares / (fw::SAMPLE_RATE * duration))
                  << ", no clipping/flat 100ms windows\n";
    }

    fw::reset();
    for (int state = 0; state < 1000; ++state) {
        int mask = state % 8;
        fw::publishTelemetry((mask & 1) ? 1 : 0, (mask & 2) ? 1 : 0, (mask & 4) ? 2 : 0);
        for (int sample = 0; sample < 320; ++sample) {
            fw::calc_effect(); assert(fw::dac > 0 && fw::dac < 255);
        }
    }
    fw::reset(); fw::publishTelemetry(0, 0, 2); fw::calc_effect();
    ++fw::telemetry_sequence; // Writer stalled during publication.
    fw::cur_slip = 0;
    fw::cur_gains.master = 0;
    for (int sample = 0; sample < 1600; ++sample) fw::calc_effect();
    assert(fw::last_raw_slip == 2 && fw::last_raw_gains.master == 1);
    ++fw::telemetry_sequence; fw::calc_effect();
    assert(fw::last_raw_slip == 0 && fw::last_raw_gains.master == 0 && fw::dac == 128);
    std::cout << "Firmware: 1000 mix transitions stay within DAC range; odd seqlock reuses last frame without spinning\n";

    // Master/effect gains scale final amplitude even when physical slip saturates.
    for (float master : {0.f, .5f, .75f, 1.f}) {
        fw::reset(); fw::publishTelemetry(0, 0, 2, {master, 1, 1, 1});
        int gainLow = 255, gainHigh = 0;
        for (int i = 0; i < 16000; ++i) {
            fw::calc_effect(); gainLow = std::min(gainLow, fw::dac); gainHigh = std::max(gainHigh, fw::dac);
        }
        assert(gainHigh == static_cast<int>(128 + master * 85));
        assert(gainLow == static_cast<int>(128 - master * 85));
        std::cout << "Master=" << master << ": slip DAC " << gainLow << ".." << gainHigh << '\n';
    }
    fw::reset(); fw::publishTelemetry(0, 0, 2, {1, 1, 1, .5f});
    int gainLow = 255, gainHigh = 0;
    for (int i = 0; i < 16000; ++i) {
        fw::calc_effect(); gainLow = std::min(gainLow, fw::dac); gainHigh = std::max(gainHigh, fw::dac);
    }
    assert(gainLow == 85 && gainHigh == 170);

    // Run the real UART parser and watchdog, including heartbeat-only traffic.
    fw::reset();
    fw::Serial.packets = {{0, "0,2,2,0,0\n"}, {100, "0,2.02,2.02,0,0\n"},
                          {200, "ID?\n"}, {400, "0,nan,0,0,0\n"}, {550, "0,2,2,0,0\n"}};
    fw::runUntil(520);
    assert(fw::cur_slip == 0 && fw::led == LOW);
    for (int i = 0; i < fw::SAMPLE_RATE / 10; ++i) fw::calc_effect();
    assert(fw::dac == 128);
    fw::runUntil(560);
    assert(fw::cur_slip == 2 && fw::led == HIGH);
    std::cout << "Firmware: invalid slip > 2.01/NaN + ID? cannot feed watchdog; silence after 500ms, then recovery\n";
    fw::reset();
    for (unsigned long ms = 0; ms < 2000; ms += 17)
        fw::Serial.packets.emplace_back(ms, "1,2,2,1,1\n");
    fw::runUntil(2000); assert(fw::cur_slip == 2 && fw::led == HIGH);
    fw::reset(); fw::Serial.packets = {{0, "0,2"}, {5, ",2,0,0\n"}};
    fw::runUntil(30); assert(fw::cur_slip == 2);
    fw::reset(); fw::Serial.packets = {{0, "0,2"}, {30, ",2,0,0\n"}};
    fw::runUntil(70); assert(fw::cur_slip == 2);
    fw::reset(); fw::Serial.packets = {{0, "0,2,2,0,0,garbage\n"}};
    fw::runUntil(10); assert(fw::cur_slip == 0);
    fw::reset(); fw::Serial.packets = {{0, "0,2,2,0,0,0.75,1,1,1\n"}};
    fw::runUntil(10); assert(fw::cur_slip == 2 && fw::cur_gains.master == .75f);
    for (const std::string& malformed : {std::string("0,2,2,0,0,0.75\n"),
            std::string("0,2,2,0,0,nan,1,1,1\n"), std::string("0,2,2,0,0,1.1,1,1,1\n")}) {
        fw::reset(); fw::Serial.packets = {{0, malformed}};
        fw::runUntil(10); assert(fw::cur_slip == 0);
    }
    fw::reset(); fw::Serial.packets = {{0, std::string(200, '9') + "\n0,2,2,0,0\n"}};
    fw::runUntil(10); assert(fw::cur_slip == 2);
    std::cout << "Firmware UART: 60Hz, 5/30ms fragmentation, legacy/new gains, malformed rejection and overflow recovery OK\n";
    std::cout << "PASS (software simulation; no physical amplifier/motor/ESP32 timing measurement)\n";
}
'''
# LUT_SIZE and SAMPLE_RATE are firmware macros, not namespace members.
source = source.replace("fw::LUT_SIZE", "LUT_SIZE").replace("fw::SAMPLE_RATE", "SAMPLE_RATE")
compiler = shutil.which("g++")
if not compiler:
    raise SystemExit("g++ is required (the compiler used by get_telemetry/build.ps1)")
with tempfile.TemporaryDirectory(prefix="haptic-slip-") as directory:
    cpp = Path(directory) / "test.cpp"
    exe = Path(directory) / "test.exe"
    cpp.write_text(source, encoding="utf-8")
    subprocess.run([compiler, "-std=c++17", "-O2", "-Wno-attributes", "-I", str(ROOT / "get_telemetry"),
                    str(cpp), "-o", str(exe), "-static", "-lcomctl32", "-luxtheme",
                    "-ldwmapi", "-lgdi32", "-lwinmm", "-lwinhttp", "-lshell32",
                    "-lole32", "-luuid"], check=True)
    subprocess.run([str(exe)], check=True)
