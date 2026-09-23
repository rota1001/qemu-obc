/*
 * TI TMS570 SCI / LIN in SCI compatibility mode
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_CHAR_TMS570_SCI_H
#define HW_CHAR_TMS570_SCI_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_TMS570_SCI "tms570-sci"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570SciState, TMS570_SCI)

struct TMS570SciState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharFrontend chr;

    /* Level 0 ("high") and level 1 ("low") interrupt lines into the VIM. */
    qemu_irq irq_high;
    qemu_irq irq_low;

    uint32_t gcr0;
    uint32_t gcr1;
    uint32_t gcr2;
    uint32_t setint;    /* interrupt enables */
    uint32_t setintlvl; /* 1 = route to the level 0 line */
    uint32_t flr;
    uint32_t format;
    uint32_t brs;
    uint32_t ed;
    uint32_t rd;
    uint32_t td;
    uint32_t pio[9];
    uint32_t iodftctrl;

    bool rx_full;
};

#endif /* HW_CHAR_TMS570_SCI_H */
