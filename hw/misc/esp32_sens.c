/*
 * ESP32 SENS (SAR ADC / Touch Sensor / DAC) peripheral model
 *
 * Implements SENS MMIO registers at DR_REG_SENS_BASE (0x3ff48800),
 * supporting RTC ADC1 single conversion start, status, and raw result read.
 *
 * Register offsets, bitfields, and reset defaults follow Espressif ESP-IDF:
 * https://github.com/espressif/esp-idf
 * (components/soc/esp32/register/soc/sens_reg.h)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "hw/misc/esp32_sens.h"

#define SENS_SAR_READ_CTRL_REG         0x0000
#define SENS_SAR_READ_STATUS1_REG      0x0004
#define SENS_SAR_MEAS_WAIT1_REG        0x0008
#define SENS_SAR_MEAS_WAIT2_REG        0x000c
#define SENS_SAR_MEAS_CTRL_REG         0x0010
#define SENS_SAR_READ_STATUS2_REG      0x0014
#define SENS_SAR_ULP_CP_SLEEP_CYC0_REG 0x0018
#define SENS_SAR_ULP_CP_SLEEP_CYC1_REG 0x001c
#define SENS_SAR_ULP_CP_SLEEP_CYC2_REG 0x0020
#define SENS_SAR_ULP_CP_SLEEP_CYC3_REG 0x0024
#define SENS_SAR_ULP_CP_SLEEP_CYC4_REG 0x0028
#define SENS_SAR_START_FORCE_REG       0x002c
#define SENS_SAR_MEM_WR_CTRL_REG       0x0030
#define SENS_SAR_ATTEN1_REG            0x0034
#define SENS_SAR_ATTEN2_REG            0x0038
#define SENS_SAR_SLAVE_ADDR1_REG       0x003c
#define SENS_SAR_SLAVE_ADDR2_REG       0x0040
#define SENS_SAR_SLAVE_ADDR3_REG       0x0044
#define SENS_SAR_SLAVE_ADDR4_REG       0x0048
#define SENS_SAR_TSENS_CTRL_REG        0x004c
#define SENS_SAR_I2C_CTRL_REG          0x0050
#define SENS_SAR_MEAS_START1_REG       0x0054
#define SENS_SAR_TOUCH_CTRL1_REG       0x0058
#define SENS_SAR_TOUCH_THRES1_REG      0x005c
#define SENS_SAR_TOUCH_THRES2_REG      0x0060
#define SENS_SAR_TOUCH_THRES3_REG      0x0064
#define SENS_SAR_TOUCH_THRES4_REG      0x0068
#define SENS_SAR_TOUCH_THRES5_REG      0x006c
#define SENS_SAR_TOUCH_OUT1_REG        0x0070
#define SENS_SAR_TOUCH_OUT2_REG        0x0074
#define SENS_SAR_TOUCH_OUT3_REG        0x0078
#define SENS_SAR_TOUCH_OUT4_REG        0x007c
#define SENS_SAR_TOUCH_OUT5_REG        0x0080
#define SENS_SAR_TOUCH_CTRL2_REG       0x0084
#define SENS_SAR_TOUCH_ENABLE_REG      0x008c
#define SENS_SAR_READ_CTRL2_REG        0x0090
#define SENS_SAR_MEAS_START2_REG       0x0094
#define SENS_SAR_DAC_CTRL1_REG         0x0098
#define SENS_SAR_DAC_CTRL2_REG         0x009c

/* SENS_SAR_MEAS_START1_REG field definitions */
#define SENS_SAR1_EN_PAD_FORCE         (1u << 31)
#define SENS_SAR1_EN_PAD_MASK          (0xfffu << 19)
#define SENS_SAR1_EN_PAD_SHIFT         19
#define SENS_MEAS1_START_FORCE         (1u << 18)
#define SENS_MEAS1_START_SAR           (1u << 17)
#define SENS_MEAS1_DONE_SAR            (1u << 16)
#define SENS_MEAS1_DATA_SAR_MASK       0xffffu

/* SENS_SAR_MEAS_START2_REG field definitions */
#define SENS_SAR2_EN_PAD_FORCE         (1u << 31)
#define SENS_SAR2_EN_PAD_MASK          (0xfffu << 19)
#define SENS_SAR2_EN_PAD_SHIFT         19
#define SENS_MEAS2_START_FORCE         (1u << 18)
#define SENS_MEAS2_START_SAR           (1u << 17)
#define SENS_MEAS2_DONE_SAR            (1u << 16)
#define SENS_MEAS2_DATA_SAR_MASK       0xffffu

