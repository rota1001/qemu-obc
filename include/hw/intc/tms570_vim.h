/*
 * TI TMS570 Vectored Interrupt Manager (VIM)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_INTC_TMS570_VIM_H
#define HW_INTC_TMS570_VIM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_TMS570_VIM "tms570-vim"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570VimState, TMS570_VIM)

#define TMS570_VIM_NUM_CHANNELS 128
#define TMS570_VIM_NUM_WORDS    (TMS570_VIM_NUM_CHANNELS / 32)

/*
 * The vector table lives in its own RAM window at 0xFFF82000.  Entry 0 is the
 * phantom vector; channel N's handler is at entry N + 1.
 */
#define TMS570_VIM_RAM_ENTRIES  (TMS570_VIM_NUM_CHANNELS + 1)
#define TMS570_VIM_RAM_SIZE     0x1000

struct TMS570VimState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    MemoryRegion ram;

    qemu_irq irq;
    qemu_irq fiq;

    /* Level state of each incoming line, independent of INTREQ. */
    uint32_t raw[TMS570_VIM_NUM_WORDS];

    uint32_t firqpr[TMS570_VIM_NUM_WORDS];
    uint32_t intreq[TMS570_VIM_NUM_WORDS];
    uint32_t reqmask[TMS570_VIM_NUM_WORDS];
    uint32_t wakemask[TMS570_VIM_NUM_WORDS];
    uint32_t chanctrl[32];
    uint32_t capevt;
    uint32_t eccctl;
    uint32_t fbvecaddr;

    uint32_t vecram[TMS570_VIM_RAM_ENTRIES];
};

#endif /* HW_INTC_TMS570_VIM_H */
