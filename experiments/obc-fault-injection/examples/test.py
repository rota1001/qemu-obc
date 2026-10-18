import obcfi
import time

with obcfi.Obc(...) as obc:
    obc.wait_for_boot()
    print(obc.command("RTC_DATE"))
    time.sleep(3)
    print(obc.command("RTC_DATE"))
    with obc.rtc.frozen():
        print(obc.command("RTC_DATE"))
        time.sleep(3)
        print(obc.command("RTC_DATE"))

# RTC Time: 2026-01-01 00:00:00  GMT+8

# RTC Time: 2026-01-01 00:00:03  GMT+8

# RTC Time: 2026-01-01 00:00:03  GMT+8

# RTC Time: 2026-01-01 00:00:03  GMT+8