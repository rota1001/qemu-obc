/*
 * TI TMS570 Vectored Interrupt Manager (VIM)
 *
 * Channels 0..127, each routed to either IRQ or FIQ by FIRQPR.  Lower channel
 * numbers win arbitration.  The firmware's vector table (HL_sys_intvecs.asm)
 * does not branch to a fixed handler; it executes
 *
 *     ldr pc, [pc, #-0x1b0]
 *
 * at offsets 0x18/0x1C, which reads IRQVECREG/FIQVECREG at 0xFFFFFE70/74.
 * That makes IRQVECREG the critical register here: it must return the handler
 * address the guest previously placed in the vector RAM at 0xFFF82000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "hw/intc/tms570_vim.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "trace.h"

REG32(ECCSTAT,      0x0EC)
REG32(ECCCTL,       0x0F0)
REG32(UERRADDR,     0x0F4)
REG32(FBVECADDR,    0x0F8)
REG32(SBERRADDR,    0x0FC)
REG32(IRQINDEX,     0x100)
REG32(FIQINDEX,     0x104)
#define A_FIRQPR0       0x110
#define A_INTREQ0       0x120
#define A_REQMASKSET0   0x130
#define A_REQMASKCLR0   0x140
#define A_WAKEMASKSET0  0x150
#define A_WAKEMASKCLR0  0x160
REG32(IRQVECREG,    0x170)
REG32(FIQVECREG,    0x174)
REG32(CAPEVT,       0x178)
#define A_CHANCTRL0     0x180

/*
 * Channels 0 and 1 are hard-wired to FIQ on this device; the rest follow
 * FIRQPR.  Returns true when the channel is an FIQ source.
 */
static bool vim_is_fiq(TMS570VimState *s, int ch)
{
    if (ch < 2) {
        return true;
    }
    return (s->firqpr[ch / 32] >> (ch % 32)) & 1;
}

static bool vim_pending(TMS570VimState *s, int ch)
{
    uint32_t bit = 1u << (ch % 32);
    return (s->intreq[ch / 32] & s->reqmask[ch / 32] & bit) != 0;
}

/* Lowest pending channel of the requested kind, or -1. */
static int vim_highest(TMS570VimState *s, bool want_fiq)
{
    for (int ch = 0; ch < TMS570_VIM_NUM_CHANNELS; ch++) {
        if (vim_pending(s, ch) && vim_is_fiq(s, ch) == want_fiq) {
            return ch;
        }
    }
    return -1;
}

static void vim_update(TMS570VimState *s)
{
    qemu_set_irq(s->irq, vim_highest(s, false) >= 0);
    qemu_set_irq(s->fiq, vim_highest(s, true) >= 0);
}

/*
 * Incoming interrupt lines.  The VIM latches a level into INTREQ; software
 * clears the condition at the source, which drops the line again.
 */
static void vim_set_irq(void *opaque, int ch, int level)
{
    TMS570VimState *s = TMS570_VIM(opaque);
    uint32_t bit = 1u << (ch % 32);
    int word = ch / 32;

    if (level) {
        s->raw[word] |= bit;
        s->intreq[word] |= bit;
    } else {
        s->raw[word] &= ~bit;
        s->intreq[word] &= ~bit;
    }
    vim_update(s);
}

