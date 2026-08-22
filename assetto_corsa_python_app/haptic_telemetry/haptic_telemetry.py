import mmap
import struct

import ac
import acsys


SHARED_MEMORY_NAME = "haptic_telemetry_v1"
SHARED_MEMORY_SIZE = 44

_app = 0
_status_label = 0
_shared_memory = None
_sequence = 0
_last_error = ""
_status_elapsed = 0.0
_status_frames = 0


def _wheel_values(value):
    if value is None or len(value) < 4:
        raise ValueError("Assetto Corsa returned incomplete wheel telemetry")
    return (float(value[0]), float(value[1]), float(value[2]), float(value[3]))


def acMain(ac_version):
    global _app, _status_label, _shared_memory

    _app = ac.newApp("Haptic Telemetry")
    ac.setSize(_app, 260, 70)
    _status_label = ac.addLabel(_app, "Shared telemetry - starting")
    ac.setPosition(_status_label, 10, 32)

    _shared_memory = mmap.mmap(0, SHARED_MEMORY_SIZE, SHARED_MEMORY_NAME)
    _shared_memory[0:4] = b"HPT1"
    _shared_memory[4:44] = b"\x00" * 40
    ac.log("[Haptic Telemetry] shared telemetry ready")
    return "Haptic Telemetry"


def acUpdate(delta_t):
    global _sequence, _last_error, _status_elapsed, _status_frames

    try:
        brake = float(ac.getCarState(0, acsys.CS.Brake))
        speed_kmh = float(ac.getCarState(0, acsys.CS.SpeedKMH))
        slip_ratio = _wheel_values(ac.getCarState(0, acsys.CS.SlipRatio))
        nd_slip = _wheel_values(ac.getCarState(0, acsys.CS.NdSlip))
        suspension = _wheel_values(ac.getCarState(0, acsys.CS.SuspensionTravel))

        odd_sequence = (_sequence + 1) & 0xffffffff
        if odd_sequence == 0:
            odd_sequence = 1
        even_sequence = (odd_sequence + 1) & 0xffffffff
        if even_sequence == 0:
            even_sequence = 2

        # Seqlock publication: the C++ reader accepts a frame only when both
        # sequence values match and are even.
        _shared_memory[4:8] = struct.pack("<I", odd_sequence)
        _shared_memory[8:40] = struct.pack(
            "<8f",
            brake,
            speed_kmh,
            slip_ratio[0],
            slip_ratio[1],
            nd_slip[0],
            nd_slip[1],
            suspension[0],
            suspension[1],
        )
        _shared_memory[40:44] = struct.pack("<I", even_sequence)
        _shared_memory[4:8] = struct.pack("<I", even_sequence)
        _sequence = even_sequence

        if _last_error:
            _last_error = ""
            ac.log("[Haptic Telemetry] stream recovered")

        # Updating an AC UI label for every telemetry frame is surprisingly
        # expensive. Publish on every acUpdate callback, but refresh this
        # diagnostic text only once per second.
        _status_elapsed += delta_t
        _status_frames += 1
        if _status_elapsed >= 1.0:
            measured_hz = int(round(_status_frames / _status_elapsed))
            ac.setText(_status_label, "Publishing at %d Hz" % measured_hz)
            _status_elapsed = _status_elapsed % 1.0
            _status_frames = 0
    except Exception as error:
        message = str(error)
        ac.setText(_status_label, "Telemetry error - check log")
        if message != _last_error:
            _last_error = message
            ac.log("[Haptic Telemetry] " + message)


def acShutdown():
    global _shared_memory
    if _shared_memory is not None:
        try:
            _shared_memory.close()
        except Exception:
            pass
        _shared_memory = None
