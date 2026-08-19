#include <Arduino.h>
#include <math.h>
#include "soc/rtc_io_reg.h"


const uint32_t SAMPLE_RATE = 5000;
const float EPS = 1e-2;

const int LED_PIN = 2; 
const int DAC_PIN = 25; 


float cur_absVal   = 0.0f;
float cur_brakeVal = 0.0f;
float slipL    = 0.0f;
float slipR    = 0.0f;
float SusL    = 0.0f;
float SusR    = 0.0f;


const float dt = 1.0f / SAMPLE_RATE;

float cur_slip = 0.0f, cur_susL = 0.0f, cur_susR = 0.0f, delta_sus = 0.0f;
float pre_susL = 0.0f, pre_susR = 0.0f;


char buffer[64];
void serial_read(void *pvParameters) {
    for (;;) {
        if (Serial.available() > 0) {
                size_t len = Serial.readBytesUntil('\n', buffer, sizeof(buffer) - 1);

                if (len > 0) {
                    buffer[len] = '\0';
                    swap(cur_susL, pre_susL);
                    swap(cur_susR, pre_susR);
                }

                int count = sscanf(buffer, "%f, %f, %f, %f, %f, %f", &cur_absVal, &cur_brakeVal, &slipL, &slipR, &cur_susL, &cur_susR);

                if (cur_absVal < 0.5f) cur_brakeVal = 0;
                delta_sus = max(abs(cur_susL - pre_susL), abs(cur_susR - pre_susR));
                cur_slip = max(slipL, slipR);


                digitalWrite(LED_PIN, (count == 6 ? HIGH : LOW));
                                    
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}



float t_abs = 0.0f, t_brake = 0.0f, t_slip = 0.0f, t_sus = 0.0f;

float EMAsmooth(float target_val, float prev_val, const float alpha = 0.072f) {
    return prev_val + alpha * (target_val - prev_val);
}


float curg_brakeVal = 0.0f, curg_slip = 0.0f, curg_delta_sus = 0.0f;




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
    

float RP = (0.05f);
void IRAM_ATTR calc_effect() {



    curg_brakeVal = EMAsmooth(cur_brakeVal, curg_brakeVal);
    curg_delta_sus = EMAsmooth(delta_sus, curg_delta_sus);
    curg_slip = EMAsmooth(cur_slip, curg_slip);



    if (curg_brakeVal < EPS) {
        t_abs = 0;
    } else {
        t_abs += dt;
        if (t_abs > (1.0f / 12.0f)) t_abs -= (1.0f / 12.0f);
    }

    t_sus += dt;
    if (t_sus > 1.0f) t_sus -= 1.0f;


    if (curg_slip < 0.3f) {
        t_slip = 0;
    } else {
        t_slip += dt;
        if (t_slip > 1.0f) t_slip -= 1.0f;
    }


    //ABS : (freq : 50 -> 60hz, amplitude : Max 105, but interupt with 12hz freq)
    //The magnitude of ABS will change the amplitude. (120 * absVal)
    float abs_effect = (t_abs <= RP ? (105.0f * curg_brakeVal) * sinf(TWO_PI * 60.0f * t_abs) : 0.0f);

    //road effect : (freq : 100 -> 120hz, amplitude : Max 30)
    //Maximum delta = 0.06m
    //The delta between magnitude of suspension will change the amplitude. ((30 / 0.06) * delta(Max(SusL, SusR)))
    float road_effect = fminf(fabsf(curg_delta_sus) * (30.0f / 0.06f), 30.0f) * sinf(TWO_PI * 100.0f * t_sus);

    //Tire slip : (freq : ~45hz, amplitude : 10 - 37)
    float a_slip = 0.0f;
    if (curg_slip < 0.3f) {
        a_slip = 0.0f;
    } else if (curg_slip < 1.5f) {
        a_slip = 10.0f + 27.0f * (curg_slip - 0.3f) / (1.5f - 0.3f); 
    } else if (curg_slip > 1.5) {
        a_slip = 37.0f;
    }
    float slip_effect = a_slip * sinf(TWO_PI * 50.0f * t_slip);

    float total = 128 + (abs_effect + road_effect + slip_effect);
    total = fmaxf(total, 0.0f);
    total = fminf(total, 255.0f);
    SET_PERI_REG_BITS(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_DAC, (uint8_t)total, RTC_IO_PDAC1_DAC_S);
    
}

hw_timer_t *timer = NULL;


void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    dacWrite(DAC_PIN, 128);

    xTaskCreatePinnedToCore(serial_read, "Serial_Task", 8192, NULL, 1, NULL, 0);
    timer = timerBegin(1000000);
    timerAttachInterrupt(timer, &calc_effect);
    timerAlarm(timer, 200, true, 0);

}

void loop() {
    vTaskDelete(NULL);
}
