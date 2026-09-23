/* Local TMS570 I2C controller model. */
#ifndef HW_I2C_TMS570_I2C_H
#define HW_I2C_TMS570_I2C_H
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_TMS570_I2C "tms570-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(TMS570I2CState, TMS570_I2C)

struct TMS570I2CState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    I2CBus *bus;
    uint32_t reg[0x100 / 4];
    uint32_t status;
    uint32_t remaining;
    bool active;
};
#endif
