/* TMS570 SPI4 polled compatibility mode, local OBC100 experiment.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/ssi/tms570_spi.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "trace.h"
#include "qemu/error-report.h"
#include "hw/core/irq.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qapi/error.h"

#define R_GCR0 0x00
#define R_FLG  0x10
#define R_PC0  0x14
#define R_PC3  0x20
#define R_PC4  0x24
#define R_PC5  0x28
#define R_DAT0 0x38
#define R_DAT1 0x3c
#define R_BUF  0x40
#define R_TGFLG 0x84
#define R_TGCTRL 0x98
#define R_DMACTRL 0xd8

static void spi_irq(TMS570SpiState *s)
{
    uint32_t active = s->reg[R_TGFLG / 4] & s->reg[0x74 / 4];
    uint32_t level = s->reg[0x7c / 4];
    qemu_set_irq(s->irq[0], !!(active & ~level));
    qemu_set_irq(s->irq[1], !!(active & level));
}

static void sd_queue(TMS570SpiState *s, uint8_t value)
{
    if (s->response_len < sizeof(s->response)) {
        s->response[s->response_len++] = value;
    }
}

static void sd_block(TMS570SpiState *s, const uint8_t *data, unsigned len)
{
    unsigned i;
    sd_queue(s, 0xff);
    sd_queue(s, 0xfe);
    for (i = 0; i < len; i++) {
        sd_queue(s, data[i]);
    }
    sd_queue(s, 0xff);
    sd_queue(s, 0xff);
}

static void sd_command(TMS570SpiState *s)
{
    uint32_t arg = ((uint32_t)s->cmd[1] << 24) | ((uint32_t)s->cmd[2] << 16) |
                   ((uint32_t)s->cmd[3] << 8) | s->cmd[4];
    uint8_t cmd = s->cmd[0] & 0x3f;
    uint8_t data[512] = { 0 };
    int64_t size = s->drive ? blk_getlength(s->drive) : 0;
    uint32_t sectors = size > 0 ? size / 512 : 0;

    trace_tms570_sd_command(cmd, arg);

    s->response_pos = s->response_len = 0;
    switch (cmd) {
    case 0:
        s->idle = true;
        sd_queue(s, 1);
        break;
    case 8:
        sd_queue(s, s->idle ? 1 : 0);
        sd_queue(s, 0);
        sd_queue(s, 0);
        sd_queue(s, 1);
        sd_queue(s, 0xaa);
        break;
    case 55:
        s->app_cmd = true;
        sd_queue(s, s->idle ? 1 : 0);
        break;
    case 41:
        if (s->app_cmd) {
            s->idle = false;
            sd_queue(s, 0);
        } else {
            sd_queue(s, 4);
        }
        s->app_cmd = false;
        break;
    case 58:
        sd_queue(s, s->idle ? 1 : 0);
        sd_queue(s, 0xc0);
        sd_queue(s, 0xff);
        sd_queue(s, 0x80);
        sd_queue(s, 0);
        break;
    case 9:
        data[0] = 0x40; /* SDHC CSD version 2. */
        data[8] = ((sectors / 1024) - 1) >> 8;
        data[9] = (sectors / 1024) - 1;
        sd_queue(s, 0);
        sd_block(s, data, 16);
        break;
    case 17:
        sd_queue(s, 0);
        if (arg < sectors && blk_pread(s->drive, (int64_t)arg * 512,
                                       512, data, 0) == 0) {
            sd_block(s, data, 512);
        }
        break;
    case 18:
        s->sector = arg;
        s->multi_read = arg < sectors;
        sd_queue(s, s->multi_read ? 0 : 0x20);
        break;
    case 12:
        s->multi_read = false;
        s->response_pos = s->response_len = 0;
        sd_queue(s, 0xff); /* SPI CMD12 stuff byte. */
        sd_queue(s, 0);
        break;
    case 24:
        s->sector = arg;
        s->write_pending = arg < sectors;
        s->multi_write = false;
        sd_queue(s, s->write_pending ? 0 : 0x20);
        break;
    case 25:
        s->sector = arg;
        s->write_pending = arg < sectors;
        s->multi_write = true;
        sd_queue(s, s->write_pending ? 0 : 0x20);
        break;
    case 23:
        sd_queue(s, 0);
        s->app_cmd = false;
        break;
    case 16:
        sd_queue(s, arg == 512 ? 0 : 0x40);
        break;
    default:
        sd_queue(s, 4);
        break;
    }
}

