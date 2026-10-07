# Kiểm tra mất rung khi trượt mạnh — 2026-10-07

**Cập nhật 1.04:** đã sửa các lỗi phần mềm tái hiện bên dưới. Test hiện là regression
test: extreme slip và NdSlip lỗi giữ phản hồi; ID chia nhỏ chờ đủ dòng; Master và
gain từng hiệu ứng điều chỉnh biên độ sóng. UART giữ frame qua khoảng nghỉ 30 ms,
loại dữ liệu thừa, và app nhận diện brownout/watchdog fault.

Đã kiểm tra Python bridge của AC, reader AC/ACC, toàn bộ worker xử lý telemetry,
sender 60 Hz, handshake/monitor serial, parser/watchdog firmware, mixer, phase,
EMA và cách áp dụng gain. Các cơ chế dưới đây tái hiện được; chưa có bản ghi
telemetry hoặc phép đo lúc sự cố thực tế xảy ra để xác định cơ chế nào đã xảy ra.

## Chẩn đoán ban đầu trên 1.03

| Điểm | Bằng chứng | Quan hệ với triệu chứng |
| --- | --- | --- |
| App loại cả frame khi `abs(SlipRatio) > 10` hoặc `abs(NdSlip) > 100` | Chạy reader và toàn bộ worker AC với frame mới liên tục nhưng vượt ngưỡng. Sau hơn 250 ms, sender gửi cả năm trường bằng 0. App vẫn Connected, không có cờ panic. | Tái hiện đúng việc mất toàn bộ rung mà không fatal panic. Chưa biết giá trị thực lúc xe trượt. |
| `NdSlip` dùng cho chẩn đoán nhưng có thể ngắt cả stream | Giá trị 100.01 làm reader từ chối frame dù SlipRatio hợp lệ. Mock API trả lỗi NdSlip khiến callback Python không xuất bản frame mới. | Chỉ một trường chẩn đoán bị lỗi cũng làm ABS, slip và road mất sau timeout. |
| Handshake nhận ID khi chưa kết thúc dòng | Chia phản hồi `HAPTIC_PEDAL,1,00C4D2BD2A58,1.03\n` thành hai phần qua Win32 named pipe. `readHapticIdentity` chấp nhận ID thiếu `00C4D`; heartbeat đầy đủ sau đó gây disconnect, không có panic. | Lỗi xác định được ở kết nối. Không phụ thuộc mức slip; thường ảnh hưởng khi vừa kết nối hoặc kết nối lại. |
| Master gain nhân vào dữ liệu, không nhân vào sóng cuối | Với Master 75%, TireSlip 100%, raw slip 2: app gửi slip 1.5; firmware vẫn tạo DAC 43–213, biên độ tối đa 85. | Điều chỉnh Master không bảo đảm giảm biên độ slip theo tỷ lệ ở vùng bão hòa. Bão hòa biên độ không tự làm sóng đứng ngang. |

Vị trí liên quan:

- [`validateTelemetry`](../get_telemetry/app_gui.cpp), các ngưỡng khoảng dòng 372–383.
- `telemetryWorker`, freshness 250 ms và `clearLiveTelemetry`, khoảng dòng 952–961.
- `readHapticIdentity`, khoảng dòng 136–170; `serialMonitorWorker`, so ID khoảng dòng 760.
- `sendDataToESP32`, gain khoảng dòng 311–320.
- [`calc_effect`](../haptic_firmware/haptic_firmware.ino), biên độ slip và tổng sóng khoảng dòng 234–254.

## Các đường mất hoặc giảm rung trên 1.03

| Điều kiện | Hành vi đã kiểm tra |
| --- | --- |
| Phanh <= 5% hoặc tốc độ <= 3 km/h | Slip và ABS bị tắt theo brake gate; road vẫn có đường xử lý riêng. |
| Frame không đổi sequence / bridge ngừng xuất bản | Sau hơn 250 ms, app đưa mọi hiệu ứng về 0. |
| Frame có sequence lẻ, sequence đầu/cuối khác nhau hoặc magic sai | Reader từ chối frame. Dữ liệu hợp lệ trở lại được chấp nhận. |
| Python không pack được float quá lớn | Callback báo lỗi và để sequence lẻ; dữ liệu hợp lệ ở callback sau phục hồi được. |
| UART không có telemetry hợp lệ hơn 500 ms | Firmware zero snapshot và tắt LED. `ID?` và dữ liệu lỗi không làm mới thời điểm telemetry hợp lệ. |
| UART chia dòng với khoảng nghỉ 5 ms | Frame hợp lệ được nhận. |
| UART chia dòng trước khi đủ năm trường với khoảng nghỉ 30 ms | Timeout đọc từng byte 20 ms làm mất frame; kéo dài thiếu frame hợp lệ sẽ kích hoạt watchdog. |
| Gói có thêm trường phía sau năm float | `sscanf` vẫn chấp nhận năm trường đầu. Đây là thiếu kiểm tra đuôi dòng; sender hiện gửi năm trường. |
| WriteFile thất bại / heartbeat quá hạn 5 giây | App yêu cầu disconnect; không cần có fatal panic. |
| Thông báo `Guru Meditation` | Monitor bật cờ fatal panic và dừng gửi. |
| Thông báo `Brownout detector was triggered` | Monitor không bật cờ fatal panic. Test này kiểm tra cách phân loại thông báo, không tạo sụt nguồn thật. |
| Slip cùng ABS/road | Allocation slip giảm từ 85 khi chạy riêng xuống 35 với ABS, 75 với road, 20 khi cả ba chạy. Cảm giác slip có thể yếu đi dù tổng tín hiệu vẫn dao động. |

