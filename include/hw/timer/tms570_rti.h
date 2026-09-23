/*
 * TI TMS570 Real Time Interrupt module (RTI), including the digital watchdog
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_TMS570_RTI_H
#define HW_TIMER_TMS570_RTI_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_TMS570_RTI "tms570-rti"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570RtiState, TMS570_RTI)

#define TMS570_RTI_NUM_COUNTERS 2
#define TMS570_RTI_NUM_COMPARES 4

typedef struct TMS570RtiCounter {
    uint32_t frc;      /* free running counter */
    uint32_t uc;       /* up counter (prescaler) */
    uint32_t cpuc;     /* prescaler compare */
    int64_t  base_ns;  /* host time the counter last started from zero */
    bool     running;
} TMS570RtiCounter;

struct TMS570RtiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* One line per compare, then overflow 0/1 -- VIM channels 2..8. */
    qemu_irq cmp_irq[TMS570_RTI_NUM_COMPARES];

    QEMUTimer *timer;

    uint32_t clk_hz;   /* RTI counter input clock */

    uint32_t gctrl;
    uint32_t tbctrl;
    uint32_t capctrl;
    uint32_t compctrl;
    uint32_t setintena;
    uint32_t intflag;

    TMS570RtiCounter cnt[TMS570_RTI_NUM_COUNTERS];
    uint32_t comp[TMS570_RTI_NUM_COMPARES];
    uint32_t udcp[TMS570_RTI_NUM_COMPARES];
    /* Value of the driving counter at which each compare next fires. */
    uint32_t next[TMS570_RTI_NUM_COMPARES];

    /* Digital watchdog. */
    uint32_t dwdctrl;
    uint32_t dwdprld;
    uint32_t wdstatus;
    uint32_t wdkey;
    int64_t  dwd_expire_ns;
    bool     dwd_running;
    QEMUTimer *dwd_timer;
    qemu_irq watchdog_reset;
};

#endif /* HW_TIMER_TMS570_RTI_H */
