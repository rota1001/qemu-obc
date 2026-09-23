/* TMS570 DMA subset: SCI and MibSPI frame/block transfers and BTC interrupt.
 * Local OBC100 experiment, not intended for upstream.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/dma/tms570_dma.h"
#include "hw/core/irq.h"
#include "qemu/log.h"
#include "system/address-spaces.h"

#define DMA_GCTRL       0x000
#define DMA_HWCHENAS    0x014
#define DMA_HWCHENAR    0x01c
#define DMA_GCHIENAS    0x044
#define DMA_GCHIENAR    0x04c
#define DMA_DREQASI     0x054
#define DMA_BTCMAP      0x0cc
#define DMA_BTCINTENAS  0x10c
#define DMA_BTCINTENAR  0x114
#define DMA_BTCFLAG     0x13c
#define DMA_BTCAOFFSET  0x158

static void dma_update_irq(TMS570DmaState *s)
{
    qemu_set_irq(s->btca, !!(s->btc_pending & s->btc_enabled &
                            s->global_irq_enabled & ~s->reg[DMA_BTCMAP / 4]));
}

static void dma_arm(TMS570DmaState *s, unsigned ch)
{
    s->current_src[ch] = s->packet[ch * 8];
    s->current_dst[ch] = s->packet[ch * 8 + 1];
    s->frames_left[ch] = s->packet[ch * 8 + 2] >> 16;
}

static void dma_transfer(TMS570DmaState *s, unsigned ch, bool block)
{
    unsigned p = ch * 8;
    uint32_t count = s->packet[p + 2];
    uint32_t ctrl = s->packet[p + 4];
    unsigned frames = block || (ctrl & 0x100) ? s->frames_left[ch] : 1;
    unsigned elements = count & 0x1fff;
    unsigned rsize = 1u << ((ctrl >> 14) & 3);
    unsigned wsize = 1u << ((ctrl >> 12) & 3);
    unsigned rmode = (ctrl >> 3) & 3, wmode = (ctrl >> 1) & 3;
    int roff = (int16_t)s->packet[p + 5];
    int woff = (int16_t)(s->packet[p + 5] >> 16);
    int rframe = (int16_t)s->packet[p + 6];
    int wframe = (int16_t)(s->packet[p + 6] >> 16);
    uint8_t bytes[8];

    if (!s->frames_left[ch] || !elements || rsize != wsize ||
        rmode == 2 || wmode == 2) {
        qemu_log_mask(LOG_UNIMP, "tms570-dma: unsupported packet ch=%u "
                      "count=%08x ctrl=%08x\n", ch, count, ctrl);
        return;
    }
    for (unsigned f = 0; f < frames; f++) {
        uint32_t src = s->current_src[ch], dst = s->current_dst[ch];
        for (unsigned i = 0; i < elements; i++) {
            if (address_space_read(&address_space_memory, src,
                                   MEMTXATTRS_UNSPECIFIED, bytes, rsize) != MEMTX_OK ||
                address_space_write(&address_space_memory, dst,
                                    MEMTXATTRS_UNSPECIFIED, bytes, wsize) != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "tms570-dma: bus fault ch=%u "
                              "src=%08x dst=%08x\n", ch, src, dst);
                return;
            }
            src += rmode == 1 ? rsize : rmode == 3 ? roff : 0;
            dst += wmode == 1 ? wsize : wmode == 3 ? woff : 0;
        }
        s->current_src[ch] = rmode == 3 ? s->current_src[ch] + rframe : src;
        s->current_dst[ch] = wmode == 3 ? s->current_dst[ch] + wframe : dst;
        s->frames_left[ch]--;
    }
    if (!s->frames_left[ch]) {
        s->hw_enabled &= ~(1u << ch);
        s->btc_pending |= 1u << ch;
        if (ctrl & 1) {
            dma_arm(s, ch);
            s->hw_enabled |= 1u << ch;
        }
    }
    dma_update_irq(s);
}

void tms570_dma_request(TMS570DmaState *s, unsigned request)
{
    if (!s || !(s->reg[DMA_GCTRL / 4] & 0x10000)) {
        return;
    }
    for (unsigned ch = 0; ch < 32; ch++) {
        unsigned shift = (3 - ch % 4) * 8;
        unsigned req = (s->reg[(DMA_DREQASI / 4) + ch / 4] >> shift) & 0xff;
        if ((s->hw_enabled & (1u << ch)) && req == request) {
            dma_transfer(s, ch, false);
        }
    }
}

static uint64_t dma_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570DmaState *s = opaque;
    uint32_t value;

    switch (addr) {
    case DMA_HWCHENAS:
    case DMA_HWCHENAR:
        return s->hw_enabled;
    case DMA_GCHIENAS:
    case DMA_GCHIENAR:
        return s->global_irq_enabled;
    case DMA_BTCINTENAS:
    case DMA_BTCINTENAR:
        return s->btc_enabled;
    case DMA_BTCFLAG:
        return s->btc_pending;
    case DMA_BTCAOFFSET:
        if (s->btc_pending) {
            unsigned ch = ctz32(s->btc_pending);
            s->btc_pending &= ~(1u << ch);
            dma_update_irq(s);
            return ch + 1;
        }
        return 0;
    default:
        value = s->reg[addr / 4];
        return value;
    }
}

static void dma_reg_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    TMS570DmaState *s = opaque;
    uint32_t v = val;

    switch (addr) {
    case DMA_HWCHENAS:
        s->hw_enabled |= v;
        for (unsigned ch = 0; ch < 32; ch++) {
            if (v & (1u << ch)) {
                dma_arm(s, ch);
                unsigned shift = (3 - ch % 4) * 8;
                unsigned req = (s->reg[(DMA_DREQASI / 4) + ch / 4] >> shift) & 0xff;
                uint32_t dst = s->packet[ch * 8 + 1];
                /* SCI TX is always ready. Shared MibSPI requests must wait
                 * for the SPI transfer group, not run when merely armed. */
                if ((req == 29 && dst == 0xfff7e43b) ||
                    (req == 31 && dst == 0xfff7e53b) ||
                    (req == 43 && dst == 0xfff7e73b)) {
                    dma_transfer(s, ch, true);
                }
            }
        }
        return;
    case DMA_HWCHENAR:
        s->hw_enabled &= ~v;
        return;
    case 0x24: /* Software request: service the programmed packet. */
        for (unsigned ch = 0; ch < 32; ch++) {
            if (v & (1u << ch)) {
                dma_arm(s, ch);
                dma_transfer(s, ch, true);
            }
        }
        return;
    case DMA_GCHIENAS:
        s->global_irq_enabled |= v;
        dma_update_irq(s);
        return;
    case DMA_GCHIENAR:
        s->global_irq_enabled &= ~v;
        dma_update_irq(s);
        return;
    case DMA_BTCINTENAS:
        s->btc_enabled |= v;
        dma_update_irq(s);
        return;
    case DMA_BTCINTENAR:
        s->btc_enabled &= ~v;
        dma_update_irq(s);
        return;
    case DMA_BTCFLAG:
        s->btc_pending &= ~v;
        dma_update_irq(s);
        return;
    default:
        s->reg[addr / 4] = v;
        return;
    }
}

