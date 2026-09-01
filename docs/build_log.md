
# 16/8/2026

![items](../Image/1682026-arrived_items.png)


**Today received items** :
- ESP32-DEVKIT-V1
- TPA3116D2 (XH-M542)
- DC jack (female)
- Sound exciter extended cable
- Micro-usb cable
- D-point cables

## Today works :



![items](../Image/1682026-connecting_board.png)

- Connecting ESP32 and the amply through D-point cables
- Connecting the female DC jack with TPA3116V2

# 18/8/2026

**Today received items** :
- DC 12V - 3A cable
- 40 mm bass transducer
- Amplifier case
- Wire connector


## Today works :

### Connected all of the components.
![items](../Image/1882026-complete_components.jpg)


### Testing 

#### Continuous sine-AM ABS prototype

The first ABS waveform used a 12 Hz sine envelope to modulate a 60 Hz carrier, with brake-pedal pressure controlling the amplitude:

$$y_{\text{carrier}}(t) = \sin(2\pi \cdot 60 \cdot t)$$

$$y_{\text{mod}}(t) = \sin(2\pi \cdot 12 \cdot t)$$

$$y(t) = \left( \frac{1 + \sin(2\pi \cdot 12 \cdot t)}{2} \right) \cdot \sin(2\pi \cdot 60 \cdot t)$$

$$x_{\text{ABS}}(t) = (120 \cdot \text{brakeVal}) \cdot y(t)$$

`brakeVal` is the normalized brake-pedal pressure ($0.0 \le \text{brakeVal} \le 1.0$), and $x_{\text{ABS}}(t)$ is the zero-centered ABS effect sample.

- Test video: [ABS with amplitude modulation (60 Hz carrier / 12 Hz envelope)](https://youtube.com/shorts/pjtzisKoqhs?feature=share)
- Result: Weak intensity even at maximum amplitude because of the sound exciter's physical limitations.
- Decision: Replace the continuous sine envelope with 12 Hz square-wave pulse gating to produce sharper pedal kicks. The current implementation is documented in [Design rationale and control logic](design.md#effect-calculations).


# 21/8/2026

**Today received items** : Nothing yet
## Today works :

### Completed the ESP32 firmware

### Testing all effects

- Demo video (ABS) : [Here](https://youtube.com/shorts/HPPclNWR_eQ)
- Demo video (slip effect) : [Here](https://youtube.com/shorts/F_2Ib3Tkiu4?feature=share)
- Demo video (road effect) : [Here](https://youtube.com/shorts/F_2Ib3Tkiu4?feature=share)


# 22/8/2026

**Today received items** : Nothing yet
## Today works :

### Refined ABS telemetry handling

- Assetto Corsa: Derive ABS activation from the Python API's front longitudinal slip ratios. Use the shared-memory `abs` field only as an availability hint and, when plausible, as the slip threshold.
- Assetto Corsa Competizione: Use the native shared-memory `abs` intervention signal directly; do not use the unpopulated `absInAction` compatibility field.
- Normalize both paths to the same boolean `absVal` field before serial transmission to the ESP32.



