/*
 * TI TMS570 Error Signaling Module (ESM)
 *
 * Models 96 group-1 channels, group-2 and group-3 status, interrupt vectors
 * and error-pin control. The error GPIO input is arranged as group 1 (0-95),
 * group 2 (96-127), group 3 (128-159). Low-time pin timing is not modeled.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "hw/misc/tms570_esm.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

REG32(EEPAPR1,  0x00)
REG32(DEPAPR1,  0x04)
REG32(IESR1,    0x08)
REG32(IECR1,    0x0C)
REG32(ILSR1,    0x10)
REG32(ILCR1,    0x14)
#define A_SR1   0x18   /* SR1[3]: 0x18, 0x1C, 0x20 */
REG32(EPSR,     0x24)
REG32(IOFFHR,   0x28)
REG32(IOFFLR,   0x2C)
REG32(LTCR,     0x30)
REG32(LTCPR,    0x34)
REG32(EKR,      0x38)
REG32(SSR2,     0x3C)

static void esm_update(TMS570EsmState *s)
{
    uint32_t g1 = s->sr[0] & s->ier;

    bool high = (g1 & s->ilr) || s->sr[1];
    bool low = (g1 & ~s->ilr) != 0;
    bool pin = (s->sr[0] & s->epapr) || s->sr[1] || s->sr[2];

    for (unsigned i = 0; i < 2; i++) {
        uint32_t pending = s->upper_sr[i] & s->upper_ier[i];
        high |= (pending & s->upper_ilr[i]) != 0;
        low |= (pending & ~s->upper_ilr[i]) != 0;
        pin |= (s->upper_sr[i] & s->upper_epapr[i]) != 0;
    }
    s->error_pin |= pin;
    qemu_set_irq(s->irq_high, high);
    qemu_set_irq(s->irq_low, low);
}

/* Index of the lowest set bit plus one, as IOFFxR reports it. */
static uint32_t esm_offset(uint32_t pending)
{
    return pending ? ctz32(pending) + 1 : 0;
}

static uint32_t esm_vector(TMS570EsmState *s, bool high)
{
    uint32_t pending, vector;

    if (high && s->sr[1]) {
        vector = esm_offset(s->sr[1]);
        s->sr[1] &= ~(1u << (vector - 1));
        esm_update(s);
        return 32 + vector;
    }
    pending = s->sr[0] & s->ier & (high ? s->ilr : ~s->ilr);
    if (pending) {
        return esm_offset(pending);
    }
    for (unsigned i = 0; i < 2; i++) {
        pending = s->upper_sr[i] & s->upper_ier[i] &
                  (high ? s->upper_ilr[i] : ~s->upper_ilr[i]);
        if (pending) {
            return 64 * (i + 1) + esm_offset(pending);
        }
    }
    return 0;
}

static void esm_error(void *opaque, int channel, int level)
{
    TMS570EsmState *s = opaque;
    unsigned bank = channel / 32;
    uint32_t bit = 1u << (channel % 32);

    if (!level) {
        return;
    }
    if (bank == 0) {
        s->sr[0] |= bit;
    } else if (bank < 3) {
        s->upper_sr[bank - 1] |= bit;
    } else {
        s->sr[bank - 2] |= bit;
        if (bank == 3) {
            s->ssr2 |= bit;
        }
    }
    esm_update(s);
}

static uint64_t esm_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570EsmState *s = TMS570_ESM(opaque);

    if (addr >= 0x40 && addr <= 0xa0) {
        unsigned bank = addr / 0x40 - 1;
        switch (addr % 0x40) {
        case 0: case 4: return s->upper_epapr[bank];
        case 8: case 12: return s->upper_ier[bank];
        case 16: case 20: return s->upper_ilr[bank];
        case 24: return s->upper_sr[bank];
        default: return 0;
        }
    }
    switch (addr) {
    case A_EEPAPR1:
        return s->epapr;
    case A_DEPAPR1:
        return s->epapr;
    case A_IESR1:
    case A_IECR1:
        return s->ier;
    case A_ILSR1:
    case A_ILCR1:
        return s->ilr;
    case A_SR1:
    case A_SR1 + 4:
    case A_SR1 + 8:
        return s->sr[(addr - A_SR1) / 4];
    case A_EPSR:
        return !s->error_pin;
    case A_IOFFHR:
        return esm_vector(s, true);
    case A_IOFFLR:
        return esm_vector(s, false);
    case A_LTCR:
        return s->ltcr;
    case A_LTCPR:
        return s->ltcpr;
    case A_EKR:
        return s->ekr;
    case A_SSR2:
        return s->ssr2;
    default:
        break;
    }

    qemu_log_mask(LOG_UNIMP, "tms570-esm: read from unhandled 0x%" HWADDR_PRIx
                  "\n", addr);
    return 0;
}

