/*
 * TI TMS570 Real Time Interrupt module (RTI)
 *
 * Counter 0 compare 0 is the FreeRTOS tick on this firmware: os_port.c sets
 * CNT0_COMP0 to (configCPU_CLOCK_HZ / 2) / configTICK_RATE_HZ and enables
 * SETINTENA bit 0, then vPortPreemptiveTick clears INTFLAG bit 0.
 *
 * The free running counters are derived from the host clock on read rather
 * than stepped, so only the compares need real timers.
 *
 * The digital watchdog is modelled far enough to reset the machine when the
 * guest stops feeding it, which is the behaviour task_watchdog relies on.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "hw/timer/tms570_rti.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "system/runstate.h"

REG32(GCTRL,        0x00)
REG32(TBCTRL,       0x04)
REG32(CAPCTRL,      0x08)
REG32(COMPCTRL,     0x0C)
#define A_CNT0      0x10   /* two counters, 0x20 apart */
#define A_CMP0      0x50   /* four compares, 8 bytes each */
REG32(TBLCOMP,      0x70)
REG32(TBHCOMP,      0x74)
REG32(SETINTENA,    0x80)
REG32(CLEARINTENA,  0x84)
REG32(INTFLAG,      0x88)
REG32(DWDCTRL,      0x90)
REG32(DWDPRLD,      0x94)
REG32(WDSTATUS,     0x98)
REG32(WDKEY,        0x9C)
REG32(DWDCNTR,      0xA0)
REG32(WWDRXNCTRL,   0xA4)
REG32(WWDSIZECTRL,  0xA8)
REG32(INTCLRENABLE, 0xAC)
#define A_COMP0CLR  0xB0

/* Counter register offsets within a CNT block. */
#define CNT_FRC     0x00
#define CNT_UC      0x04
#define CNT_CPUC    0x08
#define CNT_CAFRC   0x10
#define CNT_CAUC    0x14

#define DWD_ENABLE_KEY  0xA98559DA
#define DWD_DISABLE_KEY 0x5312ACED

static uint32_t rti_counter_value(TMS570RtiState *s, int n);
static void rti_reschedule(TMS570RtiState *s);

/* Effective tick rate of a free running counter, after its prescaler. */
static uint32_t rti_frc_hz(TMS570RtiState *s, int n)
{
    uint32_t div = s->cnt[n].cpuc + 1;

    return div ? s->clk_hz / div : s->clk_hz;
}

