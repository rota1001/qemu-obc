/*
 * TI TMS570 Error Signaling Module (ESM)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_TMS570_ESM_H
#define HW_MISC_TMS570_ESM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_TMS570_ESM "tms570-esm"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570EsmState, TMS570_ESM)

#define TMS570_ESM_NUM_GROUPS 3   /* groups 1, 2 and 3 */

struct TMS570EsmState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq_high;
    qemu_irq irq_low;

    /* Group 1 is maskable and has a level select; groups 2 and 3 are not. */
    uint32_t sr[TMS570_ESM_NUM_GROUPS];
    uint32_t ier;     /* group 1 interrupt enable */
    uint32_t ilr;     /* group 1 interrupt level: 1 = high (FIQ) */
    uint32_t epapr;
    uint32_t upper_sr[2];
    uint32_t upper_ier[2];
    uint32_t upper_ilr[2];
    uint32_t upper_epapr[2];
    bool error_pin;
    uint32_t ltcr;
    uint32_t ltcpr;
    uint32_t ekr;
    uint32_t ssr2;
};

#endif /* HW_MISC_TMS570_ESM_H */
