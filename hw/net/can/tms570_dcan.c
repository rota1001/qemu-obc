/* TMS570 DCAN message RAM/IF interface, local OBC100 experiment.
 * Classical CAN frames use the QEMU CAN bus. Bit timing and error confinement
 * are not simulated. Register layout follows TI SPNU563, DCAN chapter.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/net/tms570_dcan.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"

#define CTL 0x00
#define ES 0x04
#define INT 0x10
#define TEST 0x14
#define INIT 1u
#define VALID (1u << 31)
#define EXT (1u << 30)
#define DIR (1u << 29)
#define NEWDAT 0x8000
#define MSGLST 0x4000
#define UMASK 0x1000
#define TXIE 0x800
#define RXIE 0x400
#define INTPND 0x200
#define TXRQST 0x100
#define EOB 0x80

static void dcan_update_irq(TMS570DcanState *s)
{
    uint32_t pending[2] = { 0, 0 };
    unsigned i, line;

    for (i = 0; i < 128; i++) {
        line = (s->reg[(0xd8 / 4) + i / 32] >> (i % 32)) & 1;
        if ((s->msg[i].control & INTPND) && !pending[line]) {
            pending[line] = i + 1;
        }
    }
    s->reg[INT / 4] = pending[0] | (pending[1] << 16);
    qemu_set_irq(s->irq[0], pending[0] && (s->reg[CTL / 4] & 2));
    qemu_set_irq(s->irq[1], pending[1] && (s->reg[CTL / 4] & 0x20000));
}

static bool dcan_can_receive(CanBusClientState *client)
{
    TMS570DcanState *s = container_of(client, TMS570DcanState, client);
    return !(s->reg[CTL / 4] & INIT);
}

static ssize_t dcan_receive(CanBusClientState *client,
                           const qemu_can_frame *frames, size_t count)
{
    TMS570DcanState *s = container_of(client, TMS570DcanState, client);
    size_t f;
    unsigned i, j;

    for (f = 0; f < count; f++) {
        const qemu_can_frame *frame = &frames[f];
        uint32_t arb = (frame->can_id & QEMU_CAN_EFF_FLAG) ?
            EXT | (frame->can_id & QEMU_CAN_EFF_MASK) :
            (frame->can_id & QEMU_CAN_SFF_MASK) << 18;
        if (frame->flags & QEMU_CAN_FRMF_TYPE_FD ||
            frame->can_id & QEMU_CAN_ERR_FLAG) {
            continue;
        }
        if (frame->can_id & QEMU_CAN_RTR_FLAG) {
            arb |= DIR;
        }
        s->reg[ES / 4] = (s->reg[ES / 4] & ~7u) | 0x10;
        for (i = 0; i < 128; i++) {
            TMS570DcanMessage *m = &s->msg[i];
            uint32_t mask = QEMU_CAN_EFF_MASK | EXT | DIR;
            if (!(m->arb & VALID)) {
                continue;
            }
            if (m->control & UMASK) {
                mask = (m->mask & QEMU_CAN_EFF_MASK) |
                       ((m->mask & (1u << 31)) ? EXT : 0) |
                       ((m->mask & (1u << 30)) ? DIR : 0);
            }
            if ((m->arb ^ arb) & mask) {
                continue;
            }
            /* FIFO chains skip full entries until their end-of-buffer. */
            if ((m->control & NEWDAT) && !(m->control & EOB)) {
                continue;
            }
            if (m->control & NEWDAT) {
                m->control |= MSGLST;
            }
            m->arb = VALID | arb;
            m->control = (m->control & ~15u) | MIN(frame->can_dlc, 8) | NEWDAT;
            m->data[0] = m->data[1] = 0;
            for (j = 0; j < MIN(frame->can_dlc, 8); j++) {
                m->data[j / 4] |= (uint32_t)frame->data[j] << ((j % 4) * 8);
            }
            if (m->control & RXIE) {
                m->control |= INTPND;
            }
            break;
        }
    }
    dcan_update_irq(s);
    /* An active controller ACKs even a frame rejected by its ID filters. */
    return count;
}

