/* Polled TMS570 ADC conversion and calibration subset, local experiment.
 * Synthetic samples are deterministic and do not represent board sensors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/adc/tms570_adc.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"

#define R_OPMODE 0x04
#define R_CALCR  0x0c
#define R_INTFLG 0x34
#define R_INTCR  0x40
#define R_BNDEND 0x5c
#define R_SEL    0x78
#define R_CALR   0x84
#define R_BUF0   0x90

static void adc_irq(TMS570AdcState *s)
{
    for (unsigned g = 0; g < 3; g++) {
        qemu_set_irq(s->irq[g], !!(s->reg[(0x28 / 4) + g] &
                                   s->reg[R_INTFLG / 4 + g]));
    }
}

static void adc_input(void *opaque, int channel, int value)
{
    TMS570AdcState *s = opaque;
    s->sample[channel] = MIN(MAX(value, 0), 4095);
}

static uint64_t adc_read(void *opaque, hwaddr offset, unsigned size)
{
    TMS570AdcState *s = opaque;
    unsigned group;

    if (offset >= 0x200 || (offset & 3)) {
        return 0;
    }
    for (group = 0; group < 3; group++) {
        if (offset >= R_BUF0 + group * 0x20 &&
            offset < R_BUF0 + (group + 1) * 0x20) {
            unsigned index = s->fifo_index[group];
            if (index < s->fifo_count[group]) {
                s->fifo_index[group]++;
                s->reg[R_INTCR / 4 + group] =
                    (s->reg[R_INTCR / 4 + group] + 1) & 0x1ff;
                return s->fifo[group][index];
            }
            return s->reg[R_OPMODE / 4] & 0x80000000 ? 0x80000000 : 0x8000;
        }
    }
    return s->reg[offset / 4];
}

static void adc_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    TMS570AdcState *s = opaque;
    unsigned group;

    if (offset >= 0x200 || (offset & 3)) {
        return;
    }
    if (offset == R_CALCR) {
        /* Hardware clears CALSTART when calibration conversion completes. */
        s->reg[offset / 4] = value & ~0x10000u;
        if (value & 0x10000u) {
            s->reg[R_CALR / 4] = 0x7ff;
        }
        return;
    }
    for (group = 0; group < 3; group++) {
        if (offset == R_INTFLG + group * 4) {
            s->reg[offset / 4] &= ~value;
            adc_irq(s);
            return;
        }
        if (offset == R_INTCR + group * 4) {
            s->threshold[group] = value & 0x1ff;
            s->reg[offset / 4] = value & 0x1ff;
            return;
        }
        if (offset == R_SEL + group * 4) {
            s->reg[offset / 4] = value;
            if (value) {
                s->fifo_index[group] = 0;
                s->fifo_count[group] = 0;
                for (unsigned ch = 0; ch < 32; ch++) {
                    if (value & (1u << ch)) {
                        unsigned shift = s->reg[R_OPMODE / 4] & 0x80000000 ? 16 : 10;
                        unsigned id = s->reg[0x10 / 4 + group] & 0x20 ? ch : 0;
                        unsigned sample = s->sample[ch];
                        if (shift == 10) {
                            sample >>= 2;
                        }
                        s->fifo[group][s->fifo_count[group]++] = (id << shift) | sample;
                    }
                }
                /* Signed 9-bit threshold counts down on conversion, up on read. */
                s->reg[R_INTCR / 4 + group] =
                    (s->threshold[group] - s->fifo_count[group]) & 0x1ff;
                s->reg[(R_INTFLG + group * 4) / 4] |= 8;
                if (s->threshold[group] && s->fifo_count[group] >= s->threshold[group]) {
                    s->reg[R_INTFLG / 4 + group] |= 1;
                }
            }
            adc_irq(s);
            return;
        }
    }
    s->reg[offset / 4] = value;
    if (offset == R_BNDEND) {
        s->reg[offset / 4] &= 0xffff;
    }
    adc_irq(s);
}

static const MemoryRegionOps adc_ops = {
    .read = adc_read,
    .write = adc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void adc_reset(DeviceState *dev)
{
    TMS570AdcState *s = TMS570_ADC(dev);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->fifo_index, 0, sizeof(s->fifo_index));
    memset(s->fifo_count, 0, sizeof(s->fifo_count));
    memset(s->threshold, 0, sizeof(s->threshold));
    s->reg[R_OPMODE / 4] = 0x80000000;
    for (unsigned ch = 0; ch < 32; ch++) {
        s->sample[ch] = 1800;
    }
    if (s->unit == 0) {
        s->sample[0] = 2200;
        s->sample[1] = 1429;
        s->sample[2] = s->sample[3] = 3929;
    }
    s->sample[30] = s->sample[31] = 2000;
    adc_irq(s);
}

static void adc_init(Object *obj)
{
    TMS570AdcState *s = TMS570_ADC(obj);
    unsigned ch;

    memory_region_init_io(&s->iomem, obj, &adc_ops, s, TYPE_TMS570_ADC, 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (unsigned g = 0; g < 3; g++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[g]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), adc_input, "analog", 32);

    /*
     * Test harness input.  QMP's standard qom-set/qom-get commands can write
     * these properties while the machine is running.  Values are raw 12-bit
     * samples; board-level units and sensor transfer functions belong in the
     * external test API, not in the ADC peripheral.
     */
    for (ch = 0; ch < 32; ch++) {
        object_property_add_uint16_ptr(obj, "sample[*]", &s->sample[ch],
                                       OBJ_PROP_FLAG_READWRITE);
    }
}

static const Property adc_properties[] = {
    DEFINE_PROP_UINT32("unit", TMS570AdcState, unit, 0),
};

static void adc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, adc_reset);
    device_class_set_props(dc, adc_properties);
}

static const TypeInfo adc_info = {
    .name = TYPE_TMS570_ADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570AdcState),
    .instance_init = adc_init,
    .class_init = adc_class_init,
};

static void adc_register_types(void)
{
    type_register_static(&adc_info);
}
type_init(adc_register_types)
