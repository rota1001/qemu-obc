/* TMS570 I2C master subset used by the OBC100 polled driver.
 * Local experiment, not intended for upstream.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/i2c/tms570_i2c.h"
#include "qemu/log.h"

#define R_STR   (0x08 / 4)
#define R_CNT   (0x14 / 4)
#define R_DRR   (0x18 / 4)
#define R_SAR   (0x1c / 4)
#define R_DXR   (0x20 / 4)
#define R_MDR   (0x24 / 4)
#define R_DIN   (0x50 / 4)
#define MDR_RESET  0x0020
#define MDR_RX     0x0200
#define MDR_STOP   0x0800
#define MDR_START  0x2000
#define STR_NACK   0x0002
#define STR_ARDY   0x0004
#define STR_RXRDY  0x0008
#define STR_TXRDY  0x0010
#define STR_SCD    0x0020
#define STR_BB     0x1000

static void i2c_finish(TMS570I2CState *s)
{
    if (s->active) {
        i2c_end_transfer(s->bus);
        s->active = false;
    }
    s->status &= ~(STR_BB | STR_RXRDY | STR_TXRDY);
    s->status |= STR_SCD;
    s->reg[R_MDR] &= ~(MDR_STOP | MDR_START);
}

static void i2c_start(TMS570I2CState *s)
{
    bool receive = !(s->reg[R_MDR] & MDR_RX);
    uint8_t address = s->reg[R_SAR] & 0x7f;

    s->status &= ~(STR_NACK | STR_ARDY | STR_SCD | STR_RXRDY | STR_TXRDY);
    if (i2c_start_transfer(s->bus, address, receive) != 0) {
        s->status |= STR_NACK;
        i2c_finish(s);
        return;
    }
    s->active = true;
    s->remaining = s->reg[R_CNT] & 0xffff;
    s->status |= STR_BB;
    s->status |= receive ? STR_RXRDY : STR_TXRDY;
    s->reg[R_MDR] &= ~MDR_START;
    if (!s->remaining) {
        s->status |= STR_ARDY;
        if (s->reg[R_MDR] & MDR_STOP) {
            i2c_finish(s);
        }
    }
}

static uint64_t i2c_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570I2CState *s = opaque;
    unsigned index = addr / 4;
    uint8_t data;

    switch (index) {
    case R_STR:
        if (s->active && !s->remaining && (s->reg[R_MDR] & MDR_STOP)) {
            i2c_finish(s);
        }
        return s->status;
    case R_DIN:
        return 3; /* idle pull-ups on SDA and SCL */
    case R_DRR:
        if (!s->active || !(s->status & STR_RXRDY) || !s->remaining) {
            return s->reg[index];
        }
        data = i2c_recv(s->bus);
        s->reg[index] = data;
        if (--s->remaining == 0) {
            s->status &= ~STR_RXRDY;
            s->status |= STR_ARDY;
            if (s->reg[R_MDR] & MDR_STOP) {
                i2c_finish(s);
            }
        }
        return data;
    default:
        return s->reg[index];
    }
}

static void i2c_reg_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    TMS570I2CState *s = opaque;
    unsigned index = addr / 4;
    uint32_t v = val;

    switch (index) {
    case R_STR:
        s->status &= ~v; /* status flags are write-one-to-clear */
        return;
    case R_DXR:
        s->reg[index] = v;
        if (s->active && (s->status & STR_TXRDY) && s->remaining) {
            if (i2c_send(s->bus, v & 0xff) < 0) {
                s->status |= STR_NACK;
                i2c_finish(s);
            } else if (--s->remaining == 0) {
                s->status &= ~STR_TXRDY;
                s->status |= STR_ARDY;
                if (s->reg[R_MDR] & MDR_STOP) {
                    i2c_finish(s);
                }
            }
        }
        return;
    case R_MDR:
        s->reg[index] = v;
        if (!(v & MDR_RESET)) {
            if (s->active) {
                i2c_end_transfer(s->bus);
                s->active = false;
            }
            s->status = 0;
        } else if (v & MDR_START) {
            i2c_start(s);
        }
        return;
    default:
        s->reg[index] = v;
        return;
    }
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_reg_read, .write = i2c_reg_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void i2c_reset(DeviceState *dev)
{
    TMS570I2CState *s = TMS570_I2C(dev);
    if (s->active) {
        i2c_end_transfer(s->bus);
    }
    memset(s->reg, 0, sizeof(s->reg));
    s->status = 0;
    s->remaining = 0;
    s->active = false;
}

static void i2c_init(Object *obj)
{
    TMS570I2CState *s = TMS570_I2C(obj);
    memory_region_init_io(&s->iomem, obj, &i2c_ops, s, "tms570-i2c", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
}

static void i2c_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), i2c_reset);
}

static const TypeInfo i2c_info = {
    .name = TYPE_TMS570_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570I2CState),
    .instance_init = i2c_init,
    .class_init = i2c_class_init,
};

static void i2c_register_types(void)
{
    type_register_static(&i2c_info);
}
type_init(i2c_register_types)
