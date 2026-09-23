/*
 * TI TMS570 General purpose I/O (GIO)
 *
 * Two 8-bit ports plus the shared interrupt block.  The same port register
 * layout is reused by several other peripherals (HET, MibSPI, I2C, SCI) for
 * their pin control; only the GIO instance is modelled here.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "hw/gpio/tms570_gio.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

REG32(GCR0,   0x00)
REG32(INTDET, 0x08)
REG32(POL,    0x0C)
REG32(ENASET, 0x10)
REG32(ENACLR, 0x14)
REG32(LVLSET, 0x18)
REG32(LVLCLR, 0x1C)
REG32(FLG,    0x20)
REG32(OFF1,   0x24)
REG32(OFF2,   0x28)
REG32(EMU1,   0x2C)
REG32(EMU2,   0x30)

#define GIO_PORTA_BASE 0x34
#define GIO_PORT_SIZE  0x20

/* Per-port register offsets. */
#define PORT_DIR    0x00
#define PORT_DIN    0x04
#define PORT_DOUT   0x08
#define PORT_DSET   0x0C
#define PORT_DCLR   0x10
#define PORT_PDR    0x14
#define PORT_PULDIS 0x18
#define PORT_PSL    0x1C

static void gio_update_irq(TMS570GioState *s)
{
    uint32_t active = s->flg & s->ena;

    qemu_set_irq(s->irq_high, (active & s->lvl) != 0);
    qemu_set_irq(s->irq_low, (active & ~s->lvl) != 0);
}

static uint32_t gio_offset(uint32_t pending)
{
    return pending ? ctz32(pending) + 1 : 0;
}

/* Drive the outgoing GPIO lines for whichever pins are configured as output. */
static void gio_update_port(TMS570GioState *s, int p)
{
    TMS570GioPort *port = &s->port[p];
    int i;

    for (i = 0; i < TMS570_GIO_PORT_PINS; i++) {
        if (port->dir & (1u << i)) {
            qemu_set_irq(s->out[p * TMS570_GIO_PORT_PINS + i],
                         (port->dout >> i) & 1);
        }
    }
}

static uint64_t gio_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570GioState *s = TMS570_GIO(opaque);
    int p;

    if (addr >= GIO_PORTA_BASE &&
        addr < GIO_PORTA_BASE + TMS570_GIO_NUM_PORTS * GIO_PORT_SIZE) {
        hwaddr off = addr - GIO_PORTA_BASE;
        TMS570GioPort *port;

        p = off / GIO_PORT_SIZE;
        off %= GIO_PORT_SIZE;
        port = &s->port[p];

        switch (off) {
        case PORT_DIR:
            return port->dir;
        case PORT_DIN:
            return (((port->din & port->driven) |
                     (port->psl & ~port->puldis & ~port->driven)) & ~port->dir) |
                   (port->dout & port->dir);
        case PORT_DOUT:
        case PORT_DSET:
        case PORT_DCLR:
            return port->dout;
        case PORT_PDR:
            return port->pdr;
        case PORT_PULDIS:
            return port->puldis;
        case PORT_PSL:
            return port->psl;
        default:
            return 0;
        }
    }

    switch (addr) {
    case A_GCR0:
        return s->gcr0;
    case A_INTDET:
        return s->intdet;
    case A_POL:
        return s->pol;
    case A_ENASET:
    case A_ENACLR:
        return s->ena;
    case A_LVLSET:
    case A_LVLCLR:
        return s->lvl;
    case A_FLG:
        return s->flg;
    case A_OFF1:
    case A_OFF2: {
        uint32_t pending = s->flg & s->ena & (addr == A_OFF1 ? s->lvl : ~s->lvl);
        unsigned offset = gio_offset(pending);
        if (offset) {
            s->flg &= ~(1u << (offset - 1));
            gio_update_irq(s);
        }
        return offset;
    }
    case A_EMU1:
        return gio_offset(s->flg & s->ena & s->lvl);
    case A_EMU2:
        return gio_offset(s->flg & s->ena & ~s->lvl);
    default:
        break;
    }

    qemu_log_mask(LOG_UNIMP, "tms570-gio: read from unhandled 0x%" HWADDR_PRIx
                  "\n", addr);
    return 0;
}

