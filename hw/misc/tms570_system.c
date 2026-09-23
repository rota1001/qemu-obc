/*
 * TI TMS570 system module (SYS and SYS2 frames)
 *
 * Only enough behaviour to get the HALCoGen startup code through systemInit():
 *
 *  - Clock sources report themselves valid as soon as they are enabled, so the
 *    "wait for PLL lock" loops in setupPLL()/trimLPO() terminate.  A model that
 *    leaves CSVSTAT at zero hangs the guest forever.
 *  - SYSESR latches a reset cause the board can choose, which is what
 *    obc_reset_info_capture() reads on the way into main().
 *  - Writing the key 0x75 to SSIR1..4 raises VIM channels 21..24.  FreeRTOS
 *    uses SSIR1 for portYIELD_WITHIN_API(), so without this taskYIELD() is a
 *    no-op and the scheduler never switches.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "system/runstate.h"
#include "hw/core/registerfields.h"
#include "hw/misc/tms570_system.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

/* Primary frame (0xFFFFFF00). */
REG32(SYSPC1,     0x000)
REG32(CSDIS,      0x030)
REG32(CSDISSET,   0x034)
REG32(CSDISCLR,   0x038)
REG32(CDDIS,      0x03C)
REG32(CDDISSET,   0x040)
REG32(CDDISCLR,   0x044)
REG32(GHVSRC,     0x048)
REG32(VCLKASRC,   0x04C)
REG32(RCLKSRC,    0x050)
REG32(CSVSTAT,    0x054)
REG32(MSTGCR,     0x058)
REG32(MINITGCR,   0x05C)
REG32(MSINENA,    0x060)
REG32(MSTFAIL,    0x064)
REG32(MSTCGSTAT,  0x068)
REG32(MINISTAT,   0x06C)
REG32(PLLCTL1,    0x070)
REG32(PLLCTL2,    0x074)
REG32(SYSPC10,    0x078)
REG32(DIEIDL,     0x07C)
REG32(DIEIDH,     0x080)
REG32(LPOMONCTL,  0x088)
REG32(CLKTEST,    0x08C)
REG32(GPREG1,     0x0A0)
REG32(SSIR1,      0x0B0)
REG32(SSIR2,      0x0B4)
REG32(SSIR3,      0x0B8)
REG32(SSIR4,      0x0BC)
REG32(RAMGCR,     0x0C0)
REG32(BMMCR1,     0x0C4)
REG32(CPURSTCR,   0x0CC)
REG32(CLKCNTL,    0x0D0)
REG32(ECPCNTL,    0x0D4)
REG32(DEVCR1,     0x0DC)
REG32(SYSECR,     0x0E0)
REG32(SYSESR,     0x0E4)
REG32(SYSTASR,    0x0E8)
REG32(GBLSTAT,    0x0EC)
REG32(DEVID,      0x0F0)
REG32(SSIVEC,     0x0F4)
REG32(SSIF,       0x0F8)

/* Secondary frame (0xFFFFE100). */
REG32(PLLCTL3,    0x000)
REG32(STCCLKDIV,  0x008)
REG32(ECPCNTRL1,  0x028)
REG32(CLK2CNTRL,  0x03C)
REG32(HCLKCNTL,   0x054)
REG32(VCLKACON1,  0x040)

#define SSI_KEY         0x75
#define SSI_KEY_SHIFT   8

/*
 * CSVSTAT reports which clock sources are valid.  Real silicon needs
 * (127 + 1024 * NR) oscillator cycles for a PLL to lock; here a source is
 * valid the moment it is not disabled, which is what the polling loops want.
 */
static void sys_update_csvstat(TMS570SystemState *s)
{
    /* Nothing to compute: CSVSTAT is derived on read from CSDIS. */
}

