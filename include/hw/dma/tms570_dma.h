/* Local TMS570 DMA model for OBC100 bring-up. */
#ifndef HW_DMA_TMS570_DMA_H
#define HW_DMA_TMS570_DMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_TMS570_DMA "tms570-dma"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570DmaState, TMS570_DMA)

struct TMS570DmaState {
    SysBusDevice parent_obj;
    MemoryRegion regs;
    MemoryRegion ram;
    qemu_irq btca;
    uint32_t reg[0x400 / 4];
    uint32_t packet[0x1000 / 4];
    uint32_t hw_enabled;
    uint32_t btc_enabled;
    uint32_t global_irq_enabled;
    uint32_t btc_pending;
    uint32_t current_src[32], current_dst[32], frames_left[32];
};

void tms570_dma_request(TMS570DmaState *s, unsigned request);

#endif
