#include <math.h>
#include <WiFi.h>
#include "soc/rtc_io_reg.h"



#define SAMPLE_RATE 16000
#define EPS 1e-2f
#define DT (1.0f / SAMPLE_RATE)

const int LED_PIN = 2; 
const int DAC_PIN = 25; 
const char* HAPTIC_ID_PREFIX = "HAPTIC_PEDAL,1,";


volatile float road_intensity = 0.0f, cur_slip = 0.0f, cur_absVal = 0.0f;
volatile uint32_t telemetry_sequence = 0;

struct weightmp {
    float abs, sus, slip;
};

// Single-writer seqlock: publish a coherent telemetry snapshot without taking
// another portMUX from inside the GPTimer's shared interrupt handler.
__attribute__((always_inline)) void publishTelemetry(float absVal, float roadIntensity, float slip) {
    __atomic_add_fetch(&telemetry_sequence, 1, __ATOMIC_SEQ_CST); // odd: update in progress
    cur_absVal = absVal;
    road_intensity = roadIntensity;
    cur_slip = slip;
    __atomic_add_fetch(&telemetry_sequence, 1, __ATOMIC_SEQ_CST); // even: snapshot ready
}


char buffer[64];
void printHapticIdentity() {
    uint64_t chipId = ESP.getEfuseMac();
    Serial.printf("%s%04X%08X\n", HAPTIC_ID_PREFIX,
        static_cast<uint16_t>(chipId >> 32), static_cast<uint32_t>(chipId));
}


bool check(float absVal, float slipL, float slipR, float roadL, float roadR) {
    if (!isfinite(absVal) || !isfinite(slipL) || !isfinite(slipR) || !isfinite(roadL) || !isfinite(roadR)) return false;
    if (absVal < 0.0f || absVal > 1.0f) return false;
    if (slipL < 0.0f || slipL > 2.01f) return false;
    if (slipR < 0.0f || slipR > 2.01f) return false;
    if (roadL < 0.0f || roadL > 1.001f) return false;
    if (roadR < 0.0f || roadR > 1.001f) return false;
    return true;
}

