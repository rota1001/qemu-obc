/*
 * TI TMS570LC43xx SoC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "qapi/error.h"
#include "hw/arm/tms570lc43.h"
#include "hw/misc/unimp.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"

/* VIM channel assignments, from TI SPNS195C table 6-15. */
#define VIM_CH_ESM_HIGH     0
#define VIM_CH_RTI_COMP0    2
#define VIM_CH_RTI_COMP1    3
#define VIM_CH_RTI_COMP2    4
#define VIM_CH_RTI_COMP3    5
#define VIM_CH_GIO_HIGH     9
#define VIM_CH_LIN1_LVL0    13
#define VIM_CH_ESM_LOW      20
#define VIM_CH_SSI          21
#define VIM_CH_GIO_LOW      23
#define VIM_CH_DMA_BTCA     40
#define VIM_CH_LIN1_LVL1    27
#define VIM_CH_SCI3_LVL0    64
#define VIM_CH_SCI3_LVL1    74
#define VIM_CH_SCI4_LVL0    116

/*
 * Peripherals the firmware initialises but that this model does not implement.
 * Their init routines all poll for a status bit to *clear*, so reads returning
 * zero let them fall through instead of hanging -- which is exactly what
 * create_unimplemented_device() gives us, plus a log line per access.
 */
static const struct {
    const char *name;
    hwaddr base;
    hwaddr size;
} tms570_unimplemented[] = {
    /* Timers and capture units -- not used by this firmware. */
    { "tms570.het1ram",    0xFF460000, 0x20000 },
    { "tms570.het2ram",    0xFF440000, 0x20000 },
    { "tms570.htu1",       0xFFF7A400, 0x100 },
    { "tms570.htu2",       0xFFF7A500, 0x100 },
    { "tms570.htu1dcp",    0xFF4E0000, 0x1000 },
    { "tms570.htu2dcp",    0xFF4C0000, 0x1000 },
    { "tms570.etpwm",      0xFCF78C00, 0x800 },
    { "tms570.ecap",       0xFCF79300, 0x600 },
    { "tms570.eqep",       0xFCF79900, 0x100 },

    /* Serial and analogue peripherals the firmware initialises but this
     * model does not implement.  Every one of their init routines polls a
     * status bit for it to *clear*, so reads of zero let them fall through. */
    { "tms570.sdcmmr",     0xFA000000, 0x1000 },

    /* Memory and bus infrastructure. */
    { "tms570.efc",        0xFFF8C000, 0x1000 },
    { "tms570.pmm",        0xFFFF0000, 0x800 },
    { "tms570.scm",        0xFFFF0A00, 0x100 },
    { "tms570.epc",        0xFFFF0C00, 0x100 },
    { "tms570.nmpu_ps",    0xFFFF1800, 0x200 },
    { "tms570.nmpu_dma",   0xFFFF1A00, 0x200 },
    { "tms570.nmpu_emac",  0xFCFF1800, 0x200 },
    { "tms570.pom",        0xFFA04000, 0x1000 },

    /* Self-test and debug blocks. */
    { "tms570.ccmr5",      0xFFFFF600, 0x100 },
    { "tms570.pbist",      0xFFFFE560, 0x100 },
    { "tms570.stc1",       0xFFFFE600, 0x200 },
    { "tms570.stc2",       0xFFFF0800, 0x200 },
    { "tms570.dmm",        0xFFFFF700, 0x100 },
    { "tms570.rtp",        0xFFFFFA00, 0x100 },
    { "tms570.crc1",       0xFE000000, 0x1000 },
    { "tms570.crc2",       0xFB000000, 0x1000 },
};