static void dcan_transmit(void *opaque)
{
    TMS570DcanState *s = opaque;
    bool retry = false;
    unsigned i, j;

    if (s->reg[CTL / 4] & INIT) {
        return;
    }
    for (i = 0; i < 128; i++) {
        TMS570DcanMessage *m = &s->msg[i];
        qemu_can_frame frame = { 0 };
        bool ack = false;
        bool loop = (s->reg[CTL / 4] & 0x80) && (s->reg[TEST / 4] & 0x10);
        if (!(m->arb & VALID) || !(m->control & TXRQST)) {
            continue;
        }
        frame.can_id = (m->arb & EXT) ?
            (m->arb & QEMU_CAN_EFF_MASK) | QEMU_CAN_EFF_FLAG :
            (m->arb >> 18) & QEMU_CAN_SFF_MASK;
        if (!(m->arb & DIR)) {
            frame.can_id |= QEMU_CAN_RTR_FLAG;
        }
        frame.can_dlc = MIN(m->control & 15, 8);
        for (j = 0; j < frame.can_dlc; j++) {
            frame.data[j] = m->data[j / 4] >> ((j % 4) * 8);
        }
        if (loop) {
            dcan_receive(&s->client, &frame, 1);
            ack = true;
        } else if (!(s->reg[CTL / 4] & 0x80) ||
                   !(s->reg[TEST / 4] & 8)) {
            ack = can_bus_client_send(&s->client, &frame, 1) > 0;
        }
        if (ack) {
            m->control &= ~(TXRQST | NEWDAT);
            if (m->control & TXIE) {
                m->control |= INTPND;
            }
            s->reg[ES / 4] = (s->reg[ES / 4] & ~7u) | 8;
        } else {
            s->reg[ES / 4] = (s->reg[ES / 4] & ~7u) | 3; /* ACK error */
            if (s->reg[CTL / 4] & 0x20) {
                m->control &= ~TXRQST;
            } else {
                retry = true;
            }
        }
    }
    if (retry && s->canbus) {
        timer_mod(s->retry, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    }
    dcan_update_irq(s);
}

static void dcan_command(TMS570DcanState *s, unsigned base)
{
    uint32_t cmd = s->reg[base / 4] >> 16;
    unsigned number = s->reg[base / 4] & 0xff;
    TMS570DcanMessage *m;
    uint32_t *r = &s->reg[base / 4];

    if (number < 1 || number > 128) {
        return;
    }
    m = &s->msg[number - 1];
    if (cmd & 0x80) {
        if (cmd & 0x40) { m->mask = r[1]; }
        if (cmd & 0x20) { m->arb = r[2]; }
        if (cmd & 0x10) {
            m->control = r[3];
        }
        if (cmd & 4) {
            m->control |= TXRQST | NEWDAT;
        }
        if (cmd & 2) { m->data[0] = r[4]; }
        if (cmd & 1) { m->data[1] = r[5]; }
    } else {
        if (cmd & 0x40) { r[1] = m->mask; }
        if (cmd & 0x20) { r[2] = m->arb; }
        if (cmd & 0x10) { r[3] = m->control; }
        if (cmd & 2) { r[4] = m->data[0]; }
        if (cmd & 1) { r[5] = m->data[1]; }
        if (cmd & 4) { m->control &= ~NEWDAT; }
    }
    if (!(cmd & 0x80) && (cmd & 8)) { m->control &= ~INTPND; }
    dcan_transmit(s);
    dcan_update_irq(s);
}

static uint32_t dcan_bitmap(TMS570DcanState *s, unsigned kind, unsigned word)
{
    uint32_t result = 0;
    static const uint32_t bits[] = { TXRQST, NEWDAT, INTPND, VALID };
    unsigned i;
    for (i = 0; i < 32; i++) {
        TMS570DcanMessage *m = &s->msg[word * 32 + i];
        if ((kind == 3 ? m->arb : m->control) & bits[kind]) {
            result |= 1u << i;
        }
    }
    return result;
}

static uint64_t dcan_read(void *opaque, hwaddr offset, unsigned size)
{
    TMS570DcanState *s = opaque;
    uint32_t value = s->reg[offset / 4];
    unsigned kind, word;
    hwaddr aligned = offset & ~3;
    for (kind = 0; kind < 4; kind++) {
        unsigned base = 0x84 + kind * 0x14;
        if (aligned == base) {
            value = 0;
            for (word = 0; word < 4; word++) {
                uint32_t map = dcan_bitmap(s, kind, word);
                value |= (!!(map & 0xffff)) << (word * 2);
                value |= (!!(map >> 16)) << (word * 2 + 1);
            }
        } else if (aligned > base && aligned <= base + 16) {
            value = dcan_bitmap(s, kind, (aligned - base - 4) / 4);
        }
    }
    return (value >> ((4 - size - (offset & 3)) * 8)) &
           (UINT32_MAX >> ((4 - size) * 8));
}

static void dcan_reset(DeviceState *dev)
{
    TMS570DcanState *s = TMS570_DCAN(dev);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->msg, 0, sizeof(s->msg));
    s->reg[CTL / 4] = INIT;
    s->reg[ES / 4] = 7;
    timer_del(s->retry);
    dcan_update_irq(s);
}

