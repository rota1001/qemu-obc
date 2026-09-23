/*
 * TI TMS570 Dual Clock Comparator (DCC)
 *
 * The DCC counts one clock against another to confirm a PLL is running at the
 * expected rate.  There is no notion of a wrong clock rate here -- the model
 * has no real clock tree -- so a measurement started via GCTRL completes
 * immediately and reports "done, no error".
 *
 * This matters because the PLL errata workaround the firmware runs at reset
 * (HL_errata_SSWF021_45.c, check_frequency()) spins on
 *
 *     while (dccREG1->STAT == 0U) { }
 *
 * and then treats STAT bit 0 as the error flag.  Leaving STAT at zero hangs
 * the boot before main() is ever reached.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "hw/misc/tms570_dcc.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

REG32(GCTRL,      0x00)
REG32(REV,        0x04)
REG32(CNT0SEED,   0x08)
REG32(VALID0SEED, 0x0C)
REG32(CNT1SEED,   0x10)
REG32(STAT,       0x14)
REG32(CNT0,       0x18)
REG32(VALID0,     0x1C)
REG32(CNT1,       0x20)
REG32(CNT1CLKSRC, 0x24)
REG32(CNT0CLKSRC, 0x28)

#define DCC_STAT_ERR    (1u << 0)
#define DCC_STAT_DONE   (1u << 1)

/* GCTRL[3:0] == 0xA enables the module; anything else disables it. */
#define DCC_GCTRL_ENABLE 0xA

static void dcc_update_irq(TMS570DccState *s)
{
    qemu_set_irq(s->err_irq, (s->stat & DCC_STAT_ERR) != 0);
    qemu_set_irq(s->done_irq, (s->stat & DCC_STAT_DONE) != 0);
}

static uint64_t dcc_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570DccState *s = TMS570_DCC(opaque);

    switch (addr) {
    case A_GCTRL:
        return s->gctrl;
    case A_REV:
        return 0x44420100;
    case A_CNT0SEED:
        return s->cnt0seed;
    case A_VALID0SEED:
        return s->valid0seed;
    case A_CNT1SEED:
        return s->cnt1seed;
    case A_STAT:
        return s->stat;
    case A_CNT0:
        return s->cnt0;
    case A_VALID0:
        return s->valid0;
    case A_CNT1:
        return s->cnt1;
    case A_CNT1CLKSRC:
        return s->cnt1clksrc;
    case A_CNT0CLKSRC:
        return s->cnt0clksrc;
    default:
        break;
    }

    qemu_log_mask(LOG_UNIMP, "tms570-dcc: read from unhandled 0x%" HWADDR_PRIx
                  "\n", addr);
    return 0;
}

static void dcc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TMS570DccState *s = TMS570_DCC(opaque);
    uint32_t v = val;

    switch (addr) {
    case A_GCTRL:
        s->gctrl = v;
        if ((v & 0xF) == DCC_GCTRL_ENABLE) {
            /* Measurement finishes at once, within tolerance. */
            s->stat = DCC_STAT_DONE;
            s->cnt0 = 0;
            s->cnt1 = 0;
            s->valid0 = 0;
        } else {
            s->stat = 0;
            s->cnt0 = s->cnt0seed;
            s->cnt1 = s->cnt1seed;
            s->valid0 = s->valid0seed;
        }
        dcc_update_irq(s);
        return;
    case A_CNT0SEED:
        s->cnt0seed = v;
        return;
    case A_VALID0SEED:
        s->valid0seed = v;
        return;
    case A_CNT1SEED:
        s->cnt1seed = v;
        return;
    case A_STAT:
        s->stat &= ~v;   /* write one to clear */
        dcc_update_irq(s);
        return;
    case A_CNT1CLKSRC:
        s->cnt1clksrc = v;
        return;
    case A_CNT0CLKSRC:
        s->cnt0clksrc = v;
        return;
    case A_REV:
    case A_CNT0:
    case A_VALID0:
    case A_CNT1:
        return;  /* read-only */
    default:
        break;
    }

    qemu_log_mask(LOG_UNIMP, "tms570-dcc: write 0x%08x to unhandled 0x%"
                  HWADDR_PRIx "\n", v, addr);
}

static const MemoryRegionOps dcc_ops = {
    .read = dcc_read,
    .write = dcc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void dcc_reset(DeviceState *dev)
{
    TMS570DccState *s = TMS570_DCC(dev);

    s->gctrl = 0x5;
    s->cnt0seed = 0;
    s->valid0seed = 0;
    s->cnt1seed = 0;
    s->stat = 0;
    s->cnt0 = 0;
    s->valid0 = 0;
    s->cnt1 = 0;
    s->cnt1clksrc = 0;
    s->cnt0clksrc = 0;
    dcc_update_irq(s);
}

static void dcc_instance_init(Object *obj)
{
    TMS570DccState *s = TMS570_DCC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &dcc_ops, s, "tms570-dcc", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->err_irq);
    sysbus_init_irq(sbd, &s->done_irq);
}

static const VMStateDescription vmstate_tms570_dcc = {
    .name = "tms570-dcc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gctrl, TMS570DccState),
        VMSTATE_UINT32(stat, TMS570DccState),
        VMSTATE_END_OF_LIST()
    }
};

static void dcc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, dcc_reset);
    dc->vmsd = &vmstate_tms570_dcc;
    dc->desc = "TI TMS570 dual clock comparator";
}

static const TypeInfo tms570_dcc_info = {
    .name          = TYPE_TMS570_DCC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570DccState),
    .instance_init = dcc_instance_init,
    .class_init    = dcc_class_init,
};

static void tms570_dcc_register_types(void)
{
    type_register_static(&tms570_dcc_info);
}

type_init(tms570_dcc_register_types)