static void gio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TMS570GioState *s = TMS570_GIO(opaque);
    uint32_t v = val;
    int p;

    if (addr >= GIO_PORTA_BASE &&
        addr < GIO_PORTA_BASE + TMS570_GIO_NUM_PORTS * GIO_PORT_SIZE) {
        hwaddr off = addr - GIO_PORTA_BASE;
        TMS570GioPort *port;

        p = off / GIO_PORT_SIZE;
        off %= GIO_PORT_SIZE;
        port = &s->port[p];

        switch (off) {
        case PORT_DIR:
            port->dir = v & 0xFF;
            break;
        case PORT_DOUT:
            port->dout = v & 0xFF;
            break;
        case PORT_DSET:
            port->dout |= v & 0xFF;
            break;
        case PORT_DCLR:
            port->dout &= ~(v & 0xFF);
            break;
        case PORT_PDR:
            port->pdr = v & 0xFF;
            return;
        case PORT_PULDIS:
            port->puldis = v & 0xFF;
            return;
        case PORT_PSL:
            port->psl = v & 0xFF;
            return;
        default:
            return;
        }
        gio_update_port(s, p);
        return;
    }

    switch (addr) {
    case A_GCR0:
        s->gcr0 = v & 1;
        return;
    case A_INTDET:
        s->intdet = v;
        return;
    case A_POL:
        s->pol = v;
        return;
    case A_ENASET:
        s->ena |= v;
        gio_update_irq(s);
        return;
    case A_ENACLR:
        s->ena &= ~v;
        gio_update_irq(s);
        return;
    case A_LVLSET:
        s->lvl |= v;
        gio_update_irq(s);
        return;
    case A_LVLCLR:
        s->lvl &= ~v;
        gio_update_irq(s);
        return;
    case A_FLG:
        s->flg &= ~v;   /* write one to clear */
        gio_update_irq(s);
        return;
    case A_EMU1:
        s->emu1 = v;
        return;
    case A_EMU2:
        s->emu2 = v;
        return;
    case A_OFF1:
    case A_OFF2:
        return;  /* read-only */
    default:
        break;
    }

    qemu_log_mask(LOG_UNIMP, "tms570-gio: write 0x%08x to unhandled 0x%"
                  HWADDR_PRIx "\n", v, addr);
}

static const MemoryRegionOps gio_ops = {
    .read = gio_read,
    .write = gio_write,
    /*
     * The SoC runs big-endian and the CPU's data accesses are big-endian too
     * (see tms570lc43_realize()), so registers pass through unswapped.
     */
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* Incoming pin changes, numbered port * 8 + pin. */
static void gio_set_input(void *opaque, int line, int level)
{
    TMS570GioState *s = TMS570_GIO(opaque);
    int p = line / TMS570_GIO_PORT_PINS;
    int pin = line % TMS570_GIO_PORT_PINS;
    uint32_t bit = 1u << pin;
    bool old;

    if (p >= TMS570_GIO_NUM_PORTS) {
        return;
    }

    old = (s->port[p].din & bit) != 0;
    s->port[p].driven |= bit;
    if (level) {
        s->port[p].din |= bit;
    } else {
        s->port[p].din &= ~bit;
    }

    /* GIOA occupies interrupt bits 0..7, GIOB bits 8..15. */
    if (!(s->port[p].dir & bit)) {
        uint32_t irqbit = 1u << line;
        bool rising = !old && level;
        bool falling = old && !level;
        bool edge_sel_rising = (s->pol & irqbit) != 0;

        if ((s->intdet & irqbit) ? (rising || falling)
                              : (edge_sel_rising ? rising : falling)) {
            s->flg |= irqbit;
            gio_update_irq(s);
        }
    }
}

static void gio_reset(DeviceState *dev)
{
    TMS570GioState *s = TMS570_GIO(dev);

    s->gcr0 = 0;
    s->intdet = 0;
    s->pol = 0;
    s->ena = 0;
    s->lvl = 0;
    s->flg = 0;
    s->emu1 = 0;
    s->emu2 = 0;
    memset(s->port, 0, sizeof(s->port));
    gio_update_irq(s);
}

static void gio_instance_init(Object *obj)
{
    TMS570GioState *s = TMS570_GIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &gio_ops, s, "tms570-gio", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_high);
    sysbus_init_irq(sbd, &s->irq_low);

    qdev_init_gpio_in(DEVICE(obj), gio_set_input,
                      TMS570_GIO_NUM_PORTS * TMS570_GIO_PORT_PINS);
    qdev_init_gpio_out(DEVICE(obj), s->out,
                       TMS570_GIO_NUM_PORTS * TMS570_GIO_PORT_PINS);
}

static const VMStateDescription vmstate_tms570_gio_port = {
    .name = "tms570-gio-port",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dir, TMS570GioPort),
        VMSTATE_UINT32(din, TMS570GioPort),
        VMSTATE_UINT32(dout, TMS570GioPort),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_tms570_gio = {
    .name = "tms570-gio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gcr0, TMS570GioState),
        VMSTATE_UINT32(ena, TMS570GioState),
        VMSTATE_UINT32(lvl, TMS570GioState),
        VMSTATE_UINT32(flg, TMS570GioState),
        VMSTATE_STRUCT_ARRAY(port, TMS570GioState, TMS570_GIO_NUM_PORTS, 1,
                             vmstate_tms570_gio_port, TMS570GioPort),
        VMSTATE_END_OF_LIST()
    }
};

static void gio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, gio_reset);
    dc->vmsd = &vmstate_tms570_gio;
    dc->desc = "TI TMS570 general purpose I/O";
}

static const TypeInfo tms570_gio_info = {
    .name          = TYPE_TMS570_GIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570GioState),
    .instance_init = gio_instance_init,
    .class_init    = gio_class_init,
};

static void tms570_gio_register_types(void)
{
    type_register_static(&tms570_gio_info);
}

type_init(tms570_gio_register_types)
