/* TMS570 ADC subset for the OBC100 local experiment. */
#ifndef HW_ADC_TMS570_ADC_H
#define HW_ADC_TMS570_ADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_TMS570_ADC "tms570-adc"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570AdcState, TMS570_ADC)

struct TMS570AdcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t reg[0x200 / 4];
    uint32_t unit;
    uint8_t fifo_index[3];
    uint8_t fifo_count[3];
    uint32_t fifo[3][32];
    uint16_t threshold[3];
    uint16_t sample[32];
    qemu_irq irq[3];
};

#endif
