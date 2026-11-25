/*
 * NXP PCA9575 16-bit I2C-bus GPIO expander
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qapi/visitor.h"
#include "qom/object.h"

#define TYPE_PCA9575 "pca9575"
OBJECT_DECLARE_SIMPLE_TYPE(PCA9575State, PCA9575)

enum {
    PCA9575_INPUT = 0x00,
    PCA9575_POLARITY = 0x02,
    PCA9575_BUS_HOLD = 0x04,
    PCA9575_PULL = 0x06,
    PCA9575_CONFIG = 0x08,
    PCA9575_OUTPUT = 0x0a,
    PCA9575_MASK = 0x0c,
    PCA9575_STATUS = 0x0e,
    PCA9575_REG_COUNT = 16,
    PCA9575_PIN_COUNT = 16,
    PCA9575_AUTOINC = 0x80,
};

struct PCA9575State {
    I2CSlave parent_obj;
    uint8_t regs[PCA9575_REG_COUNT];
    uint8_t external[2];
    uint8_t sampled[2];
    uint8_t pointer;
    bool expecting_pointer;
    qemu_irq gpio_out[PCA9575_PIN_COUNT];
    qemu_irq output_enable[PCA9575_PIN_COUNT];
    qemu_irq interrupt; /* Active-low INT output */
};

static void pca9575_update(PCA9575State *state)
{
    int bank, pin;

    for (bank = 0; bank < 2; bank++) {
        uint8_t config = state->regs[PCA9575_CONFIG + bank];
        uint8_t output = state->regs[PCA9575_OUTPUT + bank];

        state->regs[PCA9575_INPUT + bank] =
            (state->external[bank] & config) | (output & ~config);
        state->regs[PCA9575_STATUS + bank] =
            (state->regs[PCA9575_INPUT + bank] ^ state->sampled[bank]) &
            config & ~state->regs[PCA9575_MASK + bank];
        for (pin = 0; pin < 8; pin++) {
            int index = bank * 8 + pin;

            qemu_set_irq(state->gpio_out[index], (output >> pin) & 1);
            qemu_set_irq(state->output_enable[index], !((config >> pin) & 1));
        }
    }
    qemu_set_irq(state->interrupt,
                 !(state->regs[PCA9575_STATUS] |
                   state->regs[PCA9575_STATUS + 1]));
}

static void pca9575_advance(PCA9575State *state)
{
    if (state->pointer & PCA9575_AUTOINC) {
        state->pointer = PCA9575_AUTOINC | ((state->pointer + 1) & 0x0f);
    }
}

static uint8_t pca9575_recv(I2CSlave *i2c)
{
    PCA9575State *state = PCA9575(i2c);
    uint8_t reg = state->pointer & 0x0f;
    uint8_t value = state->regs[reg];

    if (reg < PCA9575_POLARITY) {
        value ^= state->regs[PCA9575_POLARITY + reg];
        state->sampled[reg] = state->regs[reg];
        pca9575_update(state);
    }
    pca9575_advance(state);
    return value;
}

static int pca9575_send(I2CSlave *i2c, uint8_t data)
{
    PCA9575State *state = PCA9575(i2c);
    uint8_t reg = state->pointer & 0x0f;

    if (state->expecting_pointer) {
        if (data & 0x70) {
            return 1;
        }
        state->pointer = data;
        state->expecting_pointer = false;
        return 0;
    }
    if (reg >= PCA9575_POLARITY && reg < PCA9575_STATUS) {
        if (reg == PCA9575_BUS_HOLD || reg == PCA9575_BUS_HOLD + 1) {
            data &= 0x03;
        }
        state->regs[reg] = data;
        pca9575_update(state);
    }
    pca9575_advance(state);
    return 0;
}

static int pca9575_event(I2CSlave *i2c, enum i2c_event event)
{
    PCA9575State *state = PCA9575(i2c);

    if (event == I2C_START_SEND) {
        state->expecting_pointer = true;
    }
    return 0;
}

static void pca9575_gpio_in(void *opaque, int pin, int level)
{
    PCA9575State *state = PCA9575(opaque);
    int bank;
    uint8_t mask;

    assert(pin >= 0 && pin < PCA9575_PIN_COUNT);
    bank = pin / 8;
    mask = 1 << (pin % 8);

    if (level) {
        state->external[bank] |= mask;
    } else {
        state->external[bank] &= ~mask;
    }
    pca9575_update(state);
}