static uint64_t sys1_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570SystemState *s = TMS570_SYSTEM(opaque);

    switch (addr) {
    case A_CSDIS:
    case A_CSDISSET:
    case A_CSDISCLR:
        return s->csdis;
    case A_CDDIS:
    case A_CDDISSET:
    case A_CDDISCLR:
        return s->cddis;
    case A_CSVSTAT:
        /* Every enabled source is valid. */
        return (~s->csdis) & 0xFFu;
    case A_GHVSRC:
        return s->ghvsrc;
    case A_VCLKASRC:
        return s->vclkasrc;
    case A_RCLKSRC:
        return s->rclksrc;
    case A_PLLCTL1:
        return s->pllctl1;
    case A_PLLCTL2:
        return s->pllctl2;
    case A_MSTGCR:
        return s->mstgcr;
    case A_MINITGCR:
        return s->minitgcr;
    case A_MSINENA:
        return s->msinena;
    case A_MSTFAIL:
        return 0;
    case A_MSTCGSTAT:
        /*
         * MSTDONE (bit 0) and MINIDONE (bit 8).  _memInit_ spins on MINIDONE,
         * so both have to read back as complete immediately.
         */
        return 0x101;
    case A_MINISTAT:
        return 0;
    case A_CLKCNTL:
        return s->clkcntl;
    case A_ECPCNTL:
        return s->ecpcntl;
    case A_RAMGCR:
        return s->ramgcr;
    case A_BMMCR1:
        return s->bmmcr1;
    case A_CPURSTCR:
        return s->cpurstcr;
    case A_SYSECR:
        return s->sysecr;
    case A_SYSESR:
        return s->sysesr;
    case A_SYSTASR:
        return 0;
    case A_GBLSTAT:
        return s->gblstat;
    case A_DEVID:
        /* TMS570LC4357 */
        return 0x802AAD05;
    case A_DIEIDL:
    case A_DIEIDH:
        return 0;
    case A_SSIVEC: {
        uint32_t vec = s->ssivec;
        unsigned source = (vec >> 6) & 3;

        /* The FreeRTOS SSI handler acknowledges by reading SSIVEC. */
        s->ssif &= ~(1u << source);
        qemu_set_irq(s->ssi_irq[source], 0);
        return vec;
    }
    case A_SSIF:
        return s->ssif;
    case A_GPREG1:
        return s->gpreg1;
    case A_LPOMONCTL:
        return s->lpomonctl;
    case A_CLKTEST:
        return s->clktest;
    case A_DEVCR1:
        return s->devcr1;
    case A_SSIR1 ... A_SSIR4 + 3:
        return 0;
    default:
        break;
    }

    if (addr < 0x24) {
        return s->syspc[addr / 4];
    }
    if (addr == A_SYSPC10) {
        return s->syspc[9];
    }

    qemu_log_mask(LOG_UNIMP, "tms570-system: read from unhandled SYS1 0x%"
                  HWADDR_PRIx "\n", addr);
    return 0;
}

