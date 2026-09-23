/* MT29F1G01ABAFDSF subset used by OBC100. Local experiment.
 * 1024 blocks x 64 pages x (2048 data + 128 spare) bytes.
 * Synchronous operations, no wear, factory bad blocks, or ECC corruption.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/ssi/ssi.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/block-backend.h"
#include "qapi/error.h"

#define TYPE_OBC_SPI_NAND "obc-spi-nand"
OBJECT_DECLARE_SIMPLE_TYPE(ObcSpiNandState, OBC_SPI_NAND)
#define PAGE_BYTES 2176
#define PAGE_COUNT 65536
#define WEL 2
#define E_FAIL 4
#define P_FAIL 8

struct ObcSpiNandState {
    SSIPeripheral parent_obj;
    BlockBackend *drive;
    GHashTable *pages;
    uint8_t cache[PAGE_BYTES];
    uint8_t feature[256];
    uint8_t command, feature_addr;
    unsigned pos, column;
    uint32_t row;
};

static bool nand_read_page(ObcSpiNandState *s, uint32_t page, uint8_t *data)
{
    uint8_t *saved;
    memset(data, 0xff, PAGE_BYTES);
    if (page >= PAGE_COUNT) {
        return false;
    }
    if (s->drive) {
        return blk_pread(s->drive, (int64_t)page * PAGE_BYTES,
                         PAGE_BYTES, data, 0) == 0;
    }
    saved = g_hash_table_lookup(s->pages, GUINT_TO_POINTER(page + 1));
    if (saved) {
        memcpy(data, saved, PAGE_BYTES);
    }
    return true;
}

static bool nand_write_page(ObcSpiNandState *s, uint32_t page, const uint8_t *data)
{
    if (page >= PAGE_COUNT) {
        return false;
    }
    if (s->drive) {
        return blk_pwrite(s->drive, (int64_t)page * PAGE_BYTES,
                          PAGE_BYTES, data, 0) == 0;
    }
    g_hash_table_replace(s->pages, GUINT_TO_POINTER(page + 1),
                         g_memdup2(data, PAGE_BYTES));
    return true;
}

static void nand_execute(ObcSpiNandState *s)
{
    uint8_t data[PAGE_BYTES];
    unsigned i;
    uint8_t fail = s->command == 0xd8 ? E_FAIL : P_FAIL;

    if (s->command == 0x13) {
        nand_read_page(s, s->row, s->cache);
        s->feature[0xc0] &= 0x0f; /* No injected ECC errors. */
        return;
    }
    s->feature[0xc0] &= ~fail;
    if (!(s->feature[0xc0] & WEL)) {
        return;
    }
    s->feature[0xc0] &= ~WEL;
    if (s->feature[0xa0] & 0x7c || s->row >= PAGE_COUNT) {
        s->feature[0xc0] |= fail;
        return;
    }
    if (s->command == 0x10) {
        if (!nand_read_page(s, s->row, data)) {
            s->feature[0xc0] |= fail;
            return;
        }
        for (i = 0; i < PAGE_BYTES; i++) {
            data[i] &= s->cache[i]; /* NAND programming only clears bits. */
        }
        if (!nand_write_page(s, s->row, data)) {
            s->feature[0xc0] |= fail;
        }
    } else if (s->command == 0xd8) {
        uint32_t first = s->row & ~63u;
        memset(data, 0xff, sizeof(data));
        for (i = 0; i < 64; i++) {
            if (!s->drive) {
                g_hash_table_remove(s->pages, GUINT_TO_POINTER(first + i + 1));
            } else if (!nand_write_page(s, first + i, data)) {
                s->feature[0xc0] |= fail;
                break;
            }
        }
    }
}

static void nand_reset_command(ObcSpiNandState *s)
{
    s->pos = 0;
    s->column = 0;
    s->row = 0;
    s->command = 0;
}

static int nand_cs(SSIPeripheral *dev, bool high)
{
    ObcSpiNandState *s = OBC_SPI_NAND(dev);
    if (high && s->pos == 4 &&
        (s->command == 0x10 || s->command == 0x13 || s->command == 0xd8)) {
        nand_execute(s);
    }
    nand_reset_command(s);
    return 0;
}