static uint32_t rti_counter_value(TMS570RtiState *s, int n)
{
    TMS570RtiCounter *c = &s->cnt[n];
    int64_t now, elapsed_ns;
    uint32_t hz;

    if (!c->running) {
        return c->frc;
    }

    hz = rti_frc_hz(s, n);
    if (!hz) {
        return c->frc;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    elapsed_ns = now - c->base_ns;
    return c->frc + (uint32_t)muldiv64(elapsed_ns, hz, NANOSECONDS_PER_SECOND);
}

/* Latch the current value and restart the derivation from now. */
static void rti_counter_sync(TMS570RtiState *s, int n)
{
    TMS570RtiCounter *c = &s->cnt[n];

    if (c->running) {
        c->frc = rti_counter_value(s, n);
        c->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
}

/* Which counter drives compare i, per COMPCTRL. */
static int rti_compare_source(TMS570RtiState *s, int i)
{
    return (s->compctrl >> (i * 4)) & 1;
}

static void rti_update_irq(TMS570RtiState *s)
{
    int i;

    for (i = 0; i < TMS570_RTI_NUM_COMPARES; i++) {
        qemu_set_irq(s->cmp_irq[i], (s->intflag & s->setintena & (1u << i)) != 0);
    }
}

static void rti_timer_cb(void *opaque)
{
    TMS570RtiState *s = TMS570_RTI(opaque);
    int i;

    for (i = 0; i < TMS570_RTI_NUM_COMPARES; i++) {
        int n = rti_compare_source(s, i);
        uint32_t now;

        if (!s->cnt[n].running) {
            continue;
        }
        now = rti_counter_value(s, n);
        /* Signed difference copes with the 32-bit counter wrapping. */
        if ((int32_t)(now - s->next[i]) >= 0) {
            s->intflag |= 1u << i;
            s->next[i] += s->udcp[i] ? s->udcp[i] : 1;
        }
    }

    rti_update_irq(s);
    rti_reschedule(s);
}

/* Arm the host timer for whichever compare fires first. */
static void rti_reschedule(TMS570RtiState *s)
{
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t best = INT64_MAX;
    int i;

    for (i = 0; i < TMS570_RTI_NUM_COMPARES; i++) {
        int n = rti_compare_source(s, i);
        uint32_t hz, now;
        int32_t delta;
        int64_t when;

        if (!s->cnt[n].running || !(s->setintena & (1u << i))) {
            continue;
        }
        hz = rti_frc_hz(s, n);
        if (!hz) {
            continue;
        }
        now = rti_counter_value(s, n);
        delta = (int32_t)(s->next[i] - now);
        if (delta < 0) {
            delta = 0;
        }
        when = now_ns + muldiv64(delta, NANOSECONDS_PER_SECOND, hz);
        if (when < best) {
            best = when;
        }
    }

    if (best == INT64_MAX) {
        timer_del(s->timer);
    } else {
        timer_mod_ns(s->timer, best);
    }
}

static void rti_dwd_cb(void *opaque)
{
    TMS570RtiState *s = TMS570_RTI(opaque);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "tms570-rti: watchdog expired, resetting\n");
    s->wdstatus |= 0x02;  /* time out */
    qemu_irq_pulse(s->watchdog_reset);
}

static void rti_dwd_rearm(TMS570RtiState *s)
{
    uint32_t preload = (s->dwdprld & 0xFFF) + 1;
    /* The down counter runs at RTICLK/2^13 * (preload). */
    uint64_t ticks = (uint64_t)preload << 13;
    int64_t ns;

    if (!s->dwd_running || !s->clk_hz) {
        timer_del(s->dwd_timer);
        return;
    }
    ns = muldiv64(ticks, NANOSECONDS_PER_SECOND, s->clk_hz);
    s->dwd_expire_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns;
    timer_mod_ns(s->dwd_timer, s->dwd_expire_ns);
}

static uint64_t rti_read(void *opaque, hwaddr addr, unsigned size)
{
    TMS570RtiState *s = TMS570_RTI(opaque);
    int i;

    if (addr >= A_CNT0 && addr < A_CNT0 + TMS570_RTI_NUM_COUNTERS * 0x20) {
        hwaddr off = addr - A_CNT0;
        int n = off / 0x20;

        switch (off % 0x20) {
        case CNT_FRC:
            return rti_counter_value(s, n);
        case CNT_UC:
            return s->cnt[n].uc;
        case CNT_CPUC:
            return s->cnt[n].cpuc;
        case CNT_CAFRC:
        case CNT_CAUC:
            return 0;
        default:
            return 0;
        }
    }

    if (addr >= A_CMP0 && addr < A_CMP0 + TMS570_RTI_NUM_COMPARES * 8) {
        hwaddr off = addr - A_CMP0;

        i = off / 8;
        return (off % 8) ? s->udcp[i] : s->comp[i];
    }

    if (addr >= A_COMP0CLR && addr < A_COMP0CLR + 0x10) {
        return 0;
    }

    switch (addr) {
    case A_GCTRL:
        return s->gctrl;
    case A_TBCTRL:
        return s->tbctrl;
    case A_CAPCTRL:
        return s->capctrl;
    case A_COMPCTRL:
        return s->compctrl;
    case A_SETINTENA:
    case A_CLEARINTENA:
        return s->setintena;
    case A_INTFLAG:
        return s->intflag;
    case A_DWDCTRL:
        return s->dwdctrl;
    case A_DWDPRLD:
        return s->dwdprld;
    case A_WDSTATUS:
        return s->wdstatus;
    case A_WDKEY:
        return s->wdkey;
    case A_DWDCNTR: {
        int64_t left;
        if (!s->dwd_running) {
            return (s->dwdprld & 0xFFF) << 13;
        }
        left = s->dwd_expire_ns - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (left < 0) {
            left = 0;
        }
        return (uint32_t)muldiv64(left, s->clk_hz, NANOSECONDS_PER_SECOND);
    }
    case A_TBLCOMP:
    case A_TBHCOMP:
    case A_WWDRXNCTRL:
    case A_WWDSIZECTRL:
    case A_INTCLRENABLE:
        return 0;
    default:
        break;
    }

    qemu_log_mask(LOG_UNIMP, "tms570-rti: read from unhandled 0x%" HWADDR_PRIx
                  "\n", addr);
    return 0;
}

static void rti_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TMS570RtiState *s = TMS570_RTI(opaque);
    uint32_t v = val;
    int i, n;

    if (addr >= A_CNT0 && addr < A_CNT0 + TMS570_RTI_NUM_COUNTERS * 0x20) {
        hwaddr off = addr - A_CNT0;

        n = off / 0x20;
        switch (off % 0x20) {
        case CNT_FRC:
            s->cnt[n].frc = v;
            s->cnt[n].base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            rti_reschedule(s);
            return;
        case CNT_UC:
            s->cnt[n].uc = v;
            return;
        case CNT_CPUC:
            rti_counter_sync(s, n);
            s->cnt[n].cpuc = v;
            rti_reschedule(s);
            return;
        default:
            return;
        }
    }

    if (addr >= A_CMP0 && addr < A_CMP0 + TMS570_RTI_NUM_COMPARES * 8) {
        hwaddr off = addr - A_CMP0;

        i = off / 8;
        if (off % 8) {
            s->udcp[i] = v;
        } else {
            s->comp[i] = v;
            s->next[i] = v;
        }
        rti_reschedule(s);
        return;
    }

    if (addr >= A_COMP0CLR && addr < A_COMP0CLR + 0x10) {
        return;
    }

    switch (addr) {
    case A_GCTRL: {
        uint32_t started = v & ~s->gctrl;
        uint32_t stopped = s->gctrl & ~v;

        for (n = 0; n < TMS570_RTI_NUM_COUNTERS; n++) {
            if (stopped & (1u << n)) {
                rti_counter_sync(s, n);
                s->cnt[n].running = false;
            }
            if (started & (1u << n)) {
                s->cnt[n].base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                s->cnt[n].running = true;
            }
        }
        s->gctrl = v;
        rti_reschedule(s);
        return;
    }
    case A_TBCTRL:
        s->tbctrl = v;
        return;
    case A_CAPCTRL:
        s->capctrl = v;
        return;
    case A_COMPCTRL:
        s->compctrl = v;
        rti_reschedule(s);
        return;
    case A_SETINTENA:
        s->setintena |= v;
        rti_update_irq(s);
        rti_reschedule(s);
        return;
    case A_CLEARINTENA:
        s->setintena &= ~v;
        rti_update_irq(s);
        rti_reschedule(s);
        return;
    case A_INTFLAG:
        s->intflag &= ~v;   /* write one to clear */
        rti_update_irq(s);
        return;

    case A_DWDCTRL:
        s->dwdctrl = v;
        if (v == DWD_ENABLE_KEY) {
            s->dwd_running = true;
            rti_dwd_rearm(s);
        } else if (v == DWD_DISABLE_KEY) {
            s->dwd_running = false;
            timer_del(s->dwd_timer);
        }
        return;
    case A_DWDPRLD:
        s->dwdprld = v;
        rti_dwd_rearm(s);
        return;
    case A_WDSTATUS:
        s->wdstatus &= ~v;
        return;
    case A_WDKEY:
        /* 0xE51A followed by 0xA35C feeds the watchdog. */
        if (s->wdkey == 0xE51A && (v & 0xFFFF) == 0xA35C) {
            rti_dwd_rearm(s);
        }
        s->wdkey = v & 0xFFFF;
        return;
    case A_TBLCOMP:
    case A_TBHCOMP:
    case A_WWDRXNCTRL:
    case A_WWDSIZECTRL:
    case A_INTCLRENABLE:
        return;
    default:
        break;
    }

    qemu_log_mask(LOG_UNIMP, "tms570-rti: write 0x%08x to unhandled 0x%"
                  HWADDR_PRIx "\n", v, addr);
}

