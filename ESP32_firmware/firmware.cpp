#include <Arduino.h>
#include <math.h>

const uint32_t SAMPLE_RATE = 5000;
const float EPS = 1e-6;
const float TWO_PI = 6.2831852f;

// ================= CẤU HÌNH PHẦN CỨNG =================
// Đèn LED tích hợp trên bo mạch ESP32 DevKit V1 (Onboard Blue LED)
// Hầu hết các bản ESP32 DevKit V1 kết nối LED này vào chân GPIO 2
const int LED_PIN = 2; 

// Chân DAC (khi nào có dây điện thì cắm chân này sang amply)
const int DAC_PIN = 25; 


float cur_absVal   = 0.0f;
float cur_brakeVal = 0.0f;
float slipL    = 0.0f;
float slipR    = 0.0f;
float SusL    = 0.0f;
float SusR    = 0.0f;


const float dt = 1.0f / SAMPLE_RATE;




float cur_slip = 0.0f, cur_sus = 0.0f;
float pre_absVal = 0.0f, pre_brakeVal = 0.0f, pre_slip = 0.0f, pre_sus = 0.0f;

void serial_read(void *pvParameters) {
    for (;;) {
        if (Serial.available() > 0) {
                String packet = Serial.readStringUntil('\n');

                swap(cur_absVal, pre_absVal);
                swap(cur_brakeVal, pre_brakeVal);
                swap(cur_slip, pre_slip);
                swap(cur_sus, pre_sus);


                int count = sscanf(packet.c_str(), "%f, %f, %f, %f, %f, %f", &cur_absVal, &cur_brakeVal, &slipL, &slipR, &SusL, &SusR);
                digitalWrite(LED_PIN, (count == 6 ? HIGH : LOW));


               
                cur_slip = max(slipL, slipR);
                cur_sus = max(abs(SusL), abs(SusR));

                     
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}



float t_abs = 0.0f, t_brake = 0.0f, t_slip = 0.0f, t_sus = 0.0f;


float EMAsmooth(float target_val, float prev_val, const float alpha = 0.072f) {
    return prev_val + alpha * (target_val - prev_val);
}


float curg_brakeVal = 0.0f, curg_slip = 0.0f, curg_delta_sus = 0.0f;

float RP = ((1.0f / 55.0f) * 3.0f);
void IRAM_ATTR calc_effect() {
    //ABS : (freq : 50 -> 60hz, amplitude : Max120, but interupt with 12hz freq)
    //The magnitude of ABS will change the amplitude. (120 * absVal)
    if (cur_absVal < 0.5f) {
        t_abs = 0;
    } else {
        t_abs += dt;
    }
    if (t_abs > (1.0f / 12.0f)) t_abs -= (1.0f / 12.0f);


    curg_brakeVal = EMAsmooth(cur_brakeVal, curg_brakeVal);
    float abs_effect = (t_abs <= RP ? (120.0f * curg_brakeVal) * sinf(TWO_PI * 55.0f * t_abs) : 0.0f);

    //road effect : (freq : 100 -> 120hz, amplitude : 20 - 40)
    //Maximum delta = 0.06m
    //The delta between magnitude of suspension will change the amplitude. ((40 / 0.06) * delta(Max(SusL, SusR)))

    t_sus += dt;
    if (t_sus > 1.0f) t_sus -= 1.0f;

    float delta_sus = cur_sus - pre_sus;
    curg_delta_sus = EMAsmooth(delta_sus, curg_delta_sus);
    float road_effect = fminf(fabsf(curg_delta_sus) * (60.0f / 0.09f), 60.0f) * sinf(TWO_PI * 100.0f * t_sus);
                    
    //Tire slip : (freq : ~45hz, amplitude : 20 - 50)

    if (cur_slip < EPS) {
        t_slip = 0;
    } else {
        t_slip += dt;
    }
    if (t_slip > 1.0f) t_slip -= 1.0f;


    curg_slip = EMAsmooth(cur_slip, curg_slip);
    float a_slip = 0.0f;
    if (curg_slip < 0.3f) {
        a_slip = 0.0f;
    } else if (curg_slip < 1.5f) {
        a_slip = 20.0f + 30.0f * (curg_slip - 0.3f) / (1.5f - 0.3f); 
    } else if (curg_slip > 1.5) {
        a_slip = 50.0f;
    }
    float slip_effect = a_slip * sinf(TWO_PI * 45.0f * t_slip);
}

hw_timer_t *timer = NULL;


void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    dacWrite(DAC_PIN, 128);

    xTaskCreatePinnedToCore(serial_read, "Serial_Task", 4096, NULL, 1, NULL, 0);
    timer = timerBegin(0, 80, true);
    timerAttachInterrupt(timer, &calc_effect, true);
    timerAlarmWrite(timer, 200, true);
    timerAlarmEnable(timer);
}

void loop() {
    vTaskDelete(NULL);
}