static uint32_t nand_transfer(SSIPeripheral *dev, uint32_t value)
{
    ObcSpiNandState *s = OBC_SPI_NAND(dev);
    unsigned pos = s->pos++;
    uint8_t tx = value;

    if (pos == 0) {
        s->command = tx;
        switch (tx) {
        case 0xff:
            s->feature[0xc0] = 0;
            memset(s->cache, 0xff, sizeof(s->cache));
            break;
        case 0x06: s->feature[0xc0] |= WEL; break;
        case 0x04: s->feature[0xc0] &= ~WEL; break;
        case 0x02: memset(s->cache, 0xff, sizeof(s->cache)); break;
        }
        return 0xff;
    }
    switch (s->command) {
    case 0x9f:
        return pos == 2 ? 0x2c : pos == 3 ? 0x14 : 0xff;
    case 0x0f:
    case 0x1f:
        if (pos == 1) {
            s->feature_addr = tx;
        } else if (s->command == 0x0f) {
            return s->feature[s->feature_addr];
        } else if (pos == 2 && (s->feature_addr == 0xa0 ||
                                s->feature_addr == 0xb0 ||
                                s->feature_addr == 0xd0)) {
            s->feature[s->feature_addr] = tx;
        }
        break;
    case 0x13:
    case 0x10:
    case 0xd8:
        if (pos <= 3) {
            s->row = (s->row << 8) | tx;
        }
        break;
    case 0x03:
    case 0x0b:
    case 0x02:
    case 0x84:
        if (pos <= 2) {
            s->column = (s->column << 8) | tx;
        } else if (s->command == 0x02 || s->command == 0x84) {
            if (s->column < PAGE_BYTES) {
                s->cache[s->column++] = tx;
            }
        } else if (pos > 3) {
            return s->column < PAGE_BYTES ? s->cache[s->column++] : 0xff;
        }
        break;
    }
    return 0xff;
}

static void nand_reset(DeviceState *dev)
{
    ObcSpiNandState *s = OBC_SPI_NAND(dev);
    nand_reset_command(s);
    memset(s->feature, 0, sizeof(s->feature));
    s->feature[0xa0] = 0x7c;
    s->feature[0xb0] = 0x10;
    memset(s->cache, 0xff, sizeof(s->cache));
    SSI_PERIPHERAL(dev)->cs = true;
}

static void nand_realize(SSIPeripheral *dev, Error **errp)
{
    ObcSpiNandState *s = OBC_SPI_NAND(dev);
    if (s->drive) {
        if (blk_getlength(s->drive) != (int64_t)PAGE_COUNT * PAGE_BYTES) {
            error_setg(errp, "obc-spi-nand: image must be 142606336 bytes "
                       "(65536 pages of 2048 data + 128 spare bytes)");
            return;
        }
        if (blk_set_perm(s->drive, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
    }
    s->pages = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
}

static void nand_finalize(Object *obj)
{
    ObcSpiNandState *s = OBC_SPI_NAND(obj);
    if (s->pages) {
        g_hash_table_destroy(s->pages);
    }
}

static const Property nand_properties[] = {
    DEFINE_PROP_DRIVE("drive", ObcSpiNandState, drive),
};

static void nand_class_init(ObjectClass *klass, const void *data)
{
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    sc->realize = nand_realize;
    sc->transfer = nand_transfer;
    sc->set_cs = nand_cs;
    sc->cs_polarity = SSI_CS_LOW;
    device_class_set_legacy_reset(dc, nand_reset);
    device_class_set_props(dc, nand_properties);
}

static const TypeInfo nand_info = {
    .name = TYPE_OBC_SPI_NAND,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(ObcSpiNandState),
    .instance_finalize = nand_finalize,
    .class_init = nand_class_init,
};

static void nand_register_types(void)
{
    type_register_static(&nand_info);
}
type_init(nand_register_types)