static uint64_t dma_ram_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570DmaState *s = opaque;
    return s->packet[addr / 4];
}

static void dma_ram_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    TMS570DmaState *s = opaque;
    s->packet[addr / 4] = val;
}

static const MemoryRegionOps dma_reg_ops = {
    .read = dma_reg_read, .write = dma_reg_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps dma_ram_ops = {
    .read = dma_ram_read, .write = dma_ram_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void dma_reset(DeviceState *dev)
{
    TMS570DmaState *s = TMS570_DMA(dev);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->packet, 0, sizeof(s->packet));
    memset(s->frames_left, 0, sizeof(s->frames_left));
    s->hw_enabled = 0;
    s->btc_enabled = 0;
    s->global_irq_enabled = 0;
    s->btc_pending = 0;
    dma_update_irq(s);
}

static void dma_init(Object *obj)
{
    TMS570DmaState *s = TMS570_DMA(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->regs, obj, &dma_reg_ops, s, "tms570-dma", 0x400);
    memory_region_init_io(&s->ram, obj, &dma_ram_ops, s, "tms570-dma-ram", 0x1000);
    sysbus_init_mmio(sbd, &s->regs);
    sysbus_init_mmio(sbd, &s->ram);
    sysbus_init_irq(sbd, &s->btca);
}

static void dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, dma_reset);
    dc->desc = "TMS570 DMA (SCI and MibSPI frame transfers)";
}

static const TypeInfo dma_info = {
    .name = TYPE_TMS570_DMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570DmaState),
    .instance_init = dma_init,
    .class_init = dma_class_init,
};

static void dma_register_types(void)
{
    type_register_static(&dma_info);
}
type_init(dma_register_types)
