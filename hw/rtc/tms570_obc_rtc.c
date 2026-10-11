/* OBC100 I2C RTC register subset (address 0x69).
 * Local experiment, not intended for upstream.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "qemu/bcd.h"
#include "qom/object.h"
#include "system/rtc.h"
#include "qapi/visitor.h"
#include "qapi/error.h"
#include "qemu/cutils.h"

#define TYPE_OBC_RTC "obc-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(ObcRtcState, OBC_RTC)

struct ObcRtcState {
    I2CSlave parent_obj;
    uint8_t reg[0x60];
    uint8_t ptr;
    bool expect_address;
    time_t offset;
    int64_t time_shift;
    bool frozen;
};

static void rtc_capture_time(ObcRtcState *s)
{
    struct tm now;

    if (s->frozen || !(s->reg[0] & 1) || (s->reg[1] & 2)) {
        return;
    }
    qemu_get_timedate(&now, s->offset + s->time_shift);
    s->reg[6] = to_bcd(now.tm_sec);
    s->reg[7] = to_bcd(now.tm_min);
    s->reg[8] = to_bcd(now.tm_hour);
    s->reg[9] = now.tm_wday & 7;
    s->reg[10] = to_bcd(now.tm_mday);
    s->reg[11] = to_bcd(now.tm_mon + 1);
    s->reg[12] = to_bcd((now.tm_year - 70) % 100);
}

static void rtc_commit_time(ObcRtcState *s)
{
    struct tm reference;
    struct tm value = {
        .tm_sec = from_bcd(s->reg[6]),
        .tm_min = from_bcd(s->reg[7]),
        .tm_hour = from_bcd(s->reg[8]),
        .tm_mday = from_bcd(s->reg[10]),
        .tm_mon = from_bcd(s->reg[11]) - 1,
        .tm_year = from_bcd(s->reg[12]) + 70,
    };
    /* Use the same clock as capture (including -rtc clock=vm), rather than
     * qemu_timedate_diff(), which always compares against the host clock. */
    qemu_get_timedate(&reference, 0);
    s->offset = mktimegm(&value) - mktimegm(&reference) - s->time_shift;
}

/* External fault controls persist across an MCU reset. */
static bool rtc_get_frozen(Object *obj, Error **errp)
{
    return OBC_RTC(obj)->frozen;
}

static void rtc_set_frozen(Object *obj, bool value, Error **errp)
{
    ObcRtcState *s = OBC_RTC(obj);
    if (value && !s->frozen) {
        rtc_capture_time(s);
    } else if (!value && s->frozen) {
        rtc_commit_time(s); /* Resume from the stopped clock value. */
    }
    s->frozen = value;
}

static void rtc_get_shift(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    int64_t value = OBC_RTC(obj)->time_shift;
    visit_type_int64(v, name, &value, errp);
}

static void rtc_set_shift(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    int64_t value;
    if (!visit_type_int64(v, name, &value, errp)) {
        return;
    }
    if (value < -315360000 || value > 315360000) {
        error_setg(errp, "RTC shift must be within ten years");
        return;
    }
    ObcRtcState *s = OBC_RTC(obj);
    if (s->frozen) {
        /* A time jump is an external adjustment even when the oscillator
         * is stopped. Apply it to the captured calendar immediately. */
        struct tm captured = {
            .tm_sec = from_bcd(s->reg[6]),
            .tm_min = from_bcd(s->reg[7]),
            .tm_hour = from_bcd(s->reg[8]),
            .tm_mday = from_bcd(s->reg[10]),
            .tm_mon = from_bcd(s->reg[11]) - 1,
            .tm_year = from_bcd(s->reg[12]) + 70,
        };
        time_t changed = mktimegm(&captured) + value - s->time_shift;
        gmtime_r(&changed, &captured);
        s->reg[6] = to_bcd(captured.tm_sec);
        s->reg[7] = to_bcd(captured.tm_min);
        s->reg[8] = to_bcd(captured.tm_hour);
        s->reg[9] = captured.tm_wday & 7;
        s->reg[10] = to_bcd(captured.tm_mday);
        s->reg[11] = to_bcd(captured.tm_mon + 1);
        s->reg[12] = to_bcd((captured.tm_year - 70) % 100);
    }
    s->time_shift = value;
}

static void rtc_init(Object *obj)
{
    object_property_add_bool(obj, "frozen", rtc_get_frozen, rtc_set_frozen);
    object_property_add(obj, "time-shift-seconds", "int",
                        rtc_get_shift, rtc_set_shift, NULL, NULL);
}

static int rtc_event(I2CSlave *slave, enum i2c_event event)
{
    ObcRtcState *s = OBC_RTC(slave);
    if (event == I2C_START_SEND) {
        s->expect_address = true;
    }
    return 0;
}

static uint8_t rtc_recv(I2CSlave *slave)
{
    ObcRtcState *s = OBC_RTC(slave);
    if (s->ptr >= 6 && s->ptr <= 12) {
        rtc_capture_time(s);
    }
    uint8_t value = s->reg[s->ptr];
    s->ptr = (s->ptr + 1) % sizeof(s->reg);
    return value;
}

static int rtc_send(I2CSlave *slave, uint8_t data)
{
    ObcRtcState *s = OBC_RTC(slave);
    if (s->expect_address) {
        s->ptr = data % sizeof(s->reg);
        s->expect_address = false;
    } else {
        uint8_t old = s->reg[s->ptr];
        s->reg[s->ptr] = data;
        if (s->ptr == 1 && !(old & 2) && (data & 2)) {
            rtc_commit_time(s);
        }
        s->ptr = (s->ptr + 1) % sizeof(s->reg);
    }
    return 0;
}

/*
 * The RTC is a separate battery-backed chip: an MCU reset (software reset,
 * watchdog) must not lose its time or settings.  Load the host time once at
 * realize; a machine reset only abandons any half-finished I2C transfer.
 */
static void rtc_reset(DeviceState *dev)
{
    ObcRtcState *s = OBC_RTC(dev);

    s->ptr = 0;
    s->expect_address = false;
}

static void rtc_realize(DeviceState *dev, Error **errp)
{
    ObcRtcState *s = OBC_RTC(dev);
    struct tm now;

    memset(s->reg, 0, sizeof(s->reg));
    qemu_get_timedate(&now, 0);
    s->reg[0] = 1; /* oscillator running, out of software reset */
    s->reg[6] = to_bcd(now.tm_sec);
    s->reg[7] = to_bcd(now.tm_min);
    s->reg[8] = to_bcd(now.tm_hour);
    s->reg[9] = to_bcd(now.tm_wday + 1);
    s->reg[10] = to_bcd(now.tm_mday);
    s->reg[11] = to_bcd(now.tm_mon + 1);
    /* The OBC RTC driver interprets this field as years since 1970. */
    s->reg[12] = to_bcd((now.tm_year - 70) % 100);
    s->offset = 0;
    rtc_reset(dev);
}

static void rtc_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    ic->event = rtc_event;
    ic->recv = rtc_recv;
    ic->send = rtc_send;
    DEVICE_CLASS(klass)->realize = rtc_realize;
    device_class_set_legacy_reset(DEVICE_CLASS(klass), rtc_reset);
}

static const TypeInfo rtc_info = {
    .name = TYPE_OBC_RTC,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(ObcRtcState),
    .class_init = rtc_class_init,
    .instance_init = rtc_init,
};

static void rtc_register_types(void)
{
    type_register_static(&rtc_info);
}
type_init(rtc_register_types)