static void tms570lc43_init(Object *obj)
{
    TMS570LC43State *s = TMS570LC43_SOC(obj);
    int i;

    object_initialize_child(obj, "cpu", &s->cpu,
                            ARM_CPU_TYPE_NAME("cortex-r5f"));
    object_initialize_child(obj, "system", &s->system, TYPE_TMS570_SYSTEM);
    object_initialize_child(obj, "esm", &s->esm, TYPE_TMS570_ESM);
    object_initialize_child(obj, "dcc1", &s->dcc[0], TYPE_TMS570_DCC);
    object_initialize_child(obj, "dcc2", &s->dcc[1], TYPE_TMS570_DCC);
    object_initialize_child(obj, "vim", &s->vim, TYPE_TMS570_VIM);
    object_initialize_child(obj, "rti", &s->rti, TYPE_TMS570_RTI);
    object_initialize_child(obj, "gio", &s->gio, TYPE_TMS570_GIO);
    object_initialize_child(obj, "het1", &s->het[0], TYPE_TMS570_HET_GPIO);
    object_initialize_child(obj, "het2", &s->het[1], TYPE_TMS570_HET_GPIO);
    object_initialize_child(obj, "dma", &s->dma, TYPE_TMS570_DMA);
    object_initialize_child(obj, "i2c1", &s->i2c[0], TYPE_TMS570_I2C);
    object_initialize_child(obj, "i2c2", &s->i2c[1], TYPE_TMS570_I2C);
    object_initialize_child(obj, "spi4", &s->spi4, TYPE_TMS570_SPI);
    qdev_prop_set_bit(DEVICE(&s->spi4), "sd-socket", true);
    qdev_prop_set_uint32(DEVICE(&s->spi4), "tx-request", 25);
    qdev_prop_set_uint32(DEVICE(&s->spi4), "rx-request", 24);
    for (i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("spi%d", i == 3 ? 5 : i + 1);
        static const unsigned requests[4][2] = { { 1, 0 }, { 3, 2 },
                                                 { 15, 14 }, { 31, 30 } };
        object_initialize_child(obj, name, &s->spi[i], TYPE_TMS570_SPI);
        qdev_prop_set_uint32(DEVICE(&s->spi[i]), "tx-request", requests[i][0]);
        qdev_prop_set_uint32(DEVICE(&s->spi[i]), "rx-request", requests[i][1]);
    }
    object_initialize_child(obj, "adc1", &s->adc[0], TYPE_TMS570_ADC);
    object_initialize_child(obj, "adc2", &s->adc[1], TYPE_TMS570_ADC);
    qdev_prop_set_uint32(DEVICE(&s->adc[1]), "unit", 1);
    for (i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("dcan%d", i + 1);
        object_initialize_child(obj, name, &s->dcan[i], TYPE_TMS570_DCAN);
    }

    for (i = 0; i < TMS570_NUM_SCI; i++) {
        g_autofree char *name = g_strdup_printf("sci%d", i + 1);
        object_initialize_child(obj, name, &s->sci[i], TYPE_TMS570_SCI);
    }
}

