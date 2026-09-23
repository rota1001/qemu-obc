/*
 * TI TMS570 SCI (also the LIN module in SCI compatibility mode)
 *
 * Transmission completes instantly, so TXRDY is essentially always set.  The
 * one subtlety is the transmit buffer: the OBC100 console driver does not
 * write TD as a word, it points a DMA channel at &sciREG1->TD + 3.  On this
 * big-endian part that byte address is the least significant byte of TD, so
 * byte-sized writes anywhere in the TD word have to be accepted and treated as
 * the character to send.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "hw/char/tms570_sci.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

REG32(GCR0,        0x00)
REG32(GCR1,        0x04)
REG32(GCR2,        0x08)
REG32(SETINT,      0x0C)
REG32(CLEARINT,    0x10)
REG32(SETINTLVL,   0x14)
REG32(CLEARINTLVL, 0x18)
REG32(FLR,         0x1C)
REG32(INTVECT0,    0x20)
REG32(INTVECT1,    0x24)
REG32(FORMAT,      0x28)
REG32(BRS,         0x2C)
REG32(ED,          0x30)
REG32(RD,          0x34)
REG32(TD,          0x38)
#define A_PIO0     0x3C
REG32(IODFTCTRL,   0x90)

/* FLR / SETINT bit assignments shared by both registers. */
#define SCI_BRKDT   (1u << 0)
#define SCI_WAKEUP  (1u << 1)
#define SCI_TXRDY   (1u << 8)
#define SCI_RXRDY   (1u << 9)
#define SCI_TXWAKE  (1u << 10)
#define SCI_IDLE    (1u << 11)
#define SCI_BUSY    (1u << 12)
#define SCI_TXEMPTY (1u << 13)

/* GCR1 */
#define SCI_GCR1_RXENA  (1u << 24)
#define SCI_GCR1_TXENA  (1u << 25)
#define SCI_GCR1_SWNRST (1u << 7)

static void sci_update_irq(TMS570SciState *s)
{
    uint32_t active = s->flr & s->setint;

    /* SETINTLVL=0 selects level 0 (the high VIM channel). */
    qemu_set_irq(s->irq_high, (active & ~s->setintlvl) != 0);
    qemu_set_irq(s->irq_low, (active & s->setintlvl) != 0);
}

/*
 * INTVECT reports the highest priority pending source as an offset.  The HAL
 * only uses it from the ISR to decide which event fired.
 */
static uint32_t sci_intvect(TMS570SciState *s, bool high)
{
    uint32_t active = s->flr & s->setint;

    active &= high ? ~s->setintlvl : s->setintlvl;
    if (active & SCI_RXRDY) {
        return 11;
    }
    if (active & SCI_TXRDY) {
        return 12;
    }
    if (active & SCI_WAKEUP) {
        return 1;
    }
    if (active & SCI_BRKDT) {
        return 13;
    }
    return 0;
}

static void sci_send(TMS570SciState *s, uint8_t ch)
{
    /* Writes are unbuffered: hand the byte straight to the backend. */
    qemu_chr_fe_write_all(&s->chr, &ch, 1);
    s->flr |= SCI_TXRDY | SCI_TXEMPTY;
    sci_update_irq(s);
}

static uint64_t sci_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570SciState *s = TMS570_SCI(opaque);
    uint32_t val;

    switch (addr) {
    case A_GCR0:
        return s->gcr0;
    case A_GCR1:
        return s->gcr1;
    case A_GCR2:
        return s->gcr2;
    case A_SETINT:
    case A_CLEARINT:
        return s->setint;
    case A_SETINTLVL:
    case A_CLEARINTLVL:
        return s->setintlvl;
    case A_FLR:
        return s->flr;
    case A_INTVECT0:
        return sci_intvect(s, true);
    case A_INTVECT1:
        return sci_intvect(s, false);
    case A_FORMAT:
        return s->format;
    case A_BRS:
        return s->brs;
    case A_ED:
        return s->ed;
    case A_RD:
        val = s->rd;
        s->rd = 0;
        s->rx_full = false;
        s->flr &= ~SCI_RXRDY;
        sci_update_irq(s);
        qemu_chr_fe_accept_input(&s->chr);
        return val;
    case A_TD:
        return s->td;
    case A_IODFTCTRL:
        return s->iodftctrl;
    default:
        break;
    }

    if (addr >= A_PIO0 && addr < A_PIO0 + 9 * 4) {
        return s->pio[(addr - A_PIO0) / 4];
    }

    qemu_log_mask(LOG_UNIMP, "tms570-sci: read from unhandled 0x%" HWADDR_PRIx
                  "\n", addr);
    return 0;
}