static void esp32_sens_perform_adc1_conversion(Esp32SensState *s)
{
    /* Identify selected channel from sar1_en_pad bitmap */
    uint32_t pad_mask =
        (s->sar_meas_start1 & SENS_SAR1_EN_PAD_MASK) >> SENS_SAR1_EN_PAD_SHIFT;
    uint16_t sample = 0;

    if (pad_mask != 0) {
        int channel = ctz32(pad_mask);
        if (channel < ESP32_SENS_ADC1_CHANNELS &&
            pad_mask == (1u << channel)) {
            sample = s->adc1_raw_sample[channel];
        } else {
            /* Unsupported channel selection or multiple pads enabled */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: unexpected ADC1 pad enable mask 0x%03x\n",
                          __func__, pad_mask);
            sample = 0;
        }
    }

    /* Clear start bit, assert DONE, update result data */
    s->sar_meas_start1 &= ~SENS_MEAS1_START_SAR;
    s->sar_meas_start1 |= SENS_MEAS1_DONE_SAR;
    s->sar_meas_start1 = (s->sar_meas_start1 & ~SENS_MEAS1_DATA_SAR_MASK) |
                         (sample & SENS_MEAS1_DATA_SAR_MASK);
}

static uint64_t esp32_sens_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32SensState *s = ESP32_SENS(opaque);

    switch (addr) {
    case SENS_SAR_READ_CTRL_REG:
        return s->sar_read_ctrl;
    case SENS_SAR_READ_STATUS1_REG:
        return s->sar_read_status1;
    case SENS_SAR_MEAS_WAIT1_REG:
        return s->sar_meas_wait1;
    case SENS_SAR_MEAS_WAIT2_REG:
        return s->sar_meas_wait2;
    case SENS_SAR_MEAS_CTRL_REG:
        return s->sar_meas_ctrl;
    case SENS_SAR_READ_STATUS2_REG:
        return s->sar_read_status2;
    case SENS_SAR_ULP_CP_SLEEP_CYC0_REG ... SENS_SAR_ULP_CP_SLEEP_CYC4_REG:
        return s->ulp_cp_sleep_cyc[(addr - SENS_SAR_ULP_CP_SLEEP_CYC0_REG) / 4];
    case SENS_SAR_START_FORCE_REG:
        return s->sar_start_force;
    case SENS_SAR_MEM_WR_CTRL_REG:
        return s->sar_mem_wr_ctrl;
    case SENS_SAR_ATTEN1_REG:
        return s->sar_atten1;
    case SENS_SAR_ATTEN2_REG:
        return s->sar_atten2;
    case SENS_SAR_SLAVE_ADDR1_REG ... SENS_SAR_SLAVE_ADDR4_REG:
        return s->sar_slave_addr[(addr - SENS_SAR_SLAVE_ADDR1_REG) / 4];
    case SENS_SAR_TSENS_CTRL_REG:
        return s->sar_tctrl;
    case SENS_SAR_I2C_CTRL_REG:
        return s->sar_i2c_ctrl;
    case SENS_SAR_MEAS_START1_REG:
        return s->sar_meas_start1;
    case SENS_SAR_TOUCH_CTRL1_REG:
        return s->sar_touch_ctrl1;
    case SENS_SAR_TOUCH_THRES1_REG ... SENS_SAR_TOUCH_THRES5_REG:
        return s->touch_thresh[(addr - SENS_SAR_TOUCH_THRES1_REG) / 4];
    case SENS_SAR_TOUCH_OUT1_REG ... SENS_SAR_TOUCH_OUT5_REG:
        return s->touch_meas[(addr - SENS_SAR_TOUCH_OUT1_REG) / 4];
    case SENS_SAR_TOUCH_CTRL2_REG:
        return s->sar_touch_ctrl2;
    case SENS_SAR_TOUCH_ENABLE_REG:
        return s->sar_touch_enable;
    case SENS_SAR_READ_CTRL2_REG:
        return s->sar_read_ctrl2;
    case SENS_SAR_MEAS_START2_REG:
        return s->sar_meas_start2;
    case SENS_SAR_DAC_CTRL1_REG:
        return s->sar_dac_ctrl1;
    case SENS_SAR_DAC_CTRL2_REG:
        return s->sar_dac_ctrl2;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read at 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void esp32_sens_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned int size)
{
    Esp32SensState *s = ESP32_SENS(opaque);
    uint32_t value = (uint32_t)val;

    switch (addr) {
    case SENS_SAR_READ_CTRL_REG:
        s->sar_read_ctrl = value;
        break;
    case SENS_SAR_READ_STATUS1_REG:
        s->sar_read_status1 = value;
        break;
    case SENS_SAR_MEAS_WAIT1_REG:
        s->sar_meas_wait1 = value;
        break;
    case SENS_SAR_MEAS_WAIT2_REG:
        s->sar_meas_wait2 = value;
        break;
    case SENS_SAR_MEAS_CTRL_REG:
        s->sar_meas_ctrl = value;
        break;
    case SENS_SAR_READ_STATUS2_REG:
        s->sar_read_status2 = value;
        break;
    case SENS_SAR_ULP_CP_SLEEP_CYC0_REG ... SENS_SAR_ULP_CP_SLEEP_CYC4_REG:
        s->ulp_cp_sleep_cyc[(addr - SENS_SAR_ULP_CP_SLEEP_CYC0_REG) / 4] =
            value;
        break;
    case SENS_SAR_START_FORCE_REG:
        s->sar_start_force = value;
        break;
    case SENS_SAR_MEM_WR_CTRL_REG:
        s->sar_mem_wr_ctrl = value;
        break;
    case SENS_SAR_ATTEN1_REG:
        s->sar_atten1 = value;
        break;
    case SENS_SAR_ATTEN2_REG:
        s->sar_atten2 = value;
        break;
    case SENS_SAR_SLAVE_ADDR1_REG ... SENS_SAR_SLAVE_ADDR4_REG:
        s->sar_slave_addr[(addr - SENS_SAR_SLAVE_ADDR1_REG) / 4] = value;
        break;
    case SENS_SAR_TSENS_CTRL_REG:
        s->sar_tctrl = value;
        break;
    case SENS_SAR_I2C_CTRL_REG:
        s->sar_i2c_ctrl = value;
        break;
    case SENS_SAR_MEAS_START1_REG:
    {
        uint32_t old_val = s->sar_meas_start1;
        /*
         * Preserve read-only DONE and DATA bits against arbitrary software
         * writes, while allowing writable control bits (pad enable, pad force,
         * start force, start sar).
         */
        uint32_t ro_mask = SENS_MEAS1_DONE_SAR | SENS_MEAS1_DATA_SAR_MASK;
        uint32_t ro_bits = old_val & ro_mask;
        uint32_t rw_bits = value & ~ro_mask;

        /*
         * If software clears start_sar (e.g. adc_oneshot_ll_start clearing
         * before toggling high), clear DONE bit so conversion completion is
         * not asserted prematurely.
         */
        if (!(value & SENS_MEAS1_START_SAR)) {
            ro_bits &= ~SENS_MEAS1_DONE_SAR;
        }

        s->sar_meas_start1 = rw_bits | ro_bits;

        /*
         * If START_SAR transitioned 0 -> 1 with START_FORCE enabled,
         * execute conversion.
         */
        if ((value & SENS_MEAS1_START_FORCE) &&
            (value & SENS_MEAS1_START_SAR) &&
            !(old_val & SENS_MEAS1_START_SAR)) {
            esp32_sens_perform_adc1_conversion(s);
        }
        break;
    }
    case SENS_SAR_TOUCH_CTRL1_REG:
        s->sar_touch_ctrl1 = value;
        break;
    case SENS_SAR_TOUCH_THRES1_REG ... SENS_SAR_TOUCH_THRES5_REG:
        s->touch_thresh[(addr - SENS_SAR_TOUCH_THRES1_REG) / 4] = value;
        break;
    case SENS_SAR_TOUCH_CTRL2_REG:
        s->sar_touch_ctrl2 = value;
        break;
    case SENS_SAR_TOUCH_ENABLE_REG:
        s->sar_touch_enable = value;
        break;
    case SENS_SAR_READ_CTRL2_REG:
        s->sar_read_ctrl2 = value;
        break;
    case SENS_SAR_MEAS_START2_REG:
        s->sar_meas_start2 = value;
        break;
    case SENS_SAR_DAC_CTRL1_REG:
        s->sar_dac_ctrl1 = value;
        break;
    case SENS_SAR_DAC_CTRL2_REG:
        s->sar_dac_ctrl2 = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write at 0x%" HWADDR_PRIx
                      " val 0x%08x\n",
                      __func__, addr, value);
        break;
    }
}

