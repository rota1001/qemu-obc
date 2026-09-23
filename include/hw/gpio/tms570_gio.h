/*
 * TI TMS570 General purpose I/O (GIO)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_GPIO_TMS570_GIO_H
#define HW_GPIO_TMS570_GIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_TMS570_GIO "tms570-gio"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570GioState, TMS570_GIO)

#define TMS570_GIO_NUM_PORTS 2   /* PORTA at 0x34, PORTB at 0x54 */
#define TMS570_GIO_PORT_PINS 8

typedef struct TMS570GioPort {
    uint32_t dir;
    uint32_t din;
    uint32_t driven;
    uint32_t dout;
    uint32_t pdr;
    uint32_t puldis;
    uint32_t psl;
} TMS570GioPort;

struct TMS570GioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq_high;
    qemu_irq irq_low;
    qemu_irq out[TMS570_GIO_NUM_PORTS * TMS570_GIO_PORT_PINS];

    uint32_t gcr0;
    uint32_t intdet;
    uint32_t pol;
    uint32_t ena;
    uint32_t lvl;
    uint32_t flg;
    uint32_t emu1;
    uint32_t emu2;

    TMS570GioPort port[TMS570_GIO_NUM_PORTS];
};

#endif /* HW_GPIO_TMS570_GIO_H */
