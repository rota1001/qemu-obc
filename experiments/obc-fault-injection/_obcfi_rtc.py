"""RTC time faults independent of the firmware's cached system time."""


class Rtc:
    def __init__(self, qmp):
        self._qmp = qmp
        self._path = qmp.device('obc-rtc')

    @property
    def shift_seconds(self):
        """Absolute injected offset, in seconds, relative to the RTC's own time."""
        return self._qmp.get(self._path, 'time-shift-seconds')

    @shift_seconds.setter
    def shift_seconds(self, seconds):
        if isinstance(seconds, bool) or not isinstance(seconds, int):
            raise ValueError('RTC shift must be an integer number of seconds')
        if abs(seconds) > 315360000:
            raise ValueError('RTC shift must be within ten years')
        self._qmp.set(self._path, 'time-shift-seconds', seconds)

    def frozen(self):
        """Context: stop the RTC; on exit resume from its frozen time."""
        return self._qmp.override(self._path, 'frozen', True)
