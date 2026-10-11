/* Minimal OBC100 EPS telemetry endpoint on I2C address 0x48.
 * Values are synthetic; this models the firmware's packet protocol only.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/log.h"

#define TYPE_OBC_EPS "obc-eps"
OBJECT_DECLARE_SIMPLE_TYPE(ObcEpsState, OBC_EPS)

struct ObcEpsState {
    I2CSlave parent_obj;
    uint8_t selected;
    uint8_t index;
    bool expect_command;
    uint8_t write_data[16];
    uint8_t write_count;
    uint8_t config[33];
    uint8_t response[157];
    uint8_t soh_period, watchdog, mode;
    uint16_t outputs, boots;
    uint32_t battery_mv;
    uint8_t disconnected, bad_checksum;
    qemu_irq output[16];
    QEMUTimer *timer;
    struct {
        bool active, on;
        int64_t deadline;
    } pending[22]; /* 16 outputs, 3 solar inputs, 3 heaters */
};

static unsigned eps_length(uint8_t command)
{
    switch (command) {
    case 0xe0: return 157;
    case 0xe1: return 76;
    case 0xe2: return 36;
    case 0xe3: return 5;
    case 0xe4: return 52;
    default: return 0;
    }
}

static const uint8_t default_config[33] = {
    0xaa, 0xa0, 0x40, 0, 3, 0x20, 3, 0x20, 3, 0x20,
    0, 0x0a, 0xab, 0x0b, 0x0f, 0x3c, 0xf0, 0x35, 0x20, 0x2d, 0x50,
    0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0x3c, 0, 0x3c, 0, 0x3c,
};

static uint16_t eps_output_mask(ObcEpsState *s)
{
    /* With the automatic mode controller disabled, manual switching is free. */
    if (!(s->config[22] & 0x80)) {
        return 0xffff;
    }
    return lduw_be_p(s->config + 23 + (3 - (s->mode >> 5)) * 2);
}

static void eps_outputs(ObcEpsState *s)
{
    s->outputs &= eps_output_mask(s);
    for (unsigned i = 0; i < 16; i++) {
        qemu_set_irq(s->output[i], !!(s->outputs & (0x8000 >> i)));
    }
}

static void eps_mode(ObcEpsState *s)
{
    uint8_t mode = s->battery_mv >= lduw_be_p(s->config + 15) ? 0x60 :
                   s->battery_mv >= lduw_be_p(s->config + 17) ? 0x40 :
                   s->battery_mv >= lduw_be_p(s->config + 19) ? 0x20 : 0;
    if (mode != s->mode && (s->config[22] & 0x80)) {
        s->mode = mode;
        s->outputs = eps_output_mask(s);
        eps_outputs(s);
    }
}

static void eps_apply(ObcEpsState *s, unsigned channel, bool on)
{
    if (channel < 16) {
        uint16_t bit = 0x8000 >> channel;
        s->outputs = on ? s->outputs | bit : s->outputs & ~bit;
        eps_outputs(s);
    } else if (channel < 19) {
        unsigned shift = 14 - 2 * (channel - 16);
        uint16_t value = lduw_be_p(s->config);
        value = (value & ~(3u << shift)) | ((on ? 2u : 1u) << shift);
        stw_be_p(s->config, value);
    } else {
        unsigned shift = 6 - 2 * (channel - 19);
        s->config[10] = (s->config[10] & ~(3u << shift)) |
                        ((on ? 2u : 1u) << shift);
    }
}

static void eps_schedule(ObcEpsState *s)
{
    int64_t next = INT64_MAX;
    for (unsigned i = 0; i < 22; i++) {
        if (s->pending[i].active) {
            next = MIN(next, s->pending[i].deadline);
        }
    }
    if (next == INT64_MAX) {
        timer_del(s->timer);
    } else {
        timer_mod(s->timer, next);
    }
}

static void eps_tick(void *opaque)
{
    ObcEpsState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned i = 0; i < 22; i++) {
        if (s->pending[i].active && s->pending[i].deadline <= now) {
            eps_apply(s, i, s->pending[i].on);
            s->pending[i].active = false;
        }
    }
    eps_schedule(s);
}

static void eps_switch(ObcEpsState *s, unsigned channel)
{
    if (channel >= 22) {
        return;
    }
    s->pending[channel].on = !!(s->write_data[1] & 0x80);
    s->pending[channel].deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                  (int64_t)s->write_data[2] * NANOSECONDS_PER_SECOND;
    s->pending[channel].active = true;
    eps_tick(s);
}

static void eps_response(ObcEpsState *s)
{
    unsigned length = eps_length(s->selected);
    uint8_t *p = s->response;
    memset(p, 0, sizeof(s->response));
    if (!length) {
        return;
    }
    eps_mode(s);
    p[0] = s->selected == 0xe4 ? 0x31 : length - 2;
    p[1] = s->selected;
    if (s->selected == 0xe2) {
        memcpy(p + 2, s->config, sizeof(s->config));
    } else if (s->selected == 0xe1) {
        p[2] = s->mode;
        stw_be_p(p + 3, s->boots);
    } else if (s->selected == 0xe0) {
        /* Synthetic voltages in mV, reflecting switch state. No load model. */
        for (unsigned i = 0; i < 16; i++) {
            unsigned mv = i < 4 || i == 10 || i == 11 ? 3300 :
                          i < 8 || i == 12 || i == 13 ? 5000 :
                          i < 10 ? 12000 : s->battery_mv;
            stw_be_p(p + 26 + 2 * i, (s->outputs & (0x8000 >> i)) ? mv : 0);
        }
        for (unsigned i = 0; i < 3; i++) {
            stw_be_p(p + 90 + 20 * i, s->battery_mv);
            for (unsigned j = 0; j < 4; j++) {
                stw_be_p(p + 92 + 20 * i + 2 * j, s->battery_mv / 4);
            }
        }
    }
    for (unsigned i = 0; i < length - 1; i++) {
        p[length - 1] ^= p[i];
    }
    if (s->bad_checksum) {
        p[length - 1] ^= 1;
    }
}