static uint8_t sd_transfer(TMS570SpiState *s, uint8_t tx)
{
    uint8_t out = 0xff;

    if (s->multi_read && !s->cmd_pos && (tx & 0xc0) != 0x40 &&
        s->response_pos == s->response_len) {
        uint8_t data[512];
        if (blk_pread(s->drive, (int64_t)s->sector++ * 512,
                      512, data, 0) == 0) {
            sd_block(s, data, 512);
        }
    }

    if (s->response_pos < s->response_len) {
        out = s->response[s->response_pos++];
        if (s->response_pos == s->response_len) {
            s->response_pos = s->response_len = 0;
        }
    }
    if (s->write_collect) {
        if (s->write_pos < 512) {
            s->write_data[s->write_pos++] = tx;
        } else if (++s->write_pos == 514) {
            bool ok = blk_pwrite(s->drive, (int64_t)s->sector * 512,
                                 512, s->write_data, 0) == 0;
            s->write_collect = false;
            s->write_pending = s->multi_write;
            s->sector++;
            s->write_pos = 0;
            sd_queue(s, ok ? 0x05 : 0x0d);
        }
        return out;
    }
    if (s->write_pending && s->multi_write && tx == 0xfd) {
        s->write_pending = s->multi_write = false;
        return out;
    }
    if (s->write_pending && (tx == 0xfe || (s->multi_write && tx == 0xfc))) {
        s->write_collect = true;
        s->write_pos = 0;
        return out;
    }
    if (s->cmd_pos || ((tx & 0xc0) == 0x40 && !s->response_len)) {
        s->cmd[s->cmd_pos++] = tx;
        if (s->cmd_pos == 6) {
            s->cmd_pos = 0;
            sd_command(s);
        }
    }
    return out;
}

static void spi_select(TMS570SpiState *s, bool active)
{
    if (s->cs_active != active) {
        s->cs_active = active;
        if (!active) {
            s->cmd_pos = 0;
            s->response_pos = s->response_len = 0;
            s->write_collect = s->write_pending = false;
            s->multi_read = s->multi_write = false;
        }
    }
}

static uint8_t spi_update_cs(TMS570SpiState *s)
{
    uint8_t function = s->reg[R_PC0 / 4];
    uint8_t levels = (s->hw_cs & function) |
                     (s->reg[R_PC3 / 4] & ~function);
    unsigned i;
    for (i = 0; i < 8; i++) {
        DeviceState *dev = ssi_get_cs(s->bus, i);
        if (dev) {
            qemu_set_irq(qdev_get_gpio_in_named(dev, SSI_GPIO_CS, 0),
                         !!(levels & (1u << i)));
        }
    }
    spi_select(s, s->sd_socket && !(levels & 4));
    return levels;
}

static uint16_t spi_word(TMS570SpiState *s, uint16_t tx, unsigned format)
{
    uint8_t levels = spi_update_cs(s);
    bool selected = false;
    uint16_t rx;
    unsigned bits = s->reg[(0x50 / 4) + format] & 0x1f;
    bits = bits ? MIN(bits, 16) : 8;
    if (s->reg[1] & 0x10000) { /* GCR1 internal loopback */
        return tx & (0xffff >> (16 - bits));
    }
    for (unsigned i = 0; i < 8; i++) {
        selected |= !(levels & (1u << i)) && ssi_get_cs(s->bus, i);
    }
    rx = selected ? ssi_transfer(s->bus, tx) : 0xffff;
    if (s->cs_active && s->drive) {
        rx = sd_transfer(s, tx);
    }
    return rx & (0xffff >> (16 - bits));
}

static unsigned spi_group_start(TMS570SpiState *s, unsigned group)
{
    return (s->reg[R_TGCTRL / 4 + group] >> 8) & 0x7f;
}

static unsigned spi_group_end(TMS570SpiState *s, unsigned group)
{
    unsigned end = group == 7 ? ((s->reg[0x94 / 4] >> 8) & 0x7f) + 1 :
                               (s->reg[R_TGCTRL / 4 + group + 1] >> 8) & 0xff;
    return end ? MIN(end, 128) : 128;
}

static void spi_dma_request(TMS570SpiState *s, unsigned group, bool transmit)
{
    if (s->reg[R_DMACTRL / 4 + group] & (transmit ? 0x8000 : 0x4000)) {
        tms570_dma_request(s->dma, transmit ? s->tx_request : s->rx_request);
    }
}

static void spi_group_done(TMS570SpiState *s, unsigned group)
{
    s->reg[R_TGFLG / 4] |= 1u << (16 + group);
    s->reg[R_TGCTRL / 4 + group] &= ~0x80000000u;
    spi_irq(s);
}

