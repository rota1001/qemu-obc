/*
 * TMS570 board configuration registers used by OBC100.
 * These retain configuration; they do not change QEMU bus timing, gate device
 * clocks, route physical pads, or implement flash programming/ECC diagnostics.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qemu/log.h"
#include "qom/object.h"

#define TYPE_TMS570_CONFIG "tms570-config"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570ConfigState, TMS570_CONFIG)

enum { PINMUX, PCR, EMIF, FLASHW };
struct TMS570ConfigState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t kind;
    uint32_t size;
    uint32_t regs[1024];
};

/* PCR set and clear aliases share the same stored latch. */
static int pcr_latch(unsigned addr, bool *clear)
{
    *clear = false;
    if (addr <= 4 || (addr >= 0x20 && addr <= 0x2c) ||
        (addr >= 0x60 && addr <= 0x64) ||
        (addr >= 0x80 && addr <= 0x8c) || addr == 0xc0) {
        return addr / 4;
    }
    *clear = true;
    if ((addr >= 0x10 && addr <= 0x14) ||
        (addr >= 0x70 && addr <= 0x74)) {
        return (addr - 0x10) / 4;
    }
    if ((addr >= 0x40 && addr <= 0x4c) ||
        (addr >= 0xa0 && addr <= 0xac)) {
        return (addr - 0x20) / 4;
    }
    return addr == 0xc4 ? 0xc0 / 4 : -1;
}

static bool pinmux_control(hwaddr addr)
{
    return (addr >= 0x110 && addr <= 0x1a4) ||
           (addr >= 0x250 && addr <= 0x29c) ||
           (addr >= 0x390 && addr <= 0x3dc);
}

static bool config_register(TMS570ConfigState *s, hwaddr addr)
{
    switch (s->kind) {
    case PINMUX:
        return addr == 0x38 || addr == 0x3c || pinmux_control(addr);
    case EMIF:
        return (addr >= 4 && addr <= 0x20) || addr == 0x3c ||
               addr == 0x68;
    case FLASHW:
        return addr == 0 || addr == 0x40 || addr == 0x288 || addr == 0x2b8;
    default:
        return false;
    }
}

static uint64_t config_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570ConfigState *s = opaque;
    bool clear;
    int latch;

    if (s->kind == PCR && (latch = pcr_latch(addr, &clear)) >= 0) {
        return s->regs[latch];
    }
    if (config_register(s, addr)) {
        return s->regs[addr / 4];
    }
    qemu_log_mask(LOG_UNIMP, "tms570-config %u: read at 0x%" HWADDR_PRIx
                  "\n", s->kind, addr);
    return 0;
}

static void config_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    TMS570ConfigState *s = opaque;
    bool clear;
    int latch;

    if (s->kind == PCR && (latch = pcr_latch(addr, &clear)) >= 0) {
        if (clear) {
            s->regs[latch] &= ~value;
        } else {
            s->regs[latch] |= value;
        }
        return;
    }
    if (config_register(s, addr)) {
        if (s->kind == PINMUX && pinmux_control(addr) &&
            (s->regs[0x38 / 4] != 0x83e70b13 ||
             s->regs[0x3c / 4] != 0x95a4f1e0)) {
            return;
        }
        s->regs[addr / 4] = value;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "tms570-config %u: write at 0x%" HWADDR_PRIx
                  "\n", s->kind, addr);
}

static const MemoryRegionOps config_ops = {
    .read = config_read,
    .write = config_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void config_reset(DeviceState *dev)
{
    TMS570ConfigState *s = TMS570_CONFIG(dev);
    memset(s->regs, 0, sizeof(s->regs));
    if (s->kind == PCR) {
        /* Peripheral and memory power-down latches are set at reset. */
        s->regs[0x60 / 4] = s->regs[0x64 / 4] = UINT32_MAX;
        for (unsigned i = 0x80 / 4; i <= 0x8c / 4; i++) {
            s->regs[i] = UINT32_MAX;
        }
    }
}

static void config_realize(DeviceState *dev, Error **errp)
{
    Object *obj = OBJECT(dev);
    TMS570ConfigState *s = TMS570_CONFIG(dev);
    memory_region_init_io(&s->iomem, obj, &config_ops, s,
                          TYPE_TMS570_CONFIG, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property config_properties[] = {
    DEFINE_PROP_UINT32("size", TMS570ConfigState, size, 0x1000),
    DEFINE_PROP_UINT32("kind", TMS570ConfigState, kind, PINMUX),
};

static void config_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = config_realize;
    device_class_set_props(dc, config_properties);
    device_class_set_legacy_reset(dc, config_reset);
    dc->desc = "TMS570 board configuration register subset";
}

static const TypeInfo config_info = {
    .name = TYPE_TMS570_CONFIG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570ConfigState),
    .class_init = config_class_init,
};

static void config_register_types(void)
{
    type_register_static(&config_info);
}
type_init(config_register_types)
