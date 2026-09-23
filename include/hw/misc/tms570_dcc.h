/*
 * TI TMS570 Dual Clock Comparator (DCC)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_TMS570_DCC_H
#define HW_MISC_TMS570_DCC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_TMS570_DCC "tms570-dcc"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570DccState, TMS570_DCC)

struct TMS570DccState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq err_irq;
    qemu_irq done_irq;

    uint32_t gctrl;
    uint32_t rev;
    uint32_t cnt0seed;
    uint32_t valid0seed;
    uint32_t cnt1seed;
    uint32_t stat;
    uint32_t cnt0;
    uint32_t valid0;
    uint32_t cnt1;
    uint32_t cnt1clksrc;
    uint32_t cnt0clksrc;
};

#endif /* HW_MISC_TMS570_DCC_H */