static void spi_group_transfer(TMS570SpiState *s, unsigned group)
{
    unsigned start = spi_group_start(s, group);
    unsigned end = spi_group_end(s, group);
    unsigned frames = (s->reg[R_DMACTRL / 4 + group] & 0x80000000) ?
                      (s->reg[0xf8 / 4 + group] >> 16) + 1 : 1;
    if (start >= end) {
        qemu_log_mask(LOG_GUEST_ERROR, "tms570-spi: empty transfer group %u\n",
                      group);
        return;
    }
    if (!(s->reg[1] & 1) && !(s->reg[1] & 0x10000)) {
        /* Slave words arrive through the optional byte-clock chardev. */
        s->slave_group = group;
        s->slave_pos = start;
        s->slave_end = end;
        s->slave_frames = frames;
        spi_dma_request(s, group, true);
        return;
    }
    for (unsigned f = 0; f < frames; f++) {
        spi_dma_request(s, group, true);
        for (unsigned i = start; i < end; i++) {
            uint16_t control = lduw_be_p(s->buffer + i * 4);
            uint16_t tx = lduw_be_p(s->buffer + i * 4 + 2);
            uint16_t rx;
            s->hw_cs = control;
            rx = spi_word(s, tx, (control >> 8) & 3);
            stw_be_p(s->buffer + 512 + i * 4, 0);
            stw_be_p(s->buffer + 512 + i * 4 + 2, rx);
            if (!(control & 0x1000)) {
                s->hw_cs = 0xff;
                spi_update_cs(s);
            }
        }
        spi_dma_request(s, group, false);
    }
    spi_group_done(s, group);
}

static int spi_peer_can_receive(void *opaque)
{
    TMS570SpiState *s = opaque;
    return s->slave_group >= 0 ? 1 : 0;
}

static void spi_peer_receive(void *opaque, const uint8_t *data, int size)
{
    TMS570SpiState *s = opaque;
    if (size && s->slave_group >= 0) {
        unsigned pos = s->slave_pos++;
        unsigned group = s->slave_group;
        uint8_t tx = lduw_be_p(s->buffer + pos * 4 + 2);
        stw_be_p(s->buffer + 512 + pos * 4, 0);
        stw_be_p(s->buffer + 512 + pos * 4 + 2, data[0]);
        qemu_chr_fe_write_all(&s->peer, &tx, 1);
        if (s->slave_pos >= s->slave_end) {
            spi_dma_request(s, group, false);
            if (--s->slave_frames) {
                s->slave_pos = spi_group_start(s, group);
                spi_dma_request(s, group, true);
            } else {
                s->slave_group = -1;
                spi_group_done(s, group);
            }
        }
    }
}

static uint64_t spi_read(void *opaque, hwaddr offset, unsigned size)
{
    TMS570SpiState *s = opaque;
    if (offset == R_BUF) {
        s->reg[R_FLG / 4] &= ~0x100;
        return s->rx;
    }
    if (offset == 0x1c) { /* Pin input latch */
        return s->reg[R_PC3 / 4];
    }
    if (offset < 0x200 && !(offset & 3)) {
        return s->reg[offset / 4];
    }
    return 0;
}

static void spi_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    TMS570SpiState *s = opaque;
    if (offset >= 0x200 || (offset & 3)) {
        return;
    }
    switch (offset) {
    case R_GCR0:
        if (!(value & 1)) {
            s->reg[R_FLG / 4] = 0x200;
            s->slave_group = -1;
            s->reg[R_TGFLG / 4] = 0;
            s->reg[0x74 / 4] = 0;
            s->hw_cs = 0xff;
            spi_select(s, false);
            spi_irq(s);
        }
        break;
    case R_FLG:
        s->reg[R_FLG / 4] &= ~(uint32_t)value;
        s->reg[R_FLG / 4] |= 0x200;
        return;
    case R_PC3:
        s->reg[R_PC3 / 4] = value;
        spi_update_cs(s);
        return;
    case R_PC4:
        s->reg[R_PC3 / 4] |= (uint32_t)value;
        spi_update_cs(s);
        return;
    case R_PC5:
        s->reg[R_PC3 / 4] &= ~(uint32_t)value;
        spi_update_cs(s);
        return;
    case R_PC0:
        s->reg[R_PC0 / 4] = value;
        spi_update_cs(s);
        return;
    case R_DAT0:
    case R_DAT1:
        if (offset == R_DAT1) {
            s->hw_cs = value >> 16;
        }
        s->rx = spi_word(s, value, (value >> 24) & 3);
        s->reg[R_FLG / 4] |= 0x300;
        if (offset == R_DAT1 && !(value & 0x10000000)) {
            s->hw_cs = 0xff;
            spi_update_cs(s);
        }
        break;
    case 0x74:
    case 0x7c:
        s->reg[offset / 4] |= value;
        spi_irq(s);
        return;
    case 0x78:
    case 0x80:
        s->reg[(offset - 4) / 4] &= ~value;
        spi_irq(s);
        return;
    case R_TGFLG:
        s->reg[offset / 4] &= ~value;
        spi_irq(s);
        return;
    default:
        break;
    }
    s->reg[offset / 4] = value;
    if (offset >= R_TGCTRL && offset < R_TGCTRL + 32) {
        unsigned group = (offset - R_TGCTRL) / 4;
        if (value & 0x80000000) {
            spi_group_transfer(s, group);
            qemu_chr_fe_accept_input(&s->peer);
        } else if (s->slave_group == group) {
            s->slave_group = -1;
        }
    }
}

