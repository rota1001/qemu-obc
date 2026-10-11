import unittest

from _obcfi_system import System


class SystemTest(unittest.TestCase):
    def setUp(self):
        self.writes = []
        self.system = System(lambda unit, channel, raw:
                             self.writes.append((unit, channel, raw)))

    def test_physical_voltage_is_converted_to_adc(self):
        result = self.system.set_voltage("obc_1v2", 1.2)
        self.assertEqual(result.raw, 1428)
        self.assertEqual(self.writes[-1], (1, 1, 1428))

    def test_five_volt_divider(self):
        self.assertEqual(self.system.set_voltage("obc_5v_in", 5.0).raw, 2976)

    def test_current_monitor(self):
        self.assertEqual(self.system.set_current("obc_5v", 0.2).raw, 119)

    def test_sensor_fault_preserves_true_value_and_restores(self):
        self.system.set_voltage("obc_1v2", 1.2)
        with self.system.fault("obc_1v2", stuck_at=0.0) as result:
            self.assertEqual(result.observed, 0.0)
            self.assertEqual(result.raw, 0)
            self.assertEqual(self.system.value("obc_1v2"), 1.2)
        self.assertEqual(self.writes[-1], (1, 1, 1428))

    def test_wrong_parameter_kind_is_rejected(self):
        with self.assertRaises(ValueError):
            self.system.set_current("obc_1v2", 0.2)


if __name__ == "__main__":
    unittest.main()
