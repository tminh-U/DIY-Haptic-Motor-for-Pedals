# Assetto Corsa Python telemetry app

This helper is required only for the original Assetto Corsa. Assetto Corsa
Competizione is supported directly through shared memory and does not use a
Python app.

Copy the `haptic_telemetry` directory into:

```text
<Assetto Corsa>\apps\python\haptic_telemetry
```

Then enable **Haptic Telemetry** in Assetto Corsa or Content Manager under
Python Apps. The app reads `SlipRatio`, `NdSlip`, and `SuspensionTravel` from
Assetto Corsa's in-game Python API and publishes a versioned `HPT1` frame to
the private `haptic_telemetry_v1` memory map on every `acUpdate()` callback.
The callback rate is controlled by Assetto Corsa and can differ from rendered
FPS. The PC host independently sends the newest snapshot to the ESP32 at 60 Hz.

The memory map is local to the Windows session and is read by
`get_telemetry.exe`; no network socket is opened.