static void tms570lc43_realize(DeviceState *dev, Error **errp)
{
    TMS570LC43State *s = TMS570LC43_SOC(dev);
    MemoryRegion *mem = s->board_memory;
    DeviceState *vimdev;
    SysBusDevice *sbd;
    Error *err = NULL;
    int i;

    if (!mem) {
        mem = get_system_memory();
    }

    /*
     * The part comes out of reset big-endian.  On this ARMv7-R core cfgend
     * selects the legacy word-invariant BE-32 model (SCTLR.B), which is what
     * TI's Hercules devices implement and what the firmware is linked for.
     */
    qdev_prop_set_bit(DEVICE(&s->cpu), "cfgend", true);
    /* Vectors live at 0x00000000, not the high vector base. */
    qdev_prop_set_bit(DEVICE(&s->cpu), "reset-hivecs", false);

    if (!qdev_realize(DEVICE(&s->cpu), NULL, errp)) {
        return;
    }

    /* Flash banks 0/1 as ROM, plus the on-chip and external SRAMs. */
    if (!memory_region_init_rom(&s->flash, OBJECT(dev), "tms570.flash",
                                TMS570_FLASH_SIZE, &err)) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(mem, TMS570_FLASH_BASE, &s->flash);

    if (!memory_region_init_ram(&s->sram, OBJECT(dev), "tms570.sram",
                                TMS570_SRAM_SIZE, &err)) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(mem, TMS570_SRAM_BASE, &s->sram);

    /*
     * OTP and flash bank 7 read as erased.  trimLPO() takes the top half of
     * the word at 0xF00801B4 as its LPO trim and falls back to a built-in
     * value when it reads all ones, which is what an unprogrammed part does.
     */
    if (!memory_region_init_rom(&s->otp, OBJECT(dev), "tms570.otp",
                                TMS570_OTP_SIZE, &err)) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(mem, TMS570_OTP_BASE, &s->otp);
    memset(memory_region_get_ram_ptr(&s->otp), 0xFF, TMS570_OTP_SIZE);
    /* Factory ADC temperature calibration words recorded by the firmware.
     * They are OTP data, separate from the LPO trim fallback at 0x801B4. */
    {
        static const uint8_t temperature_calibration[3][16] = {
            { 0x07, 0x9b, 0x01, 0x2f, 0x05, 0xf5, 0x00, 0xe9,
              0x09, 0xbe, 0x01, 0x8e, 0xff, 0xff, 0xff, 0xff },
            { 0x07, 0x69, 0x01, 0x2f, 0x05, 0xa4, 0x00, 0xe9,
              0x09, 0xb2, 0x01, 0x8e, 0xff, 0xff, 0xff, 0xff },
            { 0x07, 0x7d, 0x01, 0x2f, 0x05, 0xf0, 0x00, 0xe9,
              0x09, 0x9e, 0x01, 0x8e, 0xff, 0xff, 0xff, 0xff },
        };
        uint8_t *otp = memory_region_get_ram_ptr(&s->otp);
        for (i = 0; i < 3; i++) {
            memcpy(otp + 0x80310 + i * 0x10,
                   temperature_calibration[i], sizeof(temperature_calibration[i]));
        }
    }

    for (i = 0; i < 3; i++) {
        g_autofree char *name = g_strdup_printf("tms570.emif-cs%d", i + 1);
        static const hwaddr cs_base[3] = {
            TMS570_EMIF_CS1_BASE, TMS570_EMIF_CS2_BASE, TMS570_EMIF_CS3_BASE
        };

        if (!memory_region_init_ram(&s->emif_cs[i], OBJECT(dev), name,
                                    TMS570_EMIF_CS_SIZE, &err)) {
            error_propagate(errp, err);
            return;
        }
        memory_region_add_subregion(mem, cs_base[i], &s->emif_cs[i]);
    }

    /* VIM first: everything else hangs its interrupt lines off it. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->vim), errp)) {
        return;
    }
    vimdev = DEVICE(&s->vim);
    sbd = SYS_BUS_DEVICE(&s->vim);
    sysbus_mmio_map(sbd, 0, TMS570_VIM_BASE);
    sysbus_mmio_map(sbd, 1, TMS570_VIM_RAM_BASE);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(DEVICE(&s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(sbd, 1,
                       qdev_get_gpio_in(DEVICE(&s->cpu), ARM_CPU_FIQ));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->system), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&s->system);
    sysbus_mmio_map(sbd, 0, TMS570_SYSTEM1_BASE);
    sysbus_mmio_map(sbd, 1, TMS570_SYSTEM2_BASE);
    /* SSIR1..4 -> VIM channels 21..24. */
    for (i = 0; i < 4; i++) {
        sysbus_connect_irq(sbd, i,
                           qdev_get_gpio_in(vimdev, VIM_CH_SSI + i));
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->esm), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&s->esm);
    sysbus_mmio_map(sbd, 0, TMS570_ESM_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vimdev, VIM_CH_ESM_HIGH));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(vimdev, VIM_CH_ESM_LOW));

    {
        static const hwaddr dcc_base[2] = {
            TMS570_DCC1_BASE, TMS570_DCC2_BASE
        };

        for (i = 0; i < 2; i++) {
            if (!sysbus_realize(SYS_BUS_DEVICE(&s->dcc[i]), errp)) {
                return;
            }
            sysbus_mmio_map(SYS_BUS_DEVICE(&s->dcc[i]), 0, dcc_base[i]);
        }
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rti), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&s->rti);
    sysbus_mmio_map(sbd, 0, TMS570_RTI_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vimdev, VIM_CH_RTI_COMP0));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(vimdev, VIM_CH_RTI_COMP1));
    sysbus_connect_irq(sbd, 2, qdev_get_gpio_in(vimdev, VIM_CH_RTI_COMP2));
    sysbus_connect_irq(sbd, 3, qdev_get_gpio_in(vimdev, VIM_CH_RTI_COMP3));
    sysbus_connect_irq(sbd, 4, qdev_get_gpio_in_named(DEVICE(&s->system),
                                                    "watchdog-reset", 0));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gio), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&s->gio);
    sysbus_mmio_map(sbd, 0, TMS570_GIO_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vimdev, VIM_CH_GIO_HIGH));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(vimdev, VIM_CH_GIO_LOW));
    for (i = 0; i < 2; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->het[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->het[i]), 0, 0xFFF7B800 + i * 0x100);
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dma), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&s->dma);
    sysbus_mmio_map(sbd, 0, TMS570_DMA_BASE);
    sysbus_mmio_map(sbd, 1, TMS570_DMA_RAM_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vimdev, VIM_CH_DMA_BTCA));

    for (i = 0; i < 2; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2c[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2c[i]), 0,
                        0xFFF7D400 + i * 0x100);
    }
    /* The RTC is fixed on OBC100's first I2C bus. */
    i2c_slave_create_simple(s->i2c[0].bus, "obc-rtc", 0x69);
    i2c_slave_create_simple(s->i2c[0].bus, "obc-eps", 0x48);

    object_property_set_link(OBJECT(&s->spi4), "dma", OBJECT(&s->dma), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->spi4), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi4), 0, 0xFFF7FA00);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi4), 1, 0xFF060000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->spi4), 0, qdev_get_gpio_in(vimdev, 49));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->spi4), 1, qdev_get_gpio_in(vimdev, 54));
    for (i = 0; i < 4; i++) {
        static const hwaddr bases[4] = { 0xFFF7F400, 0xFFF7F600,
                                        0xFFF7F800, 0xFFF7FC00 };
        static const hwaddr ram[4] = { 0xFF0E0000, 0xFF080000,
                                      0xFF0C0000, 0xFF0A0000 };
        static const unsigned channels[4][2] = { { 12, 26 }, { 17, 30 },
                                                 { 37, 38 }, { 53, 56 } };
        sbd = SYS_BUS_DEVICE(&s->spi[i]);
        object_property_set_link(OBJECT(&s->spi[i]), "dma", OBJECT(&s->dma),
                                 &error_abort);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, bases[i]);
        sysbus_mmio_map(sbd, 1, ram[i]);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vimdev, channels[i][0]));
        sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(vimdev, channels[i][1]));
    }

    for (i = 0; i < 2; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->adc[i]), 0,
                        0xFFF7C000 + i * 0x200);
        for (unsigned group = 0; group < 3; group++) {
            static const unsigned channels[2][3] = { { 14, 15, 28 }, { 50, 51, 57 } };
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->adc[i]), group,
                                qdev_get_gpio_in(vimdev, channels[i][group]));
        }
    }

    for (i = 0; i < 4; i++) {
        static const unsigned channels[4][2] = {
            { 16, 29 }, { 35, 42 }, { 45, 55 }, { 113, 117 }
        };
        sbd = SYS_BUS_DEVICE(&s->dcan[i]);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, 0xFFF7DC00 + i * 0x200);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vimdev, channels[i][0]));
        sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(vimdev, channels[i][1]));
    }

    /*
     * SCI1 and SCI2 are the LIN modules running in SCI compatibility mode;
     * SCI3 and SCI4 are standalone.  Only SCI1 (the console), SCI3 (ADCS) and
     * SCI4 (mission board) have interrupts routed.
     */
    {
        static const struct {
            hwaddr base;
            int irq_high;
            int irq_low;
        } sci_map[TMS570_NUM_SCI] = {
            { TMS570_LIN1_BASE, VIM_CH_LIN1_LVL0, VIM_CH_LIN1_LVL1 },
            { TMS570_LIN2_BASE, 115, 118 },
            { TMS570_SCI3_BASE, VIM_CH_SCI3_LVL0, VIM_CH_SCI3_LVL1 },
            { TMS570_SCI4_BASE, VIM_CH_SCI4_LVL0, 119 },
        };

        for (i = 0; i < TMS570_NUM_SCI; i++) {
            if (!sysbus_realize(SYS_BUS_DEVICE(&s->sci[i]), errp)) {
                return;
            }
            sbd = SYS_BUS_DEVICE(&s->sci[i]);
            sysbus_mmio_map(sbd, 0, sci_map[i].base);
            if (sci_map[i].irq_high >= 0) {
                sysbus_connect_irq(sbd, 0,
                        qdev_get_gpio_in(vimdev, sci_map[i].irq_high));
            }
            if (sci_map[i].irq_low >= 0) {
                sysbus_connect_irq(sbd, 1,
                        qdev_get_gpio_in(vimdev, sci_map[i].irq_low));
            }
        }
    }

    /* Configuration-only blocks: pinmux, PCR1/2/3, EMIF, flash wrapper. */
    static const hwaddr config_base[] = {
        0xffff1c00, 0xffff1000, 0xfcff1000, 0xfff78000,
        0xfcffe800, 0xfff87000,
    };
    static const unsigned config_size[] = { 0x1000, 0x400, 0x400, 0x400, 0x100, 0x1000 };
    static const unsigned config_kind[] = { 0, 1, 1, 1, 2, 3 };
    for (i = 0; i < ARRAY_SIZE(config_base); i++) {
        DeviceState *cfg = qdev_new("tms570-config");
        qdev_prop_set_uint32(cfg, "kind", config_kind[i]);
        qdev_prop_set_uint32(cfg, "size", config_size[i]);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(cfg), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(cfg), 0, config_base[i]);
    }

    for (i = 0; i < ARRAY_SIZE(tms570_unimplemented); i++) {
        create_unimplemented_device(tms570_unimplemented[i].name,
                                    tms570_unimplemented[i].base,
                                    tms570_unimplemented[i].size);
    }
}

static const Property tms570lc43_properties[] = {
    DEFINE_PROP_LINK("memory", TMS570LC43State, board_memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void tms570lc43_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = tms570lc43_realize;
    device_class_set_props(dc, tms570lc43_properties);
    dc->desc = "TI TMS570LC43xx SoC";
    /* Not directly user-creatable: the board wires it up. */
    dc->user_creatable = false;
}

static const TypeInfo tms570lc43_info = {
    .name          = TYPE_TMS570LC43_SOC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570LC43State),
    .instance_init = tms570lc43_init,
    .class_init    = tms570lc43_class_init,
};

static void tms570lc43_register_types(void)
{
    type_register_static(&tms570lc43_info);
}

type_init(tms570lc43_register_types)
