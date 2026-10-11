"""End-to-end test; run explicitly because it boots the full firmware."""

import unittest
from pathlib import Path

import obcfi

ROOT = Path(__file__).resolve().parents[4]
QEMU = ROOT / "qemu/build/qemu-system-arm"
FIRMWARE = ROOT / "obc/build/TMS570LC43_OBC_PLUS_20240102.out"


class RealFirmwareTest(unittest.TestCase):
    def test_voltage_fault_reaches_firmware_and_recovers(self):
        with obcfi.Obc(FIRMWARE, QEMU) as obc:
            obc.wait_for_boot()
            obc.system.set_voltage("obc_1v2", 1.2)
            self.assertAlmostEqual(obc.wait_for_adc_value(1, 1.2), 1.2,
                                   delta=0.02)
            with obc.system.fault("obc_1v2", stuck_at=0.0):
                self.assertAlmostEqual(obc.wait_for_adc_value(1, 0.0), 0.0,
                                       delta=0.02)
            self.assertAlmostEqual(obc.wait_for_adc_value(1, 1.2), 1.2,
                                   delta=0.02)


if __name__ == "__main__":
    unittest.main()