## Kết quả test sóng firmware

Các khoảng DAC dưới đây lấy từ code `calc_effect` thực tế chạy trên PC, với input tối đa,
LUT sin và DAC register được mô phỏng. Mỗi cửa sổ 100 ms đều có dao động; không có
mẫu chạm 0 hoặc 255 trong những kịch bản này.

| Hiệu ứng | Thời gian mô phỏng | DAC min–max | RMS theo count DAC quanh 128 |
| --- | ---: | ---: | ---: |
| Slip | 900 giây | 43–213 | 60.0901 |
| ABS + slip | 60 giây | 3–252 | 55.0921 |
| Road + slip | 60 giây | 3–253 | 63.6162 |
| ABS + road + slip | 60 giây | 4–252 | 48.2933 |

1.000 lần chuyển qua tám trạng thái mixer cũng giữ DAC trong khoảng 1–254.
Seqlock firmware bị giữ lẻ tái sử dụng frame cuối, không quay vô hạn; khi sequence
chẵn trở lại thì nhận snapshot mới. Python sequence wrap cũng qua kiểm tra.

Regression 1.04 xác nhận Master 0%, 50%, 75%, 100% với slip bão hòa lần lượt cho
DAC 128–128, 85–170, 64–191, 43–213. Gain slip 50% cho DAC 85–170.
Toàn bộ worker AC cùng sender 60 Hz giữ rung với raw slip 11 và NdSlip 101;
NaN ở telemetry thiết yếu hoặc stream đứng vẫn kích hoạt fail-safe, rồi phục hồi.

## Build và phạm vi phần cứng

- Firmware biên dịch thành công bằng Arduino ESP32 core 3.3.11, FQBN `esp32:esp32:esp32`.
- Build 1.04: Flash 922.024 byte (70%); RAM tĩnh 50.236 byte (15%). Đây không phải phép đo heap/runtime.
- ELF đặt `calc_effect`, `mem_workaround` và timer wrapper trong IRAM; LUT và state trong DRAM.
- SDK được kiểm tra có `CONFIG_FREERTOS_FPU_IN_ISR=1`. Kết quả này thuộc build cục bộ, không xác minh binary đang trên board.
- Cài đặt đọc được: Master 75%, ABS 100%, Road 100%, TireSlip 100%.
- Khi chẩn đoán 1.03, app chạy từ `C:\Program Files\Haptic Brake Control`; SHA256 trùng EXE cũ trong repo. EXE mới 1.04 được build riêng để đóng gói.
- Không có tiến trình AC/ACC lúc kiểm tra. Các frame game là dữ liệu thử; các phép thử serial dùng file/named pipe, không chiếm COM của app đang chạy.
- Chưa đo nguồn 12 V, waveform GPIO25, đầu ra ampli, nhiệt độ hoặc thời gian ISR trên ESP32 thật.

TPA3116D2 có bảo vệ quá nhiệt, thấp áp, quá áp, DC và ngắn mạch. Vì firmware hiện
không đọc FAULTZ của ampli, ampli ngắt đầu ra có thể làm mất rung mà app vẫn nhận
telemetry bình thường. Đây là khả năng phần cứng cần đo, chưa được tái hiện trên bộ
của người dùng. Nguồn: [datasheet TI, phần Device Protection System / Thermal Protection](https://www.ti.com/lit/ds/symlink/tpa3116d2.pdf).

Nếu gặp lại: app chuyển Waiting thì ưu tiên đường telemetry; app mất Connected
thì ưu tiên serial/heartbeat; app còn telemetry hoạt động mà motor im thì cần đo
GPIO25 và nguồn/đầu ra ampli để phân biệt firmware với phần công suất.

## Chạy lại

```powershell
python testing/test_severe_slip.py
arduino-cli compile --fqbn esp32:esp32:esp32 haptic_firmware
```

Test yêu cầu Python và g++ đang dùng để build app. Kết quả `PASS` hiện xác nhận
các lỗi phần mềm đã sửa và các fail-safe được giữ. Test không flash firmware hoặc
điều khiển motor; chưa xác nhận nguyên nhân sự cố trên phần cứng của người dùng.