static void pca9575_get_input(Object *obj, Visitor *visitor,
                              const char *name, void *opaque, Error **errp)
{
    PCA9575State *state = PCA9575(obj);
    int bank = (uintptr_t)opaque;
    uint8_t value = state->external[bank];

    visit_type_uint8(visitor, name, &value, errp);
}

static void pca9575_set_input(Object *obj, Visitor *visitor,
                              const char *name, void *opaque, Error **errp)
{
    PCA9575State *state = PCA9575(obj);
    int bank = (uintptr_t)opaque;
    uint8_t value;

    if (visit_type_uint8(visitor, name, &value, errp)) {
        state->external[bank] = value;
        pca9575_update(state);
    }
}

static void pca9575_reset_hold(Object *obj, ResetType type)
{
    PCA9575State *state = PCA9575(obj);
    int bank;

    memset(state->regs, 0, sizeof(state->regs));
    for (bank = 0; bank < 2; bank++) {
        state->regs[PCA9575_PULL + bank] = 0xff;
        state->regs[PCA9575_CONFIG + bank] = 0xff;
        state->regs[PCA9575_MASK + bank] = 0xff;
        state->sampled[bank] = state->external[bank];
    }
    state->pointer = 0;
    state->expecting_pointer = true;
}

static void pca9575_reset_exit(Object *obj, ResetType type)
{
    /* Drive the output lines only once every device has reset its state. */
    pca9575_update(PCA9575(obj));
}

static const VMStateDescription pca9575_vmstate = {
    .name = TYPE_PCA9575,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, PCA9575State),
        VMSTATE_UINT8_ARRAY(regs, PCA9575State, PCA9575_REG_COUNT),
        VMSTATE_UINT8_ARRAY(external, PCA9575State, 2),
        VMSTATE_UINT8_ARRAY(sampled, PCA9575State, 2),
        VMSTATE_UINT8(pointer, PCA9575State),
        VMSTATE_BOOL(expecting_pointer, PCA9575State),
        VMSTATE_END_OF_LIST()
    }
};

static void pca9575_init(Object *obj)
{
    PCA9575State *state = PCA9575(obj);
    DeviceState *dev = DEVICE(obj);
    int bank;

    qdev_init_gpio_in(dev, pca9575_gpio_in, PCA9575_PIN_COUNT);
    qdev_init_gpio_out(dev, state->gpio_out, PCA9575_PIN_COUNT);
    qdev_init_gpio_out_named(dev, state->output_enable, "output-enable",
                             PCA9575_PIN_COUNT);
    qdev_init_gpio_out_named(dev, &state->interrupt, "int", 1);
    for (bank = 0; bank < 2; bank++) {
        g_autofree char *input = g_strdup_printf("input%d", bank);
        g_autofree char *output = g_strdup_printf("output%d", bank);
        g_autofree char *config = g_strdup_printf("config%d", bank);

        object_property_add(obj, input, "uint8", &uint8_type_info,
                            pca9575_get_input, pca9575_set_input, NULL,
                            (void *)(uintptr_t)bank);
        object_property_add_uint8_ptr(obj, output,
                                     &state->regs[PCA9575_OUTPUT + bank],
                                     OBJ_PROP_FLAG_READ);
        object_property_add_uint8_ptr(obj, config,
                                     &state->regs[PCA9575_CONFIG + bank],
                                     OBJ_PROP_FLAG_READ);
    }
}

static void pca9575_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    ic->event = pca9575_event;
    ic->recv = pca9575_recv;
    ic->send = pca9575_send;
    rc->phases.hold = pca9575_reset_hold;
    rc->phases.exit = pca9575_reset_exit;
    dc->vmsd = &pca9575_vmstate;
}

static const TypeInfo pca9575_info = {
    .name = TYPE_PCA9575,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(PCA9575State),
    .instance_init = pca9575_init,
    .class_init = pca9575_class_init,
};

static void pca9575_register_types(void)
{
    type_register_static(&pca9575_info);
}

type_init(pca9575_register_types)
