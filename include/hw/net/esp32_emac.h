/*
 * Espressif ESP32 Ethernet MAC (EMAC) model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NET_ESP32_EMAC_H
#define HW_NET_ESP32_EMAC_H

#include "hw/sysbus.h"
#include "net/net.h"

#define TYPE_ESP32_EMAC "esp32-emac"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32EMACState, ESP32_EMAC)

#define ESP32_EMAC_MMIO_SIZE 0x2000

struct ESP32EMACState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    NICConf conf;
    NICState *nic;
    uint32_t regs[ESP32_EMAC_MMIO_SIZE / 4];
    uint16_t phy_bmcr;
    uint16_t phy_anar;
    uint8_t phy_page;
    uint8_t phy_addr;
    uint32_t current_tx_desc;
    uint32_t current_rx_desc;
};

#endif

