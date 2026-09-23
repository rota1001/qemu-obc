/*
 * TI TMS570 system module (primary and secondary frames)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_TMS570_SYSTEM_H
#define HW_MISC_TMS570_SYSTEM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_TMS570_SYSTEM "tms570-system"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570SystemState, TMS570_SYSTEM)

/* SYSESR bits, as decoded by obc_reset_info_capture(). */
#define TMS570_SYSESR_PORST     (1u << 15)
#define TMS570_SYSESR_OSCRST    (1u << 14)
#define TMS570_SYSESR_WDRST     (1u << 13)
#define TMS570_SYSESR_CPURST    (1u << 5)
#define TMS570_SYSESR_SWRST     (1u << 4)
#define TMS570_SYSESR_EXTRST    (1u << 3)

struct TMS570SystemState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem1;
    MemoryRegion iomem2;

    /* Software interrupts SSIR1..4 drive VIM channels 21..24. */
    qemu_irq ssi_irq[4];

    uint32_t reset_source; /* value latched into SYSESR at reset */
    uint32_t pending_reset_source;

    uint32_t syspc[10];
    uint32_t csdis;
    uint32_t cddis;
    uint32_t ghvsrc;
    uint32_t vclkasrc;
    uint32_t rclksrc;
    uint32_t pllctl1;
    uint32_t pllctl2;
    uint32_t pllctl3;
    uint32_t mstgcr;
    uint32_t minitgcr;
    uint32_t msinena;
    uint32_t clkcntl;
    uint32_t ecpcntl;
    uint32_t ramgcr;
    uint32_t bmmcr1;
    uint32_t cpurstcr;
    uint32_t sysecr;
    uint32_t sysesr;
    uint32_t gblstat;
    uint32_t ssif;
    uint32_t ssivec;
    uint32_t gpreg1;
    uint32_t lpomonctl;
    uint32_t clktest;
    uint32_t devcr1;
    uint32_t clk2cntrl;
    uint32_t hclkcntl;
    uint32_t vclkacon1;
    uint32_t ecpcntrl1;
};

#endif /* HW_MISC_TMS570_SYSTEM_H */
