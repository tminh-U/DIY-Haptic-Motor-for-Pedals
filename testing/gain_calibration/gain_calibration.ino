#include <math.h>
#include <WiFi.h>
#include "soc/rtc_io_reg.h"

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint32_t TIMER_FREQUENCY = 2000000;
constexpr uint32_t TIMER_PERIOD = TIMER_FREQUENCY / SAMPLE_RATE;
constexpr uint32_t PHASE_INCREMENT =
    (uint32_t)((60.0 * 4294967296.0 / SAMPLE_RATE) + 0.5);

constexpr int LED_PIN = 2;
constexpr int DAC_PIN = 25;
constexpr size_t LUT_SIZE = 1024;
constexpr float MAX_MIXER_AMPLITUDE = 125.0f;

DRAM_ATTR uint8_t outputLut[LUT_SIZE];
DRAM_ATTR volatile bool outputEnabled = false;
DRAM_ATTR uint32_t phaseAccumulator = 0;

float currentGain = 0.10f;
hw_timer_t *sampleTimer = nullptr;

void IRAM_ATTR outputCalibrationTone() {
    if (!outputEnabled) {
        SET_PERI_REG_BITS(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_DAC,
                          128, RTC_IO_PDAC1_DAC_S);
        phaseAccumulator = 0;
        return;
    }

    const uint16_t lutIndex = phaseAccumulator >> (32 - 10);
    SET_PERI_REG_BITS(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_DAC,
                      outputLut[lutIndex], RTC_IO_PDAC1_DAC_S);
    phaseAccumulator += PHASE_INCREMENT;
}

void rebuildOutputLut(float gain) {
    const bool resumeAfterUpdate = outputEnabled;
    outputEnabled = false;
    dacWrite(DAC_PIN, 128);

    currentGain = gain;
    const float amplitude = MAX_MIXER_AMPLITUDE * currentGain;

    for (size_t i = 0; i < LUT_SIZE; ++i) {
        const float sine = sinf(TWO_PI * (float)i / (float)LUT_SIZE);
        outputLut[i] = (uint8_t)lroundf(128.0f + amplitude * sine);
    }

    phaseAccumulator = 0;
    outputEnabled = resumeAfterUpdate;
}

void printStatus() {
    Serial.printf("GAIN=%.3f, DAC_PEAK=%.2f counts, OUTPUT=%s\n",
                  currentGain,
                  MAX_MIXER_AMPLITUDE * currentGain,
                  outputEnabled ? "ON" : "OFF");
}

void handleCommand(String command) {
    command.trim();

    if (command == "1") {
        phaseAccumulator = 0;
        outputEnabled = true;
        digitalWrite(LED_PIN, HIGH);
        Serial.println("OUTPUT ON: continuous 60 Hz sine");
        printStatus();
        return;
    }

    if (command == "0") {
        outputEnabled = false;
        phaseAccumulator = 0;
        dacWrite(DAC_PIN, 128);
        digitalWrite(LED_PIN, LOW);
        Serial.println("OUTPUT OFF");
        printStatus();
        return;
    }

    if (command.startsWith("G") || command.startsWith("g")) {
        const float requestedGain = command.substring(1).toFloat();

        if (requestedGain < 0.01f || requestedGain > 1.00f) {
            Serial.println("ERROR: use G0.01 ... G1.00");
            return;
        }

        rebuildOutputLut(requestedGain);
        Serial.println("GAIN UPDATED");
        printStatus();
        return;
    }

    if (command == "S" || command == "s") {
        printStatus();
        return;
    }

    Serial.println("UNKNOWN COMMAND: 1=ON, 0=OFF, G0.10=SET GAIN, S=STATUS");
}

void setup() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    btStop();

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    dacWrite(DAC_PIN, 128);

    Serial.begin(115200);
    Serial.setTimeout(50);

    rebuildOutputLut(currentGain);

    sampleTimer = timerBegin(TIMER_FREQUENCY);
    timerAttachInterrupt(sampleTimer, &outputCalibrationTone);
    timerAlarm(sampleTimer, TIMER_PERIOD, true, 0);

    Serial.println();
    Serial.println("TPA3116D2 GAIN CALIBRATION READY");
    Serial.println("Commands: 1=ON, 0=OFF, G0.10=SET GAIN, S=STATUS");
    Serial.println("Start at G0.10 and measure AC Vrms across SPK+ and SPK-.");
    printStatus();
}

void loop() {
    if (Serial.available() > 0) {
        handleCommand(Serial.readStringUntil('\n'));
    }
    delay(1);
}