static void eps_command(ObcEpsState *s)
{
    unsigned n = s->write_count;
    uint8_t *p = s->write_data;
    switch (s->selected) {
    case 0xd0: if (n == 1) { s->soh_period = p[0]; } break;
    case 0xd1: if (n == 5) { memcpy(s->config + 10, p, 5); } break;
    case 0xd2:
        if (n == 16) {
            memcpy(s->config + 15, p, 16);
            eps_mode(s);
            eps_outputs(s);
        }
        break;
    case 0xd3: if (n == 10) { memcpy(s->config, p, 10); } break;
    case 0xc1: if (n == 3 && (p[0] >> 6) < 3) {
        eps_switch(s, 19 + (p[0] >> 6));
    } break;
    case 0xc3: if (n == 3 && (p[0] >> 6) < 3) {
        eps_switch(s, 16 + (p[0] >> 6));
    } break;
    case 0xc4:
        if (n == 2) {
            for (unsigned i = 0; i < 16; i++) { s->pending[i].active = false; }
            s->outputs = lduw_be_p(p);
            eps_outputs(s);
            eps_schedule(s);
        }
        break;
    case 0xc5: if (n == 3) { eps_switch(s, p[0] >> 4); } break;
    case 0xb1:
        if (n == 1) { memcpy(s->config, default_config, sizeof(s->config)); }
        break;
    case 0xf1: if (n == 1) { s->watchdog = p[0] & 0x60; } break;
    case 0xa1:
        if (n == 1) {
            s->boots++;
            memset(s->pending, 0, sizeof(s->pending));
            timer_del(s->timer);
            s->outputs = lduw_be_p(s->config + 31);
            eps_outputs(s);
        }
        break;
    case 0xa3: case 0xb0: /* No accumulated battery faults in this model. */
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe4:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "obc-eps: command 0x%02x\n", s->selected);
    }
}

static int eps_event(I2CSlave *slave, enum i2c_event event)
{
    ObcEpsState *s = OBC_EPS(slave);
    if (s->disconnected &&
        (event == I2C_START_SEND || event == I2C_START_RECV)) {
        return -1; /* Address NACK: the external device is absent. */
    }
    if (event == I2C_START_SEND) {
        s->expect_command = true;
        s->write_count = 0;
    } else if (event == I2C_START_RECV) {
        s->index = 0;
        eps_response(s);
    } else if (event == I2C_FINISH) {
        if (s->write_count) {
            eps_command(s);
            s->write_count = 0;
        }
    }
    return 0;
}

static int eps_send(I2CSlave *slave, uint8_t data)
{
    ObcEpsState *s = OBC_EPS(slave);
    if (s->expect_command) {
        s->selected = data;
        s->expect_command = false;
    } else if (s->write_count < sizeof(s->write_data)) {
        s->write_data[s->write_count++] = data;
    }
    return 0;
}

static uint8_t eps_recv(I2CSlave *slave)
{
    ObcEpsState *s = OBC_EPS(slave);
    unsigned length = eps_length(s->selected);
    unsigned index = s->index++;

    if (!length || index >= length) {
        return 0;
    }
    return s->response[index];
}

static void eps_reset(DeviceState *dev)
{
    ObcEpsState *s = OBC_EPS(dev);
    s->selected = 0xe3;
    s->index = 0;
    s->expect_command = false;
    s->write_count = 0;
    memcpy(s->config, default_config, sizeof(s->config));
    s->soh_period = 1;
    s->watchdog = 0;
    s->boots = 1;
    s->mode = 0x60;
    s->outputs = lduw_be_p(s->config + 31);
    memset(s->pending, 0, sizeof(s->pending));
    timer_del(s->timer);
    eps_outputs(s);
}

static void eps_init(Object *obj)
{
    ObcEpsState *s = OBC_EPS(obj);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, eps_tick, s);
    qdev_init_gpio_out_named(DEVICE(obj), s->output, "output", 16);
    /* Runtime external inputs; distinct from startup-only qdev properties. */
    object_property_add_uint32_ptr(obj, "input-battery-mv", &s->battery_mv,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint8_ptr(obj, "disconnected", &s->disconnected,
                                  OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint8_ptr(obj, "bad-checksum", &s->bad_checksum,
                                  OBJ_PROP_FLAG_READWRITE);
}

static void eps_finalize(Object *obj)
{
    timer_free(OBC_EPS(obj)->timer);
}

static const Property eps_properties[] = {
    DEFINE_PROP_UINT32("battery-mv", ObcEpsState, battery_mv, 16000),
};

static void eps_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    device_class_set_props(DEVICE_CLASS(klass), eps_properties);
    ic->event = eps_event;
    ic->send = eps_send;
    ic->recv = eps_recv;
    device_class_set_legacy_reset(DEVICE_CLASS(klass), eps_reset);
}

static const TypeInfo eps_info = {
    .name = TYPE_OBC_EPS,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(ObcEpsState),
    .class_init = eps_class_init,
    .instance_init = eps_init,
    .instance_finalize = eps_finalize,
};

static void eps_register_types(void)
{
    type_register_static(&eps_info);
}
type_init(eps_register_types)
