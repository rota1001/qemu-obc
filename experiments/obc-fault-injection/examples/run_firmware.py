"""Boot the real firmware and inject a stuck-low 1.2 V monitor."""

from pathlib import Path

import obcfi

ROOT = Path(__file__).resolve().parents[4]

with obcfi.Obc(
    firmware=ROOT / "obc/build/TMS570LC43_OBC_PLUS_20240102.out",
    qemu=ROOT / "qemu/build/qemu-system-arm",
) as obc:
    obc.wait_for_boot()
    print(obc.command("date"))
    obc.system.set_voltage("obc_1v2", 1.2)
    obc.interactive()
    # print("normal:", obc.wait_for_adc_value(1, 1.2))
    # with obc.system.fault("obc_1v2", stuck_at=0.0):
    #     print("fault observed by firmware:", obc.wait_for_adc_value(1, 0.0))
    # print("restored:", obc.wait_for_adc_value(1, 1.2))
