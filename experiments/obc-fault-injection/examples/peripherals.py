"""Run with PYTHONPATH=. python3 examples/peripherals.py (no hardware access)."""
from pathlib import Path
import obcfi

ROOT = Path(__file__).resolve().parents[4]

with obcfi.Obc(
    ROOT / 'obc/build/TMS570LC43_OBC_PLUS_20240102.out',
    ROOT / 'qemu/build/qemu-system-arm',
) as obc:
    obc.wait_for_boot()
    print('QEMU:', obc.status())

    # Volts at the EPS pack, converted to millivolts in the I2C packet.
    obc.eps.battery_voltage = 12.0
    print(obc.command('EPS_ANALOG_HK_DATA'))

    # Each context restores the device even if an assertion raises.
    with obc.eps.disconnected():
        print(obc.command('EPS_ANALOG_HK_DATA'))  # i2c=-3
    with obc.eps.bad_checksum():
        print(obc.command('EPS_CONFIG'))          # checksum mismatch
    print(obc.command('EPS_CONFIG'))              # valid again

    # RTC_DATE reads the chip; date displays a separately maintained OS clock.
    obc.rtc.shift_seconds = 86400
    print(obc.command('RTC_DATE'))
    with obc.rtc.frozen():
        print(obc.command('RTC_DATE'))
    obc.rtc.shift_seconds = 0
    print(obc.command('RTC_DATE'))
