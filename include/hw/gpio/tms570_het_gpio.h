/* HET GPIO subset, local OBC100 experiment. */
#ifndef HW_GPIO_TMS570_HET_GPIO_H
#define HW_GPIO_TMS570_HET_GPIO_H
#include "hw/core/sysbus.h"
#define TYPE_TMS570_HET_GPIO "tms570-het-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570HetGpioState, TMS570_HET_GPIO)
struct TMS570HetGpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t reg[0x100 / 4];
    uint32_t input, driven;
    qemu_irq out[32];
};
#endif