static void esm_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TMS570EsmState *s = TMS570_ESM(opaque);
    uint32_t v = val;

    if (addr >= 0x40 && addr <= 0xa0) {
        unsigned bank = addr / 0x40 - 1;
        switch (addr % 0x40) {
        case 0: s->upper_epapr[bank] |= v; break;
        case 4: s->upper_epapr[bank] &= ~v; break;
        case 8: s->upper_ier[bank] |= v; break;
        case 12: s->upper_ier[bank] &= ~v; break;
        case 16: s->upper_ilr[bank] |= v; break;
        case 20: s->upper_ilr[bank] &= ~v; break;
        case 24: s->upper_sr[bank] &= ~v; break;
        default: break;
        }
        esm_update(s);
        return;
    }
    switch (addr) {
    case A_EEPAPR1:
        s->epapr |= v;
        esm_update(s);
        return;
    case A_DEPAPR1:
        s->epapr &= ~v;
        return;
    case A_IESR1:
        s->ier |= v;
        esm_update(s);
        return;
    case A_IECR1:
        s->ier &= ~v;
        esm_update(s);
        return;
    case A_ILSR1:
        s->ilr |= v;
        esm_update(s);
        return;
    case A_ILCR1:
        s->ilr &= ~v;
        esm_update(s);
        return;
    case A_SR1:
    case A_SR1 + 4:
    case A_SR1 + 8:
        /* Write one to clear. */
        s->sr[(addr - A_SR1) / 4] &= ~v;
        esm_update(s);
        return;
    case A_LTCR:
        s->ltcr = v;
        return;
    case A_LTCPR:
        s->ltcpr = v;
        return;
    case A_EKR:
        s->ekr = v & 0xf;
        if (s->ekr == 0xa) {
            s->error_pin = true;
        } else if (s->ekr == 5) {
            s->error_pin = false;
            esm_update(s);
        }
        return;
    case A_SSR2:
        s->ssr2 &= ~v;
        return;
    case A_EPSR:
    case A_IOFFHR:
    case A_IOFFLR:
        return;  /* read-only */
    default:
        break;
    }

    qemu_log_mask(LOG_UNIMP, "tms570-esm: write 0x%08x to unhandled 0x%"
                  HWADDR_PRIx "\n", v, addr);
}

static const MemoryRegionOps esm_ops = {
    .read = esm_read,
    .write = esm_write,
    /*
     * The SoC runs big-endian and the CPU's data accesses are big-endian too
     * (see tms570lc43_realize()), so registers pass through unswapped.
     */
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void esm_reset(DeviceState *dev)
{
    TMS570EsmState *s = TMS570_ESM(dev);

    memset(s->sr, 0, sizeof(s->sr));
    s->ier = 0;
    s->ilr = 0;
    s->epapr = 0;
    memset(s->upper_sr, 0, sizeof(s->upper_sr));
    memset(s->upper_ier, 0, sizeof(s->upper_ier));
    memset(s->upper_ilr, 0, sizeof(s->upper_ilr));
    memset(s->upper_epapr, 0, sizeof(s->upper_epapr));
    s->error_pin = false;
    s->ltcr = 0;
    s->ltcpr = 0;
    s->ekr = 0;
    s->ssr2 = 0;
    esm_update(s);
}

static void esm_instance_init(Object *obj)
{
    TMS570EsmState *s = TMS570_ESM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esm_ops, s, "tms570-esm", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_high);
    sysbus_init_irq(sbd, &s->irq_low);
    qdev_init_gpio_in_named(DEVICE(obj), esm_error, "error", 160);
}

static const VMStateDescription vmstate_tms570_esm = {
    .name = "tms570-esm",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(sr, TMS570EsmState, TMS570_ESM_NUM_GROUPS),
        VMSTATE_UINT32(ier, TMS570EsmState),
        VMSTATE_UINT32(ilr, TMS570EsmState),
        VMSTATE_UINT32_ARRAY(upper_sr, TMS570EsmState, 2),
        VMSTATE_UINT32_ARRAY(upper_ier, TMS570EsmState, 2),
        VMSTATE_UINT32_ARRAY(upper_ilr, TMS570EsmState, 2),
        VMSTATE_UINT32_ARRAY(upper_epapr, TMS570EsmState, 2),
        VMSTATE_UINT32(epapr, TMS570EsmState),
        VMSTATE_UINT32(ltcr, TMS570EsmState),
        VMSTATE_UINT32(ltcpr, TMS570EsmState),
        VMSTATE_UINT32(ekr, TMS570EsmState),
        VMSTATE_UINT32(ssr2, TMS570EsmState),
        VMSTATE_BOOL(error_pin, TMS570EsmState),
        VMSTATE_END_OF_LIST()
    }
};

static void esm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, esm_reset);
    dc->vmsd = &vmstate_tms570_esm;
    dc->desc = "TI TMS570 Error Signaling Module";
}

static const TypeInfo tms570_esm_info = {
    .name          = TYPE_TMS570_ESM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570EsmState),
    .instance_init = esm_instance_init,
    .class_init    = esm_class_init,
};

static void tms570_esm_register_types(void)
{
    type_register_static(&tms570_esm_info);
}

type_init(tms570_esm_register_types)
