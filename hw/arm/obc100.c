/*
 * OBC100 -- SATELLA cubesat on-board computer (TI TMS570LC4357)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "qapi/error.h"
#include "hw/arm/tms570lc43.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "elf.h"
#include "target/arm/internals.h"

struct Obc100MachineState {
    MachineState parent_obj;
    TMS570LC43State soc;
    CanBusState *canbus[4];
    Chardev *spi5_peer;
};

#define TYPE_OBC100_MACHINE MACHINE_TYPE_NAME("obc100")
OBJECT_DECLARE_SIMPLE_TYPE(Obc100MachineState, OBC100_MACHINE)

static struct {
    ARMCPU *cpu;
    uint64_t entry;
} obc100_boot;

static void obc100_cpu_reset(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    cpu_reset(CPU(cpu));

    /*
     * cfgend put SCTLR.B and SCTLR.EE in reset_sctlr, but CPSR.E is what
     * actually selects the data endianness for the current mode and reset
     * does not derive it from SCTLR.EE.  Set it here, the same way
     * hw/arm/boot.c does for BE8 kernels.
     */
    env->uncached_cpsr |= CPSR_E;
    /* Writing CPSR directly leaves the cached TB flags stale. */
    arm_rebuild_hflags(env);

    /*
     * The reset vector at 0x00000000 branches to _c_int00, so entering at the
     * ELF entry point would skip the vector table.  Start at 0 like the real
     * part does and let the firmware's own vectors take over.
     */
    cpu_set_pc(CPU(cpu), 0);
}

static void obc100_load_firmware(MachineState *machine, ARMCPU *cpu)
{
    const char *filename = machine->kernel_filename;
    uint64_t entry, lowaddr, highaddr;
    ssize_t size;

    if (!filename) {
        return;
    }

    /*
     * The firmware is a big-endian ELF without EF_ARM_BE8.  This board keeps
     * physical memory in big-endian order for both instruction and data
     * accesses, so load the bytes unchanged (data_swab = 0).
     */
    size = load_elf_as(filename, NULL, NULL, NULL, &entry, &lowaddr, &highaddr,
                       NULL, ELFDATA2MSB, EM_ARM, 1, 0, NULL);
    if (size < 0) {
        error_report("obc100: could not load firmware '%s': %s",
                     filename, load_elf_strerror(size));
        exit(1);
    }

    obc100_boot.cpu = cpu;
    obc100_boot.entry = entry;
    qemu_register_reset(obc100_cpu_reset, cpu);
}

static void obc100_init(MachineState *machine)
{
    Obc100MachineState *m = OBC100_MACHINE(machine);
    DriveInfo *sd_drive;

    object_initialize_child(OBJECT(machine), "soc", &m->soc,
                            TYPE_TMS570LC43_SOC);
    object_property_set_link(OBJECT(&m->soc), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    for (unsigned i = 0; i < 4; i++) {
        object_property_set_link(OBJECT(&m->soc.dcan[i]), "canbus",
                                 OBJECT(m->canbus[i]), &error_abort);
    }
    if (m->spi5_peer) {
        qdev_prop_set_chr(DEVICE(&m->soc.spi[3]), "peer", m->spi5_peer);
    }

    /*
     * Character backends have to be attached before the SoC is realized.
     * SCI1 (the LIN1 module) is the console; SCI3 and SCI4 are the ADCS and
     * mission board links, wired up only if the user supplied more ports.
     */
    qdev_prop_set_chr(DEVICE(&m->soc.sci[0]), "chardev", serial_hd(0));
    if (serial_hd(1)) {
        qdev_prop_set_chr(DEVICE(&m->soc.sci[2]), "chardev", serial_hd(1));
    }
    if (serial_hd(2)) {
        qdev_prop_set_chr(DEVICE(&m->soc.sci[3]), "chardev", serial_hd(2));
    }

    sd_drive = drive_get(IF_SD, 0, 0);
    if (sd_drive) {
        qdev_prop_set_drive(DEVICE(&m->soc.spi4), "drive",
                            blk_by_legacy_dinfo(sd_drive));
    }

    sysbus_realize(SYS_BUS_DEVICE(&m->soc), &error_fatal);

    for (unsigned i = 0; i < 2; i++) {
        DeviceState *nand = qdev_new("obc-spi-nand");
        DriveInfo *drive = drive_get_by_index(IF_MTD, i);
        qdev_prop_set_uint8(nand, "cs", i);
        if (drive) {
            qdev_prop_set_drive(nand, "drive", blk_by_legacy_dinfo(drive));
        }
        ssi_realize_and_unref(nand, m->soc.spi4.bus, &error_fatal);
    }

    obc100_load_firmware(machine, &m->soc.cpu);
}

static void obc100_machine_instance_init(Object *obj)
{
    Obc100MachineState *m = OBC100_MACHINE(obj);
    object_property_add_link(obj, "spi5-peer", TYPE_CHARDEV,
                             (Object **)&m->spi5_peer,
                             object_property_allow_set_link, 0);
    for (unsigned i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("canbus%d", i);
        object_property_add_link(obj, name, TYPE_CAN_BUS,
                                 (Object **)&m->canbus[i],
                                 object_property_allow_set_link, 0);
    }
}

static void obc100_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SATELLA OBC100 (TMS570LC4357, Cortex-R5F, big-endian BE-32)";
    mc->init = obc100_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-r5f");
    mc->max_cpus = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
    mc->default_ram_size = 0;
    mc->default_ram_id = NULL;
}

static const TypeInfo obc100_machine_types[] = {
    {
        .name          = TYPE_OBC100_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(Obc100MachineState),
        .instance_init = obc100_machine_instance_init,
        .class_init    = obc100_machine_class_init,
    },
};

DEFINE_TYPES(obc100_machine_types)
