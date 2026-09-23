/* N2HET pin registers. Instruction engine is not used by OBC100 firmware.
 * Local experiment. SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/gpio/tms570_het_gpio.h"
#include "hw/core/irq.h"
#include "qemu/log.h"

#define DIR 0x4c
#define DIN 0x50
#define DOUT 0x54
#define DSET 0x58
#define DCLR 0x5c
#define PULDIS 0x64
#define PSL 0x68

static uint64_t het_read(void *opaque, hwaddr offset, unsigned size)
{
    TMS570HetGpioState *s = opaque;
    if (offset == DIN) {
        uint32_t dir = s->reg[DIR / 4];
        uint32_t pins = (s->input & s->driven) |
            (s->reg[PSL / 4] & ~s->reg[PULDIS / 4] & ~s->driven);
        return (pins & ~dir) | (s->reg[DOUT / 4] & dir);
    }
    if (offset == DSET || offset == DCLR) {
        return s->reg[DOUT / 4];
    }
    return s->reg[offset / 4];
}

static void het_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    TMS570HetGpioState *s = opaque;
    unsigned i;
    if (offset == DIN) {
        return;
    }
    if (offset == DSET) {
        s->reg[DOUT / 4] |= value;
    } else if (offset == DCLR) {
        s->reg[DOUT / 4] &= ~value;
    } else {
        s->reg[offset / 4] = value;
    }
    if (offset == 0 && (value & 1)) {
        qemu_log_mask(LOG_UNIMP, "tms570-het: instruction engine not modelled\n");
    }
    for (i = 0; i < 32; i++) {
        qemu_set_irq(s->out[i], !!(s->reg[DIR / 4] &
                                   s->reg[DOUT / 4] & (1u << i)));
    }
}

static void het_input(void *opaque, int pin, int level)
{
    TMS570HetGpioState *s = opaque;
    s->driven |= 1u << pin;
    s->input = (s->input & ~(1u << pin)) | (!!level << pin);
}

static const MemoryRegionOps het_ops = {
    .read = het_read, .write = het_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void het_reset(DeviceState *dev)
{
    TMS570HetGpioState *s = TMS570_HET_GPIO(dev);
    memset(s->reg, 0, sizeof(s->reg));
    for (unsigned i = 0; i < 32; i++) {
        qemu_set_irq(s->out[i], 0);
    }
}

static void het_init(Object *obj)
{
    TMS570HetGpioState *s = TMS570_HET_GPIO(obj);
    memory_region_init_io(&s->iomem, obj, &het_ops, s, TYPE_TMS570_HET_GPIO, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_in(DEVICE(obj), het_input, 32);
    qdev_init_gpio_out(DEVICE(obj), s->out, 32);
}

static void het_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), het_reset);
}

static const TypeInfo het_info = {
    .name = TYPE_TMS570_HET_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570HetGpioState),
    .instance_init = het_init,
    .class_init = het_class_init,
};
static void het_register_types(void)
{
    type_register_static(&het_info);
}
type_init(het_register_types)