static const MemoryRegionOps esp32_sens_ops = {
    .read = esp32_sens_read,
    .write = esp32_sens_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void esp32_sens_reset_hold(Object *obj, ResetType type)
{
    Esp32SensState *s = ESP32_SENS(obj);

    /*
     * Reset defaults per ESP-IDF soc/sens_reg.h:
     * - SENS_SAR_MEAS_WAIT1_REG: AMP_WAIT2[31:16]=10, AMP_WAIT1[15:0]=10
     * - SENS_SAR_MEAS_WAIT2_REG: SAR2_RSTB_WAIT[27:20]=2, AMP_WAIT3[15:0]=10
     * - SENS_SAR_ATTEN1/2_REG: all channels default to 0xffffffff
     */
    s->sar_read_ctrl = 0;
    s->sar_read_status1 = 0;
    s->sar_meas_wait1 = 0x000a000a;
    s->sar_meas_wait2 = 0x0020000a;
    s->sar_meas_ctrl = 0;
    s->sar_read_status2 = 0;
    memset(s->ulp_cp_sleep_cyc, 0, sizeof(s->ulp_cp_sleep_cyc));
    s->sar_start_force = 0;
    s->sar_mem_wr_ctrl = 0;
    s->sar_atten1 = 0xffffffff;
    s->sar_atten2 = 0xffffffff;
    memset(s->sar_slave_addr, 0, sizeof(s->sar_slave_addr));
    s->sar_tctrl = 0;
    s->sar_i2c_ctrl = 0;
    s->sar_meas_start1 = 0;
    s->sar_touch_ctrl1 = 0;
    memset(s->touch_thresh, 0, sizeof(s->touch_thresh));
    memset(s->touch_meas, 0, sizeof(s->touch_meas));
    s->sar_touch_ctrl2 = 0;
    s->sar_touch_enable = 0;
    s->sar_read_ctrl2 = 0;
    s->sar_meas_start2 = 0;
    s->sar_dac_ctrl1 = 0;
    s->sar_dac_ctrl2 = 0;
}

static void esp32_sens_get_sample(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    Esp32SensState *s = ESP32_SENS(obj);
    int ch = (uintptr_t)opaque;
    uint16_t val = s->adc1_raw_sample[ch];

    visit_type_uint16(v, name, &val, errp);
}

static void esp32_sens_set_sample(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    Esp32SensState *s = ESP32_SENS(obj);
    int ch = (uintptr_t)opaque;
    uint16_t val;

    if (visit_type_uint16(v, name, &val, errp)) {
        s->adc1_raw_sample[ch] = val;
    }
}

static void esp32_sens_init(Object *obj)
{
    Esp32SensState *s = ESP32_SENS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32_sens_ops, s,
                          TYPE_ESP32_SENS, ESP32_SENS_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    for (int ch = 0; ch < ESP32_SENS_ADC1_CHANNELS; ch++) {
        g_autofree char *prop_name = g_strdup_printf("adc1-ch%d", ch);
        object_property_add(obj, prop_name, "uint16",
                            esp32_sens_get_sample, esp32_sens_set_sample,
                            NULL, (void *)(uintptr_t)ch);
    }
}

static const VMStateDescription vmstate_esp32_sens = {
    .name = "esp32.sens",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(sar_read_ctrl, Esp32SensState),
        VMSTATE_UINT32(sar_read_status1, Esp32SensState),
        VMSTATE_UINT32(sar_meas_wait1, Esp32SensState),
        VMSTATE_UINT32(sar_meas_wait2, Esp32SensState),
        VMSTATE_UINT32(sar_meas_ctrl, Esp32SensState),
        VMSTATE_UINT32(sar_read_status2, Esp32SensState),
        VMSTATE_UINT32_ARRAY(ulp_cp_sleep_cyc, Esp32SensState, 5),
        VMSTATE_UINT32(sar_start_force, Esp32SensState),
        VMSTATE_UINT32(sar_mem_wr_ctrl, Esp32SensState),
        VMSTATE_UINT32(sar_atten1, Esp32SensState),
        VMSTATE_UINT32(sar_atten2, Esp32SensState),
        VMSTATE_UINT32_ARRAY(sar_slave_addr, Esp32SensState, 4),
        VMSTATE_UINT32(sar_tctrl, Esp32SensState),
        VMSTATE_UINT32(sar_i2c_ctrl, Esp32SensState),
        VMSTATE_UINT32(sar_meas_start1, Esp32SensState),
        VMSTATE_UINT32(sar_touch_ctrl1, Esp32SensState),
        VMSTATE_UINT32_ARRAY(touch_thresh, Esp32SensState, 5),
        VMSTATE_UINT32_ARRAY(touch_meas, Esp32SensState, 5),
        VMSTATE_UINT32(sar_touch_ctrl2, Esp32SensState),
        VMSTATE_UINT32(sar_touch_enable, Esp32SensState),
        VMSTATE_UINT32(sar_read_ctrl2, Esp32SensState),
        VMSTATE_UINT32(sar_meas_start2, Esp32SensState),
        VMSTATE_UINT32(sar_dac_ctrl1, Esp32SensState),
        VMSTATE_UINT32(sar_dac_ctrl2, Esp32SensState),
        VMSTATE_UINT16_ARRAY(adc1_raw_sample, Esp32SensState,
                             ESP32_SENS_ADC1_CHANNELS),
        VMSTATE_END_OF_LIST()
    }
};

static void esp32_sens_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32_sens_reset_hold;
    dc->vmsd = &vmstate_esp32_sens;
}

static const TypeInfo esp32_sens_info = {
    .name = TYPE_ESP32_SENS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32SensState),
    .instance_init = esp32_sens_init,
    .class_init = esp32_sens_class_init,
};

static void esp32_sens_register_types(void)
{
    type_register_static(&esp32_sens_info);
}

type_init(esp32_sens_register_types)