static void sci_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TMS570SciState *s = TMS570_SCI(opaque);
    uint32_t v = val;

    /*
     * Byte and halfword writes land anywhere inside a register.  Only TD is
     * actually accessed that way (by the console DMA channel), so fold any
     * sub-word write within the TD word onto TD itself and use the low byte.
     */
    if (size < 4 && addr >= A_TD && addr < A_TD + 4) {
        sci_send(s, val & 0xFF);
        s->td = val & 0xFF;
        return;
    }

    switch (addr) {
    case A_GCR0:
        s->gcr0 = v & 1;
        return;
    case A_GCR1: {
        bool rx_was_off = !(s->gcr1 & SCI_GCR1_RXENA);

        s->gcr1 = v;
        /* Leaving software reset arms the transmitter. */
        if (v & SCI_GCR1_SWNRST) {
            s->flr |= SCI_TXRDY | SCI_TXEMPTY | SCI_IDLE;
        }
        /*
         * The front end stops polling once sci_can_receive() has refused a
         * character, and only resumes when we accept input again.  Receive is
         * disabled out of reset, so enabling it has to kick that off or no
         * input ever arrives.
         */
        if (rx_was_off && (v & SCI_GCR1_RXENA)) {
            qemu_chr_fe_accept_input(&s->chr);
        }
        sci_update_irq(s);
        return;
    }
    case A_GCR2:
        s->gcr2 = v;
        return;
    case A_SETINT:
        s->setint |= v;
        sci_update_irq(s);
        return;
    case A_CLEARINT:
        s->setint &= ~v;
        sci_update_irq(s);
        return;
    case A_SETINTLVL:
        s->setintlvl |= v;
        sci_update_irq(s);
        return;
    case A_CLEARINTLVL:
        s->setintlvl &= ~v;
        sci_update_irq(s);
        return;
    case A_FLR:
        /*
         * Write one to clear, but TXRDY/TXEMPTY are status of an always-idle
         * transmitter and stay set.
         */
        s->flr &= ~(v & ~(SCI_TXRDY | SCI_TXEMPTY));
        sci_update_irq(s);
        return;
    case A_FORMAT:
        s->format = v;
        return;
    case A_BRS:
        s->brs = v;
        return;
    case A_ED:
        s->ed = v;
        return;
    case A_TD:
        sci_send(s, v & 0xFF);
        s->td = v & 0xFF;
        return;
    case A_RD:
    case A_INTVECT0:
    case A_INTVECT1:
        return;  /* read-only */
    case A_IODFTCTRL:
        s->iodftctrl = v;
        return;
    default:
        break;
    }

    if (addr >= A_PIO0 && addr < A_PIO0 + 9 * 4) {
        s->pio[(addr - A_PIO0) / 4] = v;
        return;
    }

    qemu_log_mask(LOG_UNIMP, "tms570-sci: write 0x%08x to unhandled 0x%"
                  HWADDR_PRIx "\n", v, addr);
}

static const MemoryRegionOps sci_ops = {
    .read = sci_read,
    .write = sci_write,
    /*
     * The SoC runs big-endian and the CPU's data accesses are big-endian too
     * (see tms570lc43_realize()), so registers pass through unswapped.
     */
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static int sci_can_receive(void *opaque)
{
    TMS570SciState *s = TMS570_SCI(opaque);

    return (!s->rx_full && (s->gcr1 & SCI_GCR1_RXENA)) ? 1 : 0;
}

static void sci_receive(void *opaque, const uint8_t *buf, int size)
{
    TMS570SciState *s = TMS570_SCI(opaque);

    s->rd = buf[0];
    s->rx_full = true;
    s->flr |= SCI_RXRDY;
    sci_update_irq(s);
}

static void sci_reset(DeviceState *dev)
{
    TMS570SciState *s = TMS570_SCI(dev);

    s->gcr0 = 0;
    s->gcr1 = 0;
    s->gcr2 = 0;
    s->setint = 0;
    s->setintlvl = 0;
    s->flr = SCI_TXRDY | SCI_TXEMPTY | SCI_IDLE;
    s->format = 0;
    s->brs = 0;
    s->ed = 0;
    s->rd = 0;
    s->td = 0;
    s->iodftctrl = 0;
    s->rx_full = false;
    memset(s->pio, 0, sizeof(s->pio));
    sci_update_irq(s);
}

static void sci_realize(DeviceState *dev, Error **errp)
{
    TMS570SciState *s = TMS570_SCI(dev);

    qemu_chr_fe_set_handlers(&s->chr, sci_can_receive, sci_receive,
                             NULL, NULL, s, NULL, true);
}

static void sci_instance_init(Object *obj)
{
    TMS570SciState *s = TMS570_SCI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sci_ops, s, "tms570-sci", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_high);
    sysbus_init_irq(sbd, &s->irq_low);
}

static const Property sci_properties[] = {
    DEFINE_PROP_CHR("chardev", TMS570SciState, chr),
};

static const VMStateDescription vmstate_tms570_sci = {
    .name = "tms570-sci",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gcr0, TMS570SciState),
        VMSTATE_UINT32(gcr1, TMS570SciState),
        VMSTATE_UINT32(setint, TMS570SciState),
        VMSTATE_UINT32(setintlvl, TMS570SciState),
        VMSTATE_UINT32(flr, TMS570SciState),
        VMSTATE_UINT32(rd, TMS570SciState),
        VMSTATE_UINT32(td, TMS570SciState),
        VMSTATE_BOOL(rx_full, TMS570SciState),
        VMSTATE_END_OF_LIST()
    }
};

static void sci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sci_realize;
    device_class_set_legacy_reset(dc, sci_reset);
    device_class_set_props(dc, sci_properties);
    dc->vmsd = &vmstate_tms570_sci;
    dc->desc = "TI TMS570 SCI/LIN serial port";
}

static const TypeInfo tms570_sci_info = {
    .name          = TYPE_TMS570_SCI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570SciState),
    .instance_init = sci_instance_init,
    .class_init    = sci_class_init,
};

static void tms570_sci_register_types(void)
{
    type_register_static(&tms570_sci_info);
}

type_init(tms570_sci_register_types)