static uint64_t vim_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570VimState *s = TMS570_VIM(opaque);
    int i;

    switch (addr) {
    case A_ECCSTAT:
    case A_UERRADDR:
    case A_SBERRADDR:
        return 0;
    case A_ECCCTL:
        return s->eccctl;
    case A_FBVECADDR:
        return s->fbvecaddr;
    case A_CAPEVT:
        return s->capevt;

    case A_IRQINDEX: {
        int ch = vim_highest(s, false);
        return ch < 0 ? 0 : ch + 1;
    }
    case A_FIQINDEX: {
        int ch = vim_highest(s, true);
        return ch < 0 ? 0 : ch + 1;
    }

    /*
     * Reading the vector register is what the IRQ/FIQ vector does.  Entry 0
     * of the vector RAM is the phantom handler, used when nothing is pending.
     */
    case A_IRQVECREG: {
        int ch = vim_highest(s, false);
        return s->vecram[ch < 0 ? 0 : ch + 1];
    }
    case A_FIQVECREG: {
        int ch = vim_highest(s, true);
        return s->vecram[ch < 0 ? 0 : ch + 1];
    }

    default:
        break;
    }

    if (addr >= A_FIRQPR0 && addr < A_FIRQPR0 + 0x10) {
        return s->firqpr[(addr - A_FIRQPR0) / 4];
    }
    if (addr >= A_INTREQ0 && addr < A_INTREQ0 + 0x10) {
        return s->intreq[(addr - A_INTREQ0) / 4];
    }
    /* Both the set and clear aliases read back the current mask. */
    if (addr >= A_REQMASKSET0 && addr < A_REQMASKSET0 + 0x10) {
        return s->reqmask[(addr - A_REQMASKSET0) / 4];
    }
    if (addr >= A_REQMASKCLR0 && addr < A_REQMASKCLR0 + 0x10) {
        return s->reqmask[(addr - A_REQMASKCLR0) / 4];
    }
    if (addr >= A_WAKEMASKSET0 && addr < A_WAKEMASKSET0 + 0x10) {
        return s->wakemask[(addr - A_WAKEMASKSET0) / 4];
    }
    if (addr >= A_WAKEMASKCLR0 && addr < A_WAKEMASKCLR0 + 0x10) {
        return s->wakemask[(addr - A_WAKEMASKCLR0) / 4];
    }
    if (addr >= A_CHANCTRL0 && addr < A_CHANCTRL0 + sizeof(s->chanctrl)) {
        i = (addr - A_CHANCTRL0) / 4;
        return s->chanctrl[i];
    }

    qemu_log_mask(LOG_UNIMP, "tms570-vim: read from unhandled 0x%" HWADDR_PRIx
                  "\n", addr);
    return 0;
}

static void vim_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TMS570VimState *s = TMS570_VIM(opaque);
    uint32_t v = val;
    int i;

    switch (addr) {
    case A_ECCCTL:
        s->eccctl = v;
        return;
    case A_FBVECADDR:
        s->fbvecaddr = v;
        return;
    case A_CAPEVT:
        s->capevt = v;
        return;
    case A_ECCSTAT:
    case A_UERRADDR:
    case A_SBERRADDR:
    case A_IRQINDEX:
    case A_FIQINDEX:
    case A_IRQVECREG:
    case A_FIQVECREG:
        return; /* read-only */
    default:
        break;
    }

    if (addr >= A_FIRQPR0 && addr < A_FIRQPR0 + 0x10) {
        s->firqpr[(addr - A_FIRQPR0) / 4] = v;
        vim_update(s);
        return;
    }
    if (addr >= A_INTREQ0 && addr < A_INTREQ0 + 0x10) {
        /*
         * INTREQ is status; writes are ignored.  A pending request goes away
         * when the peripheral deasserts its line.
         */
        return;
    }
    if (addr >= A_REQMASKSET0 && addr < A_REQMASKSET0 + 0x10) {
        i = (addr - A_REQMASKSET0) / 4;
        s->reqmask[i] |= v;
        vim_update(s);
        return;
    }
    if (addr >= A_REQMASKCLR0 && addr < A_REQMASKCLR0 + 0x10) {
        i = (addr - A_REQMASKCLR0) / 4;
        s->reqmask[i] &= ~v;
        vim_update(s);
        return;
    }
    if (addr >= A_WAKEMASKSET0 && addr < A_WAKEMASKSET0 + 0x10) {
        s->wakemask[(addr - A_WAKEMASKSET0) / 4] |= v;
        return;
    }
    if (addr >= A_WAKEMASKCLR0 && addr < A_WAKEMASKCLR0 + 0x10) {
        s->wakemask[(addr - A_WAKEMASKCLR0) / 4] &= ~v;
        return;
    }
    if (addr >= A_CHANCTRL0 && addr < A_CHANCTRL0 + sizeof(s->chanctrl)) {
        s->chanctrl[(addr - A_CHANCTRL0) / 4] = v;
        return;
    }

    qemu_log_mask(LOG_UNIMP, "tms570-vim: write 0x%08x to unhandled 0x%"
                  HWADDR_PRIx "\n", v, addr);
}

