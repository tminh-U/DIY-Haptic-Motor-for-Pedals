#include <Arduino.h>
#include <math.h>
#include "soc/rtc_io_reg.h"

/**
 * ==============================================================================
 * ESP32 SIM RACING - TEST ROAD EFFECT (100 Hz) - HARDWARE TIMER 5000Hz TỐI ƯU NHẤT
 * ==============================================================================
 * Tối ưu phần cứng đỉnh cao:
 *  - Sử dụng đúng Hardware Timer ngắt phần cứng chính xác 5000 Hz (mỗi 200us).
 *  - Ghi thẳng vào thanh ghi phần cứng DAC1 (RTC_IO_PAD_DAC1_REG) trong 1 chu kỳ clock (~4ns).
 *  - Triệt tiêu 100% nguyên nhân gây crash của hàm dacWrite() trong ngắt ISR.
 * ==============================================================================
 */

// ================= CẤU HÌNH PHẦN CỨNG =================
const int DAC_PIN = 25;       // GPIO 25 (DAC1) nối IN+ Amply TPA3116D2
const int LED_PIN = 2;        // LED onboard

// ================= THÔNG SỐ TOÁN HỌC =================
const uint32_t SAMPLE_RATE = 5000;
const float dt = 1.0f / SAMPLE_RATE;

// ================= BIẾN TRẠNG THÁI =================
float delta_sus      = 0.0f;
float curg_delta_sus = 0.0f;
float t_sus          = 0.0f;

// Hàm lọc mượt EMA
inline float EMAsmooth(float target_val, float prev_val, const float alpha = 0.072f) {
    return prev_val + alpha * (target_val - prev_val);
}

hw_timer_t *timer = NULL;

// ================= NGẮT PHẦN CỨNG HARDWARE TIMER (5000 Hz) =================
void IRAM_ATTR calc_effect() {
    // 1. Lọc mượt chấn động hệ thống treo
    curg_delta_sus = EMAsmooth(delta_sus, curg_delta_sus);

    // 2. Quản lý thời gian pha Road Effect (100 Hz)
    t_sus += dt;
    if (t_sus > 1.0f) t_sus -= 1.0f;

    // 3. Tính biên độ chấn động mặt đường (0 -> 30)
    float a_road = fminf(fabsf(curg_delta_sus) * (30.0f / 0.06f), 30.0f);

    // 4. Tạo sóng sin 100 Hz
    float road_effect = a_road * sinf(TWO_PI * 100.0f * t_sus);

    // 5. Đưa ra DAC (Điểm tĩnh 128)
    float total = 128.0f + road_effect;
    if (total > 255.0f) total = 255.0f;
    if (total < 0.0f)   total = 0.0f;

    // GHI THẲNG VÀO THANH GHI PHẦN CỨNG DAC1 (1 chu kỳ xung nhịp ~4 nanogiây)
    // Cực nhanh, an toàn 100% trong Hardware Timer ISR mà không bị lỗi LoadProhibited
    SET_PERI_REG_BITS(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_DAC, (uint8_t)total, RTC_IO_PDAC1_DAC_S);
}

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    // Khởi tạo phần cứng DAC1 ban đầu
    dacWrite(DAC_PIN, 128);

    Serial.println("=============================================================");
    Serial.println(">>> ESP32 SIM RACING: TEST ROAD EFFECT (HARDWARE TIMER) <<<");
    Serial.println("=============================================================");
    Serial.printf(" - Tần số lấy mẫu: %u Hz (Hardware Timer ngắt mỗi 200us)\n", SAMPLE_RATE);
    Serial.println(" - Tần số rung: 100 Hz");
    Serial.println(" - Ghi trực tiếp thanh ghi phần cứng: RTC_IO_PAD_DAC1_REG");
    Serial.println(" - Tự động quét 4 mức độ rung mỗi 2.5 giây.");
    Serial.println("=============================================================");

    // Cấu hình Hardware Timer 5000Hz (chuẩn ESP32 Core v3.x)
    timer = timerBegin(1000000);                      // 1 MHz -> 1 tick = 1us
    timerAttachInterrupt(timer, &calc_effect);        // Gắn ngắt cứng ISR
    timerAlarm(timer, 200, true, 0);                  // 200us = 5000 Hz, tự động lặp lại
}

void loop() {
    // [1] Mặt đường phẳng mịn (delta_sus = 0.000m) -> Hoàn toàn êm
    delta_sus = 0.000f;
    Serial.println("\n[1/4] Smooth Asphalt (delta = 0.00m) -> Pedal em ru (A = 0.0)");
    vTaskDelay(pdMS_TO_TICKS(2500));

    // [2] Mặt đường nhám / gồ ghề nhẹ (delta_sus = 0.015m) -> Rung nhẹ li ti 100Hz
    delta_sus = 0.015f;
    Serial.println("[2/4] Rough Texture  (delta = 0.015m) -> Rung nhe mat duong 100Hz (A = 7.5)");
    vTaskDelay(pdMS_TO_TICKS(2500));

    // [3] Cán qua gờ giảm tốc / Kerb vừa (delta_sus = 0.035m) -> Rung giật rõ 100Hz
    delta_sus = 0.035f;
    Serial.println("[3/4] Kerb Rumble    (delta = 0.035m) -> Can go le vua 100Hz (A = 17.5)");
    vTaskDelay(pdMS_TO_TICKS(2500));

    // [4] Bay qua gờ lề cao / Xóc mạnh (delta_sus = 0.060m) -> Rung kịch trần 100Hz
    delta_sus = 0.060f;
    Serial.println("[4/4] Heavy Kerb     (delta = 0.060m) -> Xoc go le manh 100Hz (A = 30.0)");
    vTaskDelay(pdMS_TO_TICKS(2500));
}