static void sys1_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TMS570SystemState *s = TMS570_SYSTEM(opaque);
    uint32_t v = val;
    int ssi;

    switch (addr) {
    case A_CSDIS:
        s->csdis = v & 0xFFu;
        sys_update_csvstat(s);
        return;
    case A_CSDISSET:
        s->csdis |= v & 0xFFu;
        sys_update_csvstat(s);
        return;
    case A_CSDISCLR:
        s->csdis &= ~(v & 0xFFu);
        sys_update_csvstat(s);
        return;
    case A_CDDIS:
        s->cddis = v;
        return;
    case A_CDDISSET:
        s->cddis |= v;
        return;
    case A_CDDISCLR:
        s->cddis &= ~v;
        return;
    case A_GHVSRC:
        s->ghvsrc = v;
        return;
    case A_VCLKASRC:
        s->vclkasrc = v;
        return;
    case A_RCLKSRC:
        s->rclksrc = v;
        return;
    case A_PLLCTL1:
        s->pllctl1 = v;
        return;
    case A_PLLCTL2:
        s->pllctl2 = v;
        return;
    case A_MSTGCR:
        s->mstgcr = v;
        return;
    case A_MINITGCR:
        s->minitgcr = v;
        return;
    case A_MSINENA:
        s->msinena = v;
        return;
    case A_CLKCNTL:
        s->clkcntl = v;
        return;
    case A_ECPCNTL:
        s->ecpcntl = v;
        return;
    case A_RAMGCR:
        s->ramgcr = v;
        return;
    case A_BMMCR1:
        s->bmmcr1 = v;
        return;
    case A_CPURSTCR:
        s->cpurstcr = v;
        return;
    case A_SYSECR:
        s->sysecr = v;
        if ((v & 0xc000) == 0x8000) {
            s->pending_reset_source = TMS570_SYSESR_SWRST | TMS570_SYSESR_EXTRST;
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        return;
    case A_SYSESR:
        /* Write one to clear. */
        s->sysesr &= ~v;
        return;
    case A_GBLSTAT:
        s->gblstat &= ~v;
        return;
    case A_GPREG1:
        s->gpreg1 = v;
        return;
    case A_LPOMONCTL:
        s->lpomonctl = v;
        return;
    case A_CLKTEST:
        s->clktest = v;
        return;
    case A_DEVCR1:
        s->devcr1 = v;
        return;
    case A_SSIF:
        s->ssif &= ~v;
        for (ssi = 0; ssi < 4; ssi++) {
            if (v & (1u << ssi)) {
                qemu_set_irq(s->ssi_irq[ssi], 0);
            }
        }
        return;

    case A_SSIR1:
    case A_SSIR2:
    case A_SSIR3:
    case A_SSIR4:
        ssi = (addr - A_SSIR1) / 4;
        /*
         * The upper byte is a key; only 0x75 arms the request.  FreeRTOS
         * writes 0x7500 here from portYIELD_WITHIN_API().
         */
        if (((v >> SSI_KEY_SHIFT) & 0xFFu) != SSI_KEY) {
            return;
        }
        s->ssif |= 1u << ssi;
        s->ssivec = ((v & 0xFFu) << 2) | (ssi << 6);
        qemu_set_irq(s->ssi_irq[ssi], 1);
        return;

    default:
        break;
    }

    if (addr < 0x24) {
        s->syspc[addr / 4] = v;
        return;
    }
    if (addr == A_SYSPC10) {
        s->syspc[9] = v;
        return;
    }

    qemu_log_mask(LOG_UNIMP, "tms570-system: write 0x%08x to unhandled SYS1 0x%"
                  HWADDR_PRIx "\n", v, addr);
}

static uint64_t sys2_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570SystemState *s = TMS570_SYSTEM(opaque);

    switch (addr) {
    case A_PLLCTL3:
        return s->pllctl3;
    case A_ECPCNTRL1:
        return s->ecpcntrl1;
    case A_CLK2CNTRL:
        return s->clk2cntrl;
    case A_VCLKACON1:
        return s->vclkacon1;
    case A_HCLKCNTL:
        return s->hclkcntl;
    case A_STCCLKDIV:
        return 0;
    default:
        break;
    }
    qemu_log_mask(LOG_UNIMP, "tms570-system: read from unhandled SYS2 0x%"
                  HWADDR_PRIx "\n", addr);
    return 0;
}

static void sys2_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TMS570SystemState *s = TMS570_SYSTEM(opaque);
    uint32_t v = val;

    switch (addr) {
    case A_PLLCTL3:
        s->pllctl3 = v;
        return;
    case A_ECPCNTRL1:
        s->ecpcntrl1 = v;
        return;
    case A_CLK2CNTRL:
        s->clk2cntrl = v;
        return;
    case A_VCLKACON1:
        s->vclkacon1 = v;
        return;
    case A_HCLKCNTL:
        s->hclkcntl = v;
        return;
    case A_STCCLKDIV:
        return;
    default:
        break;
    }
    qemu_log_mask(LOG_UNIMP, "tms570-system: write 0x%08x to unhandled SYS2 0x%"
                  HWADDR_PRIx "\n", v, addr);
}