static const MemoryRegionOps rti_ops = {
    .read = rti_read,
    .write = rti_write,
    /*
     * The SoC runs big-endian and the CPU's data accesses are big-endian too
     * (see tms570lc43_realize()), so registers pass through unswapped.
     */
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void rti_reset(DeviceState *dev)
{
    TMS570RtiState *s = TMS570_RTI(dev);

    timer_del(s->timer);
    timer_del(s->dwd_timer);

    s->gctrl = 0;
    s->tbctrl = 0;
    s->capctrl = 0;
    s->compctrl = 0;
    s->setintena = 0;
    s->intflag = 0;
    memset(s->cnt, 0, sizeof(s->cnt));
    memset(s->comp, 0, sizeof(s->comp));
    memset(s->udcp, 0, sizeof(s->udcp));
    memset(s->next, 0, sizeof(s->next));

    s->dwdctrl = DWD_DISABLE_KEY;
    s->dwdprld = 0xFFF;
    s->wdstatus = 0;
    s->wdkey = 0;
    s->dwd_running = false;

    rti_update_irq(s);
}

static void rti_realize(DeviceState *dev, Error **errp)
{
    TMS570RtiState *s = TMS570_RTI(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rti_timer_cb, s);
    s->dwd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rti_dwd_cb, s);
}

