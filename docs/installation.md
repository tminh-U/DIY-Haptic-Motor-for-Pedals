# Installation guide


## 1. Assemble the hardware

See [Hardware guide](hardware.md).

# 2. Install the app

- Get `HapticBrakeControl_Setup.exe` from [Github Releases](https://github.com/tminh-U/DIY-Haptic-Motor-for-Pedals/releases).
- Install the app

For **1.04**, update the ESP32 firmware and replace the AC Python app folder below
as well. The corrected volume behavior requires firmware 1.04; the host retains
legacy output for older firmware.

The app checks GitHub Releases once in the background when it starts. In
**Settings > App updates**, view the installed/latest version or select
**Check for updates** to retry. An amber dot on Settings indicates an available
app update. **Download update** opens the official installer in your browser;
run it to install the update. Firmware updates remain in the Firmware tab.
Update checks use the [GitHub Releases API](https://docs.github.com/en/rest/releases/releases#get-the-latest-release).

## 2. Flash the ESP32 firmware

- Open `Haptic Brake Control`.
- Connect the ESP32 to the computer through USB.
- Open `Firmware` tab.
- Select `Manual flash`.
- Select ESP32 COM port and 


## 3. Install the communicate application
- Open `Haptic Brake Control`.
- The host application automatically detects whether Assetto Corsa or Assetto Corsa Competizione is running, so no manual game selection is required.

## 4. Configure the game

For **Assetto Corsa**:

1. Copy `assetto_corsa_python_app/haptic_telemetry` to `<Assetto Corsa>/apps/python/haptic_telemetry`.
2. Enable **Haptic Telemetry** in Assetto Corsa or Content Manager's Python Apps settings.

For **Assetto Corsa Competizione**, no Python app or UDP configuration is required.
## 5. Start the system

- Connect the ESP32 to the PC through USB
- Open `get_telemetry.exe` and click `CONNECT` in the `ESP32 + Simulator Connection` tab.
- Open the game and start a driving session.
- Enjoy!


## Verify the installation

| Status | Expected value |
|---|---|
| ESP32 LED | LED ON while valid packets are received |

