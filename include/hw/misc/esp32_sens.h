/*
 * ESP32 SENS (SAR ADC / Touch Sensor / DAC) peripheral model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ESP32_SENS "esp32.sens"
OBJECT_DECLARE_SIMPLE_TYPE(Esp32SensState, ESP32_SENS)

#define ESP32_SENS_ADC1_CHANNELS 8
#define ESP32_SENS_MMIO_SIZE     0x400

struct Esp32SensState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* Configuration and control registers */
    uint32_t sar_read_ctrl;
    uint32_t sar_read_status1;
    uint32_t sar_meas_wait1;
    uint32_t sar_meas_wait2;
    uint32_t sar_meas_ctrl;
    uint32_t sar_read_status2;
    uint32_t ulp_cp_sleep_cyc[5];
    uint32_t sar_start_force;
    uint32_t sar_mem_wr_ctrl;
    uint32_t sar_atten1;
    uint32_t sar_atten2;
    uint32_t sar_slave_addr[4];
    uint32_t sar_tctrl;
    uint32_t sar_i2c_ctrl;

    /* ADC1 measurement / status */
    uint32_t sar_meas_start1;

    /* Touch & ADC2 registers */
    uint32_t sar_touch_ctrl1;
    uint32_t touch_thresh[5];
    uint32_t touch_meas[5];
    uint32_t sar_touch_ctrl2;
    uint32_t sar_touch_enable;
    uint32_t sar_read_ctrl2;
    uint32_t sar_meas_start2;
    uint32_t sar_dac_ctrl1;
    uint32_t sar_dac_ctrl2;

    /* Configurable raw test samples for ADC1 channels 0-7 */
    uint16_t adc1_raw_sample[ESP32_SENS_ADC1_CHANNELS];
};
