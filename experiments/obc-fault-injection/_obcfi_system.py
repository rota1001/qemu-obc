"""Private physical-input conversions used by :mod:`obcfi`."""

from __future__ import annotations

import math
from contextlib import contextmanager
from dataclasses import dataclass

ADC_MAX = 4095
ADC_REFERENCE_VOLTS = 3.44


@dataclass(frozen=True)
class Result:
    """One applied input: physical request, sensor observation, and ADC code."""
    name: str
    unit: str
    requested: float
    observed: float
    raw: int
    clipped: bool


# name: (kind, unit, ADC unit, channel, conversion, normal value)
_INPUTS = {
    "obc_board": ("temperature", "degC", 1, 0, "thermistor", 25.0),
    "obc_1v2": ("voltage", "V", 1, 1, "direct", 1.2),
    "obc_3v3_sys": ("voltage", "V", 1, 2, "direct", 3.3),
    "obc_3v3_adc": ("voltage", "V", 1, 3, "direct", 3.3),
    "obc_ext_3v3_in": ("voltage", "V", 1, 4, "direct", 3.3),
    "obc_ext_3v3_out": ("voltage", "V", 1, 5, "direct", 0.0),
    "obc_5v_current": ("current", "A", 1, 7, "current", 0.1),
    "obc_ext_5v_current": ("current", "A", 1, 8, "current", 0.0),
    "obc_5v_in": ("voltage", "V", 1, 9, "divide2", 5.0),
    "obc_5v_out": ("voltage", "V", 1, 10, "divide2", 5.0),
}


def _raw(value: float, conversion: str) -> tuple[int, bool]:
    """Convert a physical value into the voltage sampled by the 12-bit ADC."""
    if not math.isfinite(value):
        raise ValueError("physical values must be finite")
    if conversion == "thermistor":
        kelvin = value + 273.15
        if kelvin <= 0:
            raise ValueError("temperature must be above absolute zero")
        ratio = math.exp(3435.0 * (1.0 / kelvin - 0.003354))
        ideal = ADC_MAX * ratio / (1.0 + ratio)
    else:
        if value < 0:
            raise ValueError("voltage and current cannot be negative")
        pin_voltage = {
            "direct": value,
            "divide2": value / 2.0,
            # Current monitor: 50 V/V amplifier and 10 milliohm shunt.
            "current": value * 50.0 * 0.01,
        }[conversion]
        ideal = pin_voltage / ADC_REFERENCE_VOLTS * ADC_MAX
    rounded = round(ideal)
    return min(max(rounded, 0), ADC_MAX), not 0 <= rounded <= ADC_MAX


class System:
    """Physical state presented to the firmware through board sensors."""

    def __init__(self, set_adc):
        # set_adc(unit, channel, raw) is supplied privately by Obc.
        self._set_adc = set_adc
        self._values = {name: spec[5] for name, spec in _INPUTS.items()}
        self._faults: dict[str, tuple[float | None, float, float]] = {}

    def initialize(self) -> None:
        """Write all normal board values to QEMU."""
        for name, value in self._values.items():
            self._apply(name, value)

    def set_voltage(self, name: str, volts: float) -> Result:
        """Set a named rail voltage in volts, for example ``obc_1v2``."""
        return self._set(name, "voltage", volts)

    def set_current(self, name: str, amperes: float) -> Result:
        """Set a named load current in amperes."""
        aliases = {"obc_5v": "obc_5v_current", "obc_ext_5v": "obc_ext_5v_current"}
        return self._set(aliases.get(name, name), "current", amperes)

    def set_temperature(self, name: str, celsius: float) -> Result:
        """Set a named physical temperature in degrees Celsius."""
        return self._set(name, "temperature", celsius)

    def value(self, name: str) -> float:
        """Return the true physical value, before sensor faults."""
        self._spec(name)
        return self._values[name]

    def _spec(self, name: str):
        try:
            return _INPUTS[name]
        except KeyError as exc:
            raise ValueError(f"unknown system input: {name}") from exc

    def _set(self, name: str, expected_kind: str, value: float) -> Result:
        spec = self._spec(name)
        if spec[0] != expected_kind:
            raise ValueError(f"{name} is {spec[0]}, not {expected_kind}")
        value = float(value)
        _raw(value, spec[4])  # Validate the true value even with a stuck sensor.
        result = self._apply(name, value)
        self._values[name] = value  # Commit only after QEMU accepted the update.
        return result

    def _apply(self, name: str, requested: float) -> Result:
        _kind, unit_name, adc_unit, channel, conversion, _normal = self._spec(name)
        stuck, offset, gain = self._faults.get(name, (None, 0.0, 1.0))
        observed = stuck if stuck is not None else requested * gain + offset
        raw, clipped = _raw(observed, conversion)
        self._set_adc(adc_unit, channel, raw)
        return Result(name, unit_name, requested, observed, raw, clipped)

    @contextmanager
    def fault(self, name: str, *, stuck_at: float | None = None,
              offset: float = 0.0, gain: float = 1.0):
        """Temporarily corrupt a sensor while preserving true physical state.

        ``stuck_at`` fixes the observed physical value. ``offset`` and ``gain``
        model sensor bias and scale errors. Previous behavior is restored when
        the context exits, including after an exception.
        """
        self._spec(name)
        previous = self._faults.get(name)
        self._faults[name] = (stuck_at, float(offset), float(gain))
        try:
            result = self._apply(name, self._values[name])
            yield result
        finally:
            if previous is None:
                self._faults.pop(name, None)
            else:
                self._faults[name] = previous
            self._apply(name, self._values[name])