static void dcan_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    TMS570DcanState *s = opaque;
    unsigned shift = (4 - size - (offset & 3)) * 8;
    uint32_t mask = (UINT32_MAX >> ((4 - size) * 8)) << shift;
    hwaddr aligned = offset & ~3;
    s->reg[offset / 4] = (s->reg[offset / 4] & ~mask) | (value << shift);
    if (aligned == CTL) {
        if (s->reg[CTL / 4] & 0x8000) {
            dcan_reset(DEVICE(s));
        } else {
            dcan_transmit(s);
        }
    } else if (aligned == ES) {
        s->reg[ES / 4] &= 0x1f;
    } else if ((aligned == 0x100 || aligned == 0x120) &&
               offset + size == aligned + 4) {
        dcan_command(s, aligned);
    }
    dcan_update_irq(s);
}

static const MemoryRegionOps dcan_ops = {
    .read = dcan_read,
    .write = dcan_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static CanBusClientInfo dcan_client_info = {
    .can_receive = dcan_can_receive,
    .receive = dcan_receive,
};

static void dcan_realize(DeviceState *dev, Error **errp)
{
    TMS570DcanState *s = TMS570_DCAN(dev);
    s->client.info = &dcan_client_info;
    if (s->canbus) {
        can_bus_insert_client(s->canbus, &s->client);
    }
}

static void dcan_unrealize(DeviceState *dev)
{
    TMS570DcanState *s = TMS570_DCAN(dev);
    timer_del(s->retry);
    can_bus_remove_client(&s->client);
}

static void dcan_init(Object *obj)
{
    TMS570DcanState *s = TMS570_DCAN(obj);
    memory_region_init_io(&s->iomem, obj, &dcan_ops, s, TYPE_TMS570_DCAN, 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[0]);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[1]);
    s->retry = timer_new_ns(QEMU_CLOCK_VIRTUAL, dcan_transmit, s);
}

static void dcan_finalize(Object *obj)
{
    timer_free(TMS570_DCAN(obj)->retry);
}

static const Property dcan_properties[] = {
    DEFINE_PROP_LINK("canbus", TMS570DcanState, canbus, TYPE_CAN_BUS, CanBusState *),
};

static void dcan_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = dcan_realize;
    dc->unrealize = dcan_unrealize;
    device_class_set_legacy_reset(dc, dcan_reset);
    device_class_set_props(dc, dcan_properties);
}

static const TypeInfo dcan_info = {
    .name = TYPE_TMS570_DCAN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570DcanState),
    .instance_init = dcan_init,
    .instance_finalize = dcan_finalize,
    .class_init = dcan_class_init,
};

static void dcan_register_types(void)
{
    type_register_static(&dcan_info);
}
type_init(dcan_register_types)
