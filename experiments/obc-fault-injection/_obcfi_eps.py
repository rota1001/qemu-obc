"""EPS physical inputs and I2C packet faults (units are volts)."""
import math


class Eps:
    def __init__(self, qmp):
        self._qmp = qmp
        self._path = qmp.device('obc-eps')

    @property
    def battery_voltage(self):
        """Synthetic pack voltage in volts, shared by all three model batteries."""
        return self._qmp.get(self._path, 'input-battery-mv') / 1000

    @battery_voltage.setter
    def battery_voltage(self, volts):
        value = float(volts)
        if not math.isfinite(value) or not 0 <= value <= 65.535:
            raise ValueError('EPS battery voltage must be 0..65.535 V')
        self._qmp.set(self._path, 'input-battery-mv', round(value * 1000))

    def disconnected(self):
        """Context: EPS NACKs its I2C address; restore connectivity on exit."""
        return self._qmp.override(self._path, 'disconnected', 1)

    def bad_checksum(self):
        """Context: corrupt the XOR checksum of newly generated EPS packets."""
        return self._qmp.override(self._path, 'bad-checksum', 1)