static const MemoryRegionOps sys1_ops = {
    .read = sys1_read,
    .write = sys1_write,
    /*
     * The SoC runs big-endian and the CPU's data accesses are big-endian too
     * (see tms570lc43_realize()), so registers pass through unswapped.
     */
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const MemoryRegionOps sys2_ops = {
    .read = sys2_read,
    .write = sys2_write,
    /*
     * The SoC runs big-endian and the CPU's data accesses are big-endian too
     * (see tms570lc43_realize()), so registers pass through unswapped.
     */
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void sys_reset(DeviceState *dev)
{
    TMS570SystemState *s = TMS570_SYSTEM(dev);
    int i;

    memset(s->syspc, 0, sizeof(s->syspc));
    /* All clock sources enabled out of reset. */
    s->csdis = 0;
    s->cddis = 0;
    s->ghvsrc = 0;
    s->vclkasrc = 0;
    s->rclksrc = 0;
    s->pllctl1 = 0;
    s->pllctl2 = 0;
    s->pllctl3 = 0;
    s->mstgcr = 0x5;
    s->minitgcr = 0x5;
    s->msinena = 0;
    s->clkcntl = 0x00000100;
    s->ecpcntl = 0;
    s->ramgcr = 0;
    s->bmmcr1 = 0xA;
    s->cpurstcr = 0;
    s->sysecr = 0;
    s->sysesr = s->pending_reset_source ? s->pending_reset_source : s->reset_source;
    s->pending_reset_source = 0;
    s->gblstat = 0;
    s->ssif = 0;
    s->ssivec = 0;
    s->gpreg1 = 0;
    s->lpomonctl = 0;
    s->clktest = 0;
    s->devcr1 = 0;
    s->clk2cntrl = 0;
    s->hclkcntl = 0;
    s->vclkacon1 = 0;
    s->ecpcntrl1 = 0;

    for (i = 0; i < 4; i++) {
        qemu_set_irq(s->ssi_irq[i], 0);
    }
}

static void sys_watchdog_reset(void *opaque, int n, int level)
{
    TMS570SystemState *s = opaque;
    if (level) {
        s->pending_reset_source = TMS570_SYSESR_WDRST | TMS570_SYSESR_EXTRST;
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static void sys_init(Object *obj)
{
    TMS570SystemState *s = TMS570_SYSTEM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    qdev_init_gpio_in_named(DEVICE(obj), sys_watchdog_reset, "watchdog-reset", 1);

    memory_region_init_io(&s->iomem1, obj, &sys1_ops, s, "tms570-system", 0x100);
    sysbus_init_mmio(sbd, &s->iomem1);

    memory_region_init_io(&s->iomem2, obj, &sys2_ops, s, "tms570-system2", 0x100);
    sysbus_init_mmio(sbd, &s->iomem2);

    for (i = 0; i < 4; i++) {
        sysbus_init_irq(sbd, &s->ssi_irq[i]);
    }
}

static const Property sys_properties[] = {
    DEFINE_PROP_UINT32("reset-source", TMS570SystemState, reset_source,
                       TMS570_SYSESR_PORST),
};

static const VMStateDescription vmstate_tms570_system = {
    .name = "tms570-system",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(syspc, TMS570SystemState, 10),
        VMSTATE_UINT32(csdis, TMS570SystemState),
        VMSTATE_UINT32(cddis, TMS570SystemState),
        VMSTATE_UINT32(ghvsrc, TMS570SystemState),
        VMSTATE_UINT32(vclkasrc, TMS570SystemState),
        VMSTATE_UINT32(rclksrc, TMS570SystemState),
        VMSTATE_UINT32(pllctl1, TMS570SystemState),
        VMSTATE_UINT32(pllctl2, TMS570SystemState),
        VMSTATE_UINT32(pllctl3, TMS570SystemState),
        VMSTATE_UINT32(sysesr, TMS570SystemState),
        VMSTATE_UINT32(ssif, TMS570SystemState),
        VMSTATE_UINT32(ssivec, TMS570SystemState),
        VMSTATE_END_OF_LIST()
    }
};

static void sys_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sys_reset);
    device_class_set_props(dc, sys_properties);
    dc->vmsd = &vmstate_tms570_system;
    dc->desc = "TI TMS570 system module";
}

static const TypeInfo tms570_system_info = {
    .name          = TYPE_TMS570_SYSTEM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570SystemState),
    .instance_init = sys_init,
    .class_init    = sys_class_init,
};

static void tms570_system_register_types(void)
{
    type_register_static(&tms570_system_info);
}

type_init(tms570_system_register_types)