static const MemoryRegionOps vim_ops = {
    .read = vim_read,
    .write = vim_write,
    /*
     * The SoC runs big-endian and the CPU's data accesses are big-endian too
     * (see tms570lc43_realize()), so registers pass through unswapped.
     */
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* The vector RAM is plain storage; back it with the state array. */
static uint64_t vim_ram_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570VimState *s = TMS570_VIM(opaque);

    if (addr / 4 >= TMS570_VIM_RAM_ENTRIES) {
        return 0;
    }
    return s->vecram[addr / 4];
}

static void vim_ram_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    TMS570VimState *s = TMS570_VIM(opaque);

    if (addr / 4 >= TMS570_VIM_RAM_ENTRIES) {
        return;
    }
    s->vecram[addr / 4] = val;
}

static const MemoryRegionOps vim_ram_ops = {
    .read = vim_ram_read,
    .write = vim_ram_write,
    /*
     * The SoC runs big-endian and the CPU's data accesses are big-endian too
     * (see tms570lc43_realize()), so registers pass through unswapped.
     */
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void vim_reset(DeviceState *dev)
{
    TMS570VimState *s = TMS570_VIM(dev);

    memset(s->raw, 0, sizeof(s->raw));
    memset(s->firqpr, 0, sizeof(s->firqpr));
    memset(s->intreq, 0, sizeof(s->intreq));
    memset(s->reqmask, 0, sizeof(s->reqmask));
    memset(s->wakemask, 0xFF, sizeof(s->wakemask));
    memset(s->chanctrl, 0, sizeof(s->chanctrl));
    memset(s->vecram, 0, sizeof(s->vecram));
    s->capevt = 0;
    s->eccctl = 0;
    s->fbvecaddr = 0;
    vim_update(s);
}

static void vim_init(Object *obj)
{
    TMS570VimState *s = TMS570_VIM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &vim_ops, s, "tms570-vim", 0x200);
    sysbus_init_mmio(sbd, &s->iomem);

    memory_region_init_io(&s->ram, obj, &vim_ram_ops, s, "tms570-vim-ram",
                          TMS570_VIM_RAM_SIZE);
    sysbus_init_mmio(sbd, &s->ram);

    qdev_init_gpio_in(DEVICE(obj), vim_set_irq, TMS570_VIM_NUM_CHANNELS);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->fiq);
}

static const VMStateDescription vmstate_tms570_vim = {
    .name = "tms570-vim",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(raw, TMS570VimState, TMS570_VIM_NUM_WORDS),
        VMSTATE_UINT32_ARRAY(firqpr, TMS570VimState, TMS570_VIM_NUM_WORDS),
        VMSTATE_UINT32_ARRAY(intreq, TMS570VimState, TMS570_VIM_NUM_WORDS),
        VMSTATE_UINT32_ARRAY(reqmask, TMS570VimState, TMS570_VIM_NUM_WORDS),
        VMSTATE_UINT32_ARRAY(wakemask, TMS570VimState, TMS570_VIM_NUM_WORDS),
        VMSTATE_UINT32_ARRAY(chanctrl, TMS570VimState, 32),
        VMSTATE_UINT32_ARRAY(vecram, TMS570VimState, TMS570_VIM_RAM_ENTRIES),
        VMSTATE_UINT32(capevt, TMS570VimState),
        VMSTATE_UINT32(eccctl, TMS570VimState),
        VMSTATE_UINT32(fbvecaddr, TMS570VimState),
        VMSTATE_END_OF_LIST()
    }
};

static void vim_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, vim_reset);
    dc->vmsd = &vmstate_tms570_vim;
    dc->desc = "TI TMS570 Vectored Interrupt Manager";
}

static const TypeInfo tms570_vim_info = {
    .name          = TYPE_TMS570_VIM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570VimState),
    .instance_init = vim_init,
    .class_init    = vim_class_init,
};

static void tms570_vim_register_types(void)
{
    type_register_static(&tms570_vim_info);
}

type_init(tms570_vim_register_types)
