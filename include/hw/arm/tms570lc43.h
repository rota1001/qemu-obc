/*
 * TI TMS570LC43xx SoC (ARM Cortex-R5F, big-endian BE-32)
 *
 * Written for the SATELLA OBC100 flight computer.  Only the peripherals the
 * OBC100 firmware touches on its way to a console prompt are modelled; the
 * rest of the peripheral space is mapped as unimplemented so that stray
 * accesses are logged instead of aborting.
 *
 * This is a local experiment, not intended for upstream.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_TMS570LC43_H
#define HW_ARM_TMS570LC43_H

#include "hw/core/sysbus.h"
#include "qemu/units.h"
#include "target/arm/cpu.h"
#include "qom/object.h"
#include "hw/misc/tms570_system.h"
#include "hw/misc/tms570_esm.h"
#include "hw/misc/tms570_dcc.h"
#include "hw/intc/tms570_vim.h"
#include "hw/timer/tms570_rti.h"
#include "hw/char/tms570_sci.h"
#include "hw/gpio/tms570_gio.h"
#include "hw/gpio/tms570_het_gpio.h"
#include "hw/dma/tms570_dma.h"
#include "hw/i2c/tms570_i2c.h"
#include "hw/ssi/tms570_spi.h"
#include "hw/adc/tms570_adc.h"
#include "hw/net/tms570_dcan.h"

#define TYPE_TMS570LC43_SOC "tms570lc43-soc"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570LC43State, TMS570LC43_SOC)

/* Flash banks 0 and 1, contiguous, 4MB total. */
#define TMS570_FLASH_BASE       0x00000000
#define TMS570_FLASH_SIZE       (4 * MiB)

/* On-chip L2 SRAM: stacks, kernel RAM, heap, .bss/.data. */
#define TMS570_SRAM_BASE        0x08000000
#define TMS570_SRAM_SIZE        (512 * KiB)

/*
 * Flash bank 7 and the TI OTP region.  The startup code reads an LPO trim
 * value out of OTP at 0xF00801B4, and the FEE driver lives in bank 7.
 */
#define TMS570_OTP_BASE         0xF0000000
#define TMS570_OTP_SIZE         (4 * MiB)

/* EMIF asynchronous chip selects, populated with SRAM on the OBC100. */
#define TMS570_EMIF_CS1_BASE    0x60000000
#define TMS570_EMIF_CS2_BASE    0x64000000
#define TMS570_EMIF_CS3_BASE    0x68000000
#define TMS570_EMIF_CS_SIZE     (2 * MiB)

/* Peripheral bases (TI SPNS195C). */
#define TMS570_GIO_BASE         0xFFF7BC00
#define TMS570_LIN1_BASE        0xFFF7E400  /* SCI1: the console */
#define TMS570_SCI3_BASE        0xFFF7E500
#define TMS570_LIN2_BASE        0xFFF7E600
#define TMS570_SCI4_BASE        0xFFF7E700
#define TMS570_VIM_RAM_BASE     0xFFF82000
#define TMS570_EMIF_BASE        0xFCFFE800
#define TMS570_ESM_BASE         0xFFFFF500
#define TMS570_RTI_BASE         0xFFFFFC00
#define TMS570_VIM_BASE         0xFFFFFD00
#define TMS570_SYSTEM1_BASE     0xFFFFFF00
#define TMS570_SYSTEM2_BASE     0xFFFFE100
#define TMS570_DCC1_BASE        0xFFFFEC00
#define TMS570_DCC2_BASE        0xFFFFF400
#define TMS570_DMA_BASE         0xFFFFF000
#define TMS570_DMA_RAM_BASE     0xFFF80000

#define TMS570_NUM_SCI          4

struct TMS570LC43State {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    ARMCPU cpu;

    TMS570SystemState system;
    TMS570EsmState esm;
    TMS570DccState dcc[2];
    TMS570VimState vim;
    TMS570RtiState rti;
    TMS570SciState sci[TMS570_NUM_SCI];
    TMS570GioState gio;
    TMS570HetGpioState het[2];
    TMS570DmaState dma;
    TMS570I2CState i2c[2];
    TMS570SpiState spi4;
    TMS570SpiState spi[4]; /* MibSPI1, 2, 3, 5 */
    TMS570AdcState adc[2];
    TMS570DcanState dcan[4];

    MemoryRegion flash;
    MemoryRegion sram;
    MemoryRegion emif_cs[3];
    MemoryRegion otp;

    MemoryRegion *board_memory;
    MemoryRegion container;
};

#endif /* HW_ARM_TMS570LC43_H */