static const MemoryRegionOps spi_ops = {
    .read = spi_read,
    .write = spi_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void spi_reset(DeviceState *dev)
{
    TMS570SpiState *s = TMS570_SPI(dev);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->buffer, 0, 0x20000);
    s->slave_group = -1;
    s->reg[R_FLG / 4] = 0x200;
    s->rx = 0xff;
    s->hw_cs = 0xff;
    s->reg[R_PC3 / 4] = 0xff;
    s->idle = true;
    s->app_cmd = false;
    s->cmd_pos = 0;
    s->response_pos = s->response_len = 0;
    s->write_collect = s->write_pending = false;
    s->multi_read = s->multi_write = false;
    spi_select(s, false);
    spi_irq(s);
}

static void spi_init(Object *obj)
{
    TMS570SpiState *s = TMS570_SPI(obj);
    memory_region_init_io(&s->iomem, obj, &spi_ops, s, TYPE_TMS570_SPI, 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->bus = ssi_create_bus(DEVICE(obj), "spi");
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[0]);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[1]);
}

static void spi_realize(DeviceState *dev, Error **errp)
{
    TMS570SpiState *s = TMS570_SPI(dev);
    g_autofree char *path = object_get_canonical_path(OBJECT(dev));
    g_autofree char *name = g_strdup_printf("%s.ram", path);

    if (!memory_region_init_ram(&s->ram, OBJECT(dev), name, 0x20000, errp)) {
        return;
    }
    s->buffer = memory_region_get_ram_ptr(&s->ram);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->ram);

    qemu_chr_fe_set_handlers(&s->peer, spi_peer_can_receive, spi_peer_receive,
                            NULL, NULL, s, NULL, true);

    if (!s->drive) {
        /*
         * Without an image the card never answers CMD0, and the firmware just
         * reports "disk_initialize failed, status=0x01" with no hint that the
         * socket is empty.  Say so up front.
         */
        if (s->sd_socket) {
            warn_report("obc100: no SD card image attached; the guest's sddisk "
                    "commands will fail");
        error_printf("  attach one with: "
                     "-drive if=sd,format=raw,file=<image>\n");
        }
        return;
    }

    if (blk_set_perm(s->drive, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_ALL, errp) < 0) {
        return;
    }
}

static void spi_class_init(ObjectClass *klass, const void *data)
{
    DEVICE_CLASS(klass)->realize = spi_realize;
    device_class_set_legacy_reset(DEVICE_CLASS(klass), spi_reset);
    static const Property props[] = {
        DEFINE_PROP_DRIVE("drive", TMS570SpiState, drive),
        DEFINE_PROP_BOOL("sd-socket", TMS570SpiState, sd_socket, false),
        DEFINE_PROP_CHR("peer", TMS570SpiState, peer),
        DEFINE_PROP_LINK("dma", TMS570SpiState, dma, TYPE_TMS570_DMA, TMS570DmaState *),
        DEFINE_PROP_UINT32("tx-request", TMS570SpiState, tx_request, 0),
        DEFINE_PROP_UINT32("rx-request", TMS570SpiState, rx_request, 0),
    };
    device_class_set_props(DEVICE_CLASS(klass), props);
}

static const TypeInfo spi_info = {
    .name = TYPE_TMS570_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570SpiState),
    .instance_init = spi_init,
    .class_init = spi_class_init,
};

static void spi_register_types(void)
{
    type_register_static(&spi_info);
}
type_init(spi_register_types)
