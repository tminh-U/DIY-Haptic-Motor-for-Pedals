#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include "soc/rtc_io_reg.h"


const uint32_t SAMPLE_RATE = 16000;
const float eps = 1e-2;
const float dt = 1.0f / SAMPLE_RATE;

const int LED_PIN = 2; 
const int DAC_PIN = 25; 


float slipL = 0.0f;
float slipR = 0.0f;

float cur_susL = 0.0f, cur_susR = 0.0f;
float pre_susL = 0.0f, pre_susR = 0.0f;

volatile float delta_sus = 0.0f, cur_slip = 0.0f, cur_absVal = 0.0f;

char buffer[64];
void serial_read(void *pvParameters) {
    unsigned long lastValidPacket = millis();
    for (;;) {
        if (Serial.available() > 0) {
                size_t len = Serial.readBytesUntil('\n', buffer, sizeof(buffer) - 1);

                if (len > 0) {
                    buffer[len] = '\0';
                    pre_susL = cur_susL;
                    pre_susR = cur_susR;
                }
                
                float absVal, susL, susR;
                int count = sscanf(buffer, "%f, %f, %f, %f, %f", &absVal, &slipL, &slipR, &susL, &susR);

                if (count == 5) {
                    float dL = fabsf(susL - pre_susL);
                    float dR = fabsf(susR - pre_susR);

                    //UART noise filter
                    if (dL < 0.1f && dR < 0.1f) {
                        cur_susL = susL;
                        cur_susR = susR;
                        cur_absVal = absVal;
                        delta_sus = max(dL, dR);
                        cur_slip = max(slipL, slipR);
                        lastValidPacket = millis();
                        digitalWrite(LED_PIN, HIGH);
                    }
                }
        }

        // Watchdog: if no valid data for 500ms, silence the motor
        // MUST be outside Serial.available() so it fires even when USB is disconnected
        if (millis() - lastValidPacket > 500) {
            cur_absVal = 0;
            delta_sus = 0;
            cur_slip = 0;
            digitalWrite(LED_PIN, LOW);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}





inline float IRAM_ATTR EMAsmooth(float target_val, float prev_val, const float alpha = 0.01131f) {
    float diff = target_val - prev_val;
    float delta = (delta < 0.0f) ? -diff : diff;
    if (delta < 1e-4f) return target_val;
    return prev_val + alpha * diff;
}


float curg_absVal = 0.0f, curg_slip = 0.0f, curg_delta_sus = 0.0f;


struct weightmp {
    float brake, sus, slip;
};

/*
const weightmp mp[8] = {
    //ABS(bit0)   Road(bit1)  Slip(bit2)
   { 0.00f,      0.00f,      0.00f },
   { 1.00f,      0.00f,      0.00f },
   { 0.00f,      1.00f,      0.00f },
   { 0.75f,      0.25f,      0.00f },
   { 0.00f,      0.00f,      1.00f },
   { 0.70f,      0.00f,      0.30f },
   { 0.00f,      0.45f,      0.55f },
   { 0.65f,      0.15f,      0.20f }
};

void Mix(float &brake, float &sus, float &slip) {
    bool bit0 = (brake >= EPS);
    bool bit1 = (sus >= EPS);
    bool bit2 = (slip > 0.3f);

    uint8_t mask = bit0 | (bit1 << 1) | (bit2 << 2);

    brake *= mp[mask].brake;
    sus *= mp[mask].sus;
    slip *= mp[mask].slip;
}
*/

const int LUT_SIZE = 1024;

uint16_t t_abs = 0;
float step_abs = (60.0f * LUT_SIZE) / SAMPLE_RATE, step_slipA = (50.0f * LUT_SIZE) / SAMPLE_RATE, step_slipB = (50.0f * LUT_SIZE) / SAMPLE_RATE, step_sus = (100.0f * LUT_SIZE) / SAMPLE_RATE;
float phase_abs = 0.0f, phase_slip = 0.0f, phase_sus = 0.0f;


float LUT[1024];

bool flag = true;
uint32_t swap_counter = 0;




//randomizer
uint32_t xorshift_state = 67691;
inline uint32_t IRAM_ATTR xorshift32() {
    xorshift_state ^= xorshift_state << 13;
    xorshift_state ^= xorshift_state >> 17;
    xorshift_state ^= xorshift_state << 5;
    return xorshift_state;
}



void IRAM_ATTR calc_effect() {

    curg_absVal = cur_absVal; // ABS should respond instantly, no EMA
    curg_delta_sus = EMAsmooth(delta_sus, curg_delta_sus, delta_sus > curg_delta_sus ? 0.01131f : 0.0038f);
    curg_slip = EMAsmooth(cur_slip, curg_slip);


    //ABS : (freq : 50 -> 60hz, amplitude : Max 105, but interupt with 12hz freq)
    //The magnitude of ABS will change the amplitude. (120 * absVal)
    float abs_effect = (t_abs <= 800 ? (60.0f * curg_absVal) * LUT[int(phase_abs) & 1023] : 0.0f);

    //road effect : (freq : 100 -> 120hz, amplitude : Max 30)
    //Maximum delta = 0.06m
    //The delta between magnitude of suspension will change the amplitude. ((30 / 0.06) * delta(Max(SusL, SusR)))

    float a_road = (curg_delta_sus > 0 ? curg_delta_sus : -curg_delta_sus) * (30.0f / 0.06f);
    a_road = (a_road < 30.0f ? a_road : 30.0f);

    float road_effect = a_road * LUT[int(phase_sus) & 1023];

    //Tire slip : (freq : ~45hz, amplitude : 10 - 37)
    float a_slip = 0.0f;
    if (curg_slip < 0.3f) {
        a_slip = 0.0f;
    } else if (curg_slip <= 1.5f) {
        a_slip = 10.0f + 27.0f * (curg_slip - 0.3f) / (1.5f - 0.3f); 
    } else if (curg_slip > 1.5) {
        a_slip = 37.0f;
    }
    float slip_effect = a_slip * LUT[int(phase_slip) & 1023];

    float total = 128 + (abs_effect + road_effect + slip_effect);
    total = (total > 0.0f ? total : 0);
    total = (total < 255.0f ? total : 255.0f);
    SET_PERI_REG_BITS(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_DAC, (uint8_t)total, RTC_IO_PDAC1_DAC_S);



    if (curg_absVal < eps) {
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
    


    if (curg_slip < 0.3f) {
        phase_slip = 0;
    } else {
        swap_counter++;
        int random_val = (xorshift32() & 1023); 
        float factor = 0.8f + 0.4f * ((float)random_val * 0.0009765625f); // 1/1024 = 0.0009765625
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
    Serial.begin(115200);


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
