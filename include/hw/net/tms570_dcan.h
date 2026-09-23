/* TMS570 DCAN, local OBC100 experiment. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_NET_TMS570_DCAN_H
#define HW_NET_TMS570_DCAN_H

#include "hw/core/sysbus.h"
#include "net/can_emu.h"
#include "qemu/timer.h"

#define TYPE_TMS570_DCAN "tms570-dcan"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570DcanState, TMS570_DCAN)

typedef struct TMS570DcanMessage {
    uint32_t mask, arb, control;
    uint32_t data[2];
} TMS570DcanMessage;

struct TMS570DcanState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[2];
    uint32_t reg[0x200 / 4];
    TMS570DcanMessage msg[128];
    CanBusState *canbus;
    CanBusClientState client;
    QEMUTimer *retry;
};
#endif