static void rti_instance_init(Object *obj)
{
    TMS570RtiState *s = TMS570_RTI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &rti_ops, s, "tms570-rti", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);

    for (i = 0; i < TMS570_RTI_NUM_COMPARES; i++) {
        sysbus_init_irq(sbd, &s->cmp_irq[i]);
    }
    sysbus_init_irq(sbd, &s->watchdog_reset);
}

static const Property rti_properties[] = {
    /* RTICLK: VCLK / 2 with the firmware's GCM settings. */
    /* The current OBC image configures its RTI using configCPU_CLOCK_HZ=2.3MHz. */
    DEFINE_PROP_UINT32("clk-hz", TMS570RtiState, clk_hz, 2300000),
};

static const VMStateDescription vmstate_tms570_rti = {
    .name = "tms570-rti",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gctrl, TMS570RtiState),
        VMSTATE_UINT32(compctrl, TMS570RtiState),
        VMSTATE_UINT32(setintena, TMS570RtiState),
        VMSTATE_UINT32(intflag, TMS570RtiState),
        VMSTATE_UINT32_ARRAY(comp, TMS570RtiState, TMS570_RTI_NUM_COMPARES),
        VMSTATE_UINT32_ARRAY(udcp, TMS570RtiState, TMS570_RTI_NUM_COMPARES),
        VMSTATE_UINT32_ARRAY(next, TMS570RtiState, TMS570_RTI_NUM_COMPARES),
        VMSTATE_END_OF_LIST()
    }
};

static void rti_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rti_realize;
    device_class_set_legacy_reset(dc, rti_reset);
    device_class_set_props(dc, rti_properties);
    dc->vmsd = &vmstate_tms570_rti;
    dc->desc = "TI TMS570 real time interrupt module";
}

static const TypeInfo tms570_rti_info = {
    .name          = TYPE_TMS570_RTI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TMS570RtiState),
    .instance_init = rti_instance_init,
    .class_init    = rti_class_init,
};

static void tms570_rti_register_types(void)
{
    type_register_static(&tms570_rti_info);
}

type_init(tms570_rti_register_types)