void serial_read(void *pvParameters) {
    // Install the UART driver from Core 0 so its interrupt is allocated there.
    // The 16 kHz GPTimer is created by setup() on Core 1.
    Serial.begin(115200);
    Serial.setTimeout(20);

    unsigned long lastValidPacket = millis();
    bool watchdogActive = false;
    for (;;) {
        if (Serial.available() > 0) {
            size_t len = Serial.readBytesUntil('\n', buffer, sizeof(buffer) - 1);

            if (len > 0) {
                buffer[len] = '\0';

                // Host discovery handshake. This is intentionally separate
                // from the five-float telemetry protocol, so other USB serial
                // devices cannot be mistaken for this haptic controller.
                if (strcmp(buffer, "ID?") == 0) {
                    printHapticIdentity();
                    continue;
                }

                float absVal, slipL, slipR, roadL, roadR;
                int count = sscanf(buffer, "%f,%f,%f,%f,%f", &absVal, &slipL, &slipR, &roadL, &roadR);
                if (count == 5 && check(absVal, slipL, slipR, roadL, roadR)) {
                    publishTelemetry(absVal, max(roadL, roadR), max(slipL, slipR));
                    lastValidPacket = millis();
                    watchdogActive = false;
                    digitalWrite(LED_PIN, HIGH);
                }
            }
        }

        // Watchdog: if no valid data for 500ms, silence the motor
        // MUST be outside Serial.available() so it fires even when USB is disconnected
        if (!watchdogActive && millis() - lastValidPacket > 500) {
            publishTelemetry(0.0f, 0.0f, 0.0f);
            watchdogActive = true;
            digitalWrite(LED_PIN, LOW);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}





__attribute__((always_inline)) float EMAsmooth(float target_val, float prev_val, const float alpha = 0.01131f) {
    float diff = target_val - prev_val;
    float delta = (diff < 0.0f) ? -diff : diff;
    if (delta < 1e-4f) return target_val;
    return prev_val + alpha * diff;
}


DRAM_ATTR float curg_absVal = 0.0f, curg_slip = 0.0f, curg_road_intensity = 0.0f;




#define H_M (127.0f / (120.0f + 85.0f + 70.0f))

DRAM_ATTR weightmp mp[8] = {
    //ABS(bit0)         Road(bit1)        Slip(bit2)
   { 0.00f * H_M,      0.00f * H_M,      0.00f * H_M },
   { 1.00f * H_M,      0.00f * H_M,      0.00f * H_M },
   { 0.00f * H_M,      1.00f * H_M,      0.00f * H_M },
   { 0.75f * H_M,      0.25f * H_M,      0.00f * H_M },
   { 0.00f * H_M,      0.00f * H_M,      1.00f * H_M },
   { 0.85f * H_M,      0.00f * H_M,      0.15f * H_M },
   { 0.00f * H_M,      0.35f * H_M,      0.65f * H_M },
   { 0.60f * H_M,      0.25f * H_M,      0.15f * H_M }
};


DRAM_ATTR bool bit1 = 0;
__attribute__((always_inline)) weightmp Mix(volatile float abs, volatile float sus, volatile float slip) {
    bool bit0 = (abs >= EPS);
    if (sus >= EPS * 1.5f) bit1 = true;
    if (sus < 0.5f * EPS) bit1 = false;
    bool bit2 = (slip >= 0.03f);
    return mp[bit0 | (bit1 << 1) | (bit2 << 2)];
}


#define LUT_SIZE 1024

DRAM_ATTR uint16_t t_abs = 0;
#define step_abs   ((60.0f * LUT_SIZE) / SAMPLE_RATE)
#define step_slipA ((50.0f * LUT_SIZE) / SAMPLE_RATE)
#define step_slipB ((90.0f * LUT_SIZE) / SAMPLE_RATE)
#define step_sus   ((75.0f * LUT_SIZE) / SAMPLE_RATE)
DRAM_ATTR float phase_abs = 0.0f, phase_slip = 0.0f, phase_sus = 0.0f;


DRAM_ATTR float LUT[LUT_SIZE];

DRAM_ATTR bool flag = true, weight_flag = false;
DRAM_ATTR uint32_t swap_counter = 0;




//randomizer
DRAM_ATTR uint32_t xorshift_state = 67691;
__attribute__((always_inline)) uint32_t xorshift32() {
    xorshift_state ^= xorshift_state << 13;
    xorshift_state ^= xorshift_state >> 17;
    xorshift_state ^= xorshift_state << 5;
    return xorshift_state;
}


DRAM_ATTR float curg_weight_absVal = 0.0f, curg_weight_sus = 0.0f, curg_weight_slip = 0.0f;
DRAM_ATTR float last_raw_absVal = 0.0f, last_raw_road_intensity = 0.0f, last_raw_slip = 0.0f;


void IRAM_ATTR calc_effect() {

    // Bounded seqlock read: never spin indefinitely inside the 16 kHz ISR.
    // If the Core 0 writer is interrupted mid-update, reuse the last good frame.
    float local_absVal = last_raw_absVal;
    float local_road_intensity = last_raw_road_intensity;
    float local_slip = last_raw_slip;
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint32_t before = __atomic_load_n(&telemetry_sequence, __ATOMIC_SEQ_CST);
        if (before & 1U) continue;

        float candidate_absVal = cur_absVal;
        float candidate_road_intensity = road_intensity;
        float candidate_slip = cur_slip;
        uint32_t after = __atomic_load_n(&telemetry_sequence, __ATOMIC_SEQ_CST);

        if (before == after) {
            local_absVal = candidate_absVal;
            local_road_intensity = candidate_road_intensity;
            local_slip = candidate_slip;
            last_raw_absVal = candidate_absVal;
            last_raw_road_intensity = candidate_road_intensity;
            last_raw_slip = candidate_slip;
            break;
        }
    }

    curg_absVal = local_absVal; // ABS should respond instantly, no EMA
    curg_road_intensity = EMAsmooth(local_road_intensity, curg_road_intensity, local_road_intensity > curg_road_intensity ? 0.01131f : 0.0038f);
    curg_slip = EMAsmooth(local_slip, curg_slip);



    weightmp cur_weight = Mix(local_absVal, curg_road_intensity, curg_slip);
    if (!weight_flag) {
        curg_weight_absVal = cur_weight.abs;
        curg_weight_sus = cur_weight.sus;
        curg_weight_slip = cur_weight.slip;
        weight_flag = true;
    } else {
        curg_weight_absVal = EMAsmooth(cur_weight.abs, curg_weight_absVal, 0.04f);
        curg_weight_sus = EMAsmooth(cur_weight.sus, curg_weight_sus, 0.04f);
        curg_weight_slip = EMAsmooth(cur_weight.slip, curg_weight_slip, 0.04f);
    }


    //ABS : (freq : 50 -> 60hz, amplitude : Max 105, but interupt with 12hz freq)
    //The magnitude of ABS will change the amplitude. (120 * absVal)
    float abs_effect = (t_abs <= 800 ? (120.0f * curg_absVal) * LUT[int(phase_abs) & 1023] : 0.0f) * curg_weight_absVal;

    // Road effect: the PC has already calculated and clamped normalized
    // suspension velocity to 0..1 using each car's suspensionMaxTravel.
    float a_road = curg_road_intensity * 70.0f;
    a_road = (a_road < 70.0f ? a_road : 70.0f);

    float road_effect = curg_weight_sus * a_road * LUT[int(phase_sus) & 1023];

    // Longitudinal brake slip ratio: a light warning starts at 3%, then rises
    // more strongly through the tyre-limit region and toward full lock-up.
    float a_slip = 0.0f;
    if (curg_slip < 0.03f) {
        a_slip = 0.0f;
    } else if (curg_slip < 0.10f) {
        a_slip = 8.0f + 171.428571f * (curg_slip - 0.03f);
    } else if (curg_slip < 0.25f) {
        a_slip = 20.0f + 166.666667f * (curg_slip - 0.10f);
    } else if (curg_slip <= 1.0f) {
        a_slip = 45.0f + 53.333333f * (curg_slip - 0.25f);
    } else {
        a_slip = 85.0f;
    }
    float slip_effect = a_slip * LUT[int(phase_slip) & 1023] * curg_weight_slip;

    float total = 128 + (abs_effect + road_effect + slip_effect);
    total = (total > 0.0f ? total : 0);
    total = (total < 255.0f ? total : 255.0f);
    SET_PERI_REG_BITS(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_DAC, (uint8_t)total, RTC_IO_PDAC1_DAC_S);



    if (curg_absVal < EPS) {
        t_abs = 0;
        phase_abs = 0;
    } else {
        ++t_abs;
        if (t_abs > 1333) t_abs -= 1333;

        phase_abs += step_abs; 
        if (phase_abs >= LUT_SIZE) phase_abs -= LUT_SIZE;
    }

    phase_sus += step_sus;
    if (phase_sus >= LUT_SIZE) phase_sus -= LUT_SIZE;
    


    if (curg_slip < 0.03f) {
        phase_slip = 0;
    } else {
        swap_counter++;
        int random_val = (xorshift32() & 1023); 
        float factor = 0.6f + 0.8f * ((float)random_val * 0.0009765625f); // 1/1024 = 0.0009765625
        int swap_interval = (int)(640.0f * factor); // SAMPLE_RATE * 0.04 = 16000 * 0.04 = 640
        
        if (swap_counter >= swap_interval) {
            flag ^= 1;
            swap_counter = 0;
        }

        phase_slip += (flag ? step_slipA : step_slipB);
        if (phase_slip >= LUT_SIZE) phase_slip -= LUT_SIZE;
    }
    
}

hw_timer_t *timer = NULL;


void setup() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    btStop();

    pinMode(LED_PIN, OUTPUT);
    dacWrite(DAC_PIN, 128);


    for (int i = 0; i < LUT_SIZE; ++i) LUT[i] = sinf(TWO_PI * (float)i / LUT_SIZE);

    xTaskCreatePinnedToCore(serial_read, "Serial_Task", 8192, NULL, 1, NULL, 0);
    timer = timerBegin(2000000); 
    timerAttachInterrupt(timer, &calc_effect);
    timerAlarm(timer, 125, true, 0);

}

void loop() {
    vTaskDelete(NULL);
}
