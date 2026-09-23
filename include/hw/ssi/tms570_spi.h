/* OBC100 SPI4 compatibility mode, local experiment. */
#ifndef HW_SSI_TMS570_SPI_H
#define HW_SSI_TMS570_SPI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "system/block-backend.h"
#include "hw/ssi/ssi.h"
#include "hw/dma/tms570_dma.h"
#include "chardev/char-fe.h"

#define TYPE_TMS570_SPI "tms570-spi"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570SpiState, TMS570_SPI)

struct TMS570SpiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion ram;
    uint8_t *buffer;
    qemu_irq irq[2];
    uint32_t reg[0x200 / 4];
    uint16_t rx;
    TMS570DmaState *dma;
    uint32_t tx_request, rx_request;
    bool sd_socket;
    CharFrontend peer;
    int slave_group;
    unsigned slave_pos, slave_end, slave_frames;
    bool cs_active;
    SSIBus *bus;
    uint8_t hw_cs;
    BlockBackend *drive;
    uint8_t cmd[6];
    uint8_t cmd_pos;
    uint8_t response[530];
    unsigned response_pos;
    unsigned response_len;
    uint8_t write_data[512];
    unsigned write_pos;
    uint32_t sector;
    bool idle;
    bool app_cmd;
    bool write_pending;
    bool write_collect;
    bool multi_read;
    bool multi_write;
};

#endif
