/*
 * Espressif ESP32 native Ethernet MAC (EMAC) and RTL8201F PHY model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/net/esp32_emac.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/dma.h"
#include <zlib.h>

enum {
    DMA_BUS_MODE = 0x0000,
    DMA_TX_POLL = 0x0004,
    DMA_RX_POLL = 0x0008,
    DMA_RX_BASE = 0x000c,
    DMA_TX_BASE = 0x0010,
    DMA_STATUS = 0x0014,
    DMA_OPERATION = 0x0018,
    DMA_INT_ENABLE = 0x001c,
    DMA_MISSED = 0x0020,
    DMA_RX_WATCHDOG = 0x0024,
    DMA_TX_CURRENT = 0x0048,
    DMA_RX_CURRENT = 0x004c,
    DMA_TX_BUFFER = 0x0050,
    DMA_RX_BUFFER = 0x0054,
    EXT_CLOCK_OUT = 0x0800,
    EXT_OSC_CLOCK = 0x0804,
    EXT_CLOCK_CTRL = 0x0808,
    EXT_PHY_CONFIG = 0x080c,
    EXT_POWER = 0x0810,
    MAC_CONFIG = 0x1000,
    MAC_FILTER = 0x1004,
    MAC_MII_ADDR = 0x1010,
    MAC_MII_DATA = 0x1014,
    MAC_FLOW = 0x1018,
    MAC_DEBUG = 0x1024,
    MAC_INT_STATUS = 0x1038,
    MAC_INT_MASK = 0x103c,
    MAC_ADDR_HIGH = 0x1040,
    MAC_ADDR_LOW = 0x1044,
};

/* DMA Status & Interrupt Enable bit definitions */
#define DMA_STATUS_TI   BIT(0)
#define DMA_STATUS_TPS  BIT(1)
#define DMA_STATUS_TU   BIT(2)
#define DMA_STATUS_TJT  BIT(3)
#define DMA_STATUS_OVF  BIT(4)
#define DMA_STATUS_UNF  BIT(5)
#define DMA_STATUS_RI   BIT(6)
#define DMA_STATUS_RU   BIT(7)
#define DMA_STATUS_RPS  BIT(8)
#define DMA_STATUS_RWT  BIT(9)
#define DMA_STATUS_ETI  BIT(10)
#define DMA_STATUS_FBI  BIT(13)
#define DMA_STATUS_ERI  BIT(14)
#define DMA_STATUS_AIS  BIT(15)
#define DMA_STATUS_NIS  BIT(16)

#define DMA_STATUS_NIS_MASK (DMA_STATUS_TI | DMA_STATUS_TU | \
                             DMA_STATUS_RI | DMA_STATUS_ERI)
#define DMA_STATUS_AIS_MASK (DMA_STATUS_TPS | DMA_STATUS_TJT | \
                             DMA_STATUS_OVF | \
                             DMA_STATUS_UNF | DMA_STATUS_RU | DMA_STATUS_RPS | \
                             DMA_STATUS_RWT | DMA_STATUS_ETI | DMA_STATUS_FBI)

/* Transmit descriptor bit definitions */
#define TDES0_OWN        BIT(31)
#define TDES0_ES         BIT(15)
#define TDES0_IC         BIT(30)
#define TDES0_LS         BIT(29)
#define TDES0_FS         BIT(28)
#define TDES0_TER        BIT(21)
#define TDES0_TCH        BIT(20)
#define TDES1_TBS1_MASK  0x00001fff

/* Receive descriptor bit definitions */
#define RDES0_OWN        BIT(31)
#define RDES0_FL_SHIFT   16
#define RDES0_FS         BIT(9)
#define RDES0_LS         BIT(8)
#define RDES1_DIC        BIT(31)
#define RDES1_RER        BIT(15)
#define RDES1_RCH        BIT(14)
#define RDES1_RBS1_MASK  0x00001fff

struct ESP32EMACDesc {
    uint32_t des0;
    uint32_t des1;
    uint32_t des2;
    uint32_t des3;
};

static void esp32_emac_try_transmit(ESP32EMACState *state);

static void esp32_emac_update_irq(ESP32EMACState *state)
{
    uint32_t status = state->regs[DMA_STATUS / 4];
    uint32_t enable = state->regs[DMA_INT_ENABLE / 4];

    if (status & enable & DMA_STATUS_NIS_MASK) {
        state->regs[DMA_STATUS / 4] |= DMA_STATUS_NIS;
    } else {
        state->regs[DMA_STATUS / 4] &= ~DMA_STATUS_NIS;
    }

    if (status & enable & DMA_STATUS_AIS_MASK) {
        state->regs[DMA_STATUS / 4] |= DMA_STATUS_AIS;
    } else {
        state->regs[DMA_STATUS / 4] &= ~DMA_STATUS_AIS;
    }

    uint32_t pending = state->regs[DMA_STATUS / 4] & enable;
    qemu_set_irq(state->irq, !!(pending & (DMA_STATUS_NIS | DMA_STATUS_AIS)));
}

static bool esp32_emac_link_is_up(ESP32EMACState *state)
{
    NetClientState *nc = qemu_get_queue(state->nic);
    return nc && nc->peer && !nc->link_down && !nc->peer->link_down;
}


static void esp32_emac_phy_reset(ESP32EMACState *state)
{
    state->phy_bmcr = 0x3100;
    state->phy_anar = 0x01e1;
    state->phy_page = 0;
}

static uint16_t esp32_emac_phy_read(ESP32EMACState *state, unsigned reg)
{
    if (reg == 31) {
        return state->phy_page;
    }
    if (state->phy_page != 0) {
        qemu_log_mask(LOG_UNIMP,
                      "esp32-emac: unsupported RTL8201F page %u "
                      "register %u read\n",
                      state->phy_page, reg);
        return 0xffff;
    }
    switch (reg) {
    case 0: /* BMCR */
        return state->phy_bmcr;
    case 1: /* BMSR */
        if (esp32_emac_link_is_up(state)) {
            /*
             * 100Base-TX FD/HD capable, 10Base-T FD/HD capable,
             * autoneg complete, link up
             */
            return 0x782d;
        } else {
            return 0x7809;
        }
    case 2: /* PHYID1 */
        return 0x001c;
    case 3: /* PHYID2 */
        return 0xc816;
    case 4: /* ANAR */
        return state->phy_anar;
    case 5: /* ANLPAR */
        if (esp32_emac_link_is_up(state)) {
            /* Peer supports 100M FD/HD, 10M FD/HD, 802.3 selector */
            return 0x01e1;
        }
        return 0;
    case 6: /* ANER */
        return esp32_emac_link_is_up(state) ? 0x0001 : 0x0000;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "esp32-emac: unsupported RTL8201F register %u read\n",
                      reg);
        return 0xffff;
    }
}

static void esp32_emac_phy_write(ESP32EMACState *state, unsigned reg,
                                 uint16_t value)
{
    if (reg == 31) {
        state->phy_page = value & 0xff;
        return;
    }
    if (state->phy_page == 0 && reg == 0) {
        if (value & BIT(15)) {
            esp32_emac_phy_reset(state);
        } else {
            state->phy_bmcr = value & 0x7d80;
        }
        return;
    }
    if (state->phy_page == 0 && reg == 4) {
        state->phy_anar = value & 0x2fff;
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "esp32-emac: unsupported RTL8201F page %u register %u write\n",
                  state->phy_page, reg);
}

static void esp32_emac_set_link(NetClientState *nc)
{
    ESP32EMACState *state = qemu_get_nic_opaque(nc);
    if (!nc->link_down && (state->regs[DMA_OPERATION / 4] & BIT(1))) {
        qemu_flush_queued_packets(qemu_get_queue(state->nic));
    }
}

static bool esp32_emac_storage_register(hwaddr offset)
{
    switch (offset) {
    case DMA_BUS_MODE:
    case DMA_RX_BASE:
    case DMA_TX_BASE:
    case DMA_STATUS:
    case DMA_OPERATION:
    case DMA_INT_ENABLE:
    case DMA_RX_WATCHDOG:
    case EXT_CLOCK_OUT:
    case EXT_OSC_CLOCK:
    case EXT_CLOCK_CTRL:
    case EXT_PHY_CONFIG:
    case EXT_POWER:
    case MAC_CONFIG:
    case MAC_FILTER:
    case MAC_MII_ADDR:
    case MAC_MII_DATA:
    case MAC_FLOW:
    case MAC_INT_MASK:
    case MAC_ADDR_HIGH:
    case MAC_ADDR_LOW:
        return true;
    default:
        return false;
    }
}

static void esp32_emac_mac_reset(ESP32EMACState *state, bool reset_ext)
{
    uint32_t ext_regs[5];

    if (!reset_ext) {
        memcpy(ext_regs, &state->regs[EXT_CLOCK_OUT / 4], sizeof(ext_regs));
    }
    memset(state->regs, 0, sizeof(state->regs));
    if (!reset_ext) {
        memcpy(&state->regs[EXT_CLOCK_OUT / 4], ext_regs, sizeof(ext_regs));
    }
    state->regs[MAC_ADDR_HIGH / 4] = BIT(31);
    state->current_tx_desc = 0;
    state->current_rx_desc = 0;
    esp32_emac_update_irq(state);
}

static int esp32_emac_read_desc(hwaddr addr, struct ESP32EMACDesc *desc)
{
    if (dma_memory_read(&address_space_memory, addr, desc,
                        sizeof(*desc), MEMTXATTRS_UNSPECIFIED)) {
        return -1;
    }
    desc->des0 = le32_to_cpu(desc->des0);
    desc->des1 = le32_to_cpu(desc->des1);
    desc->des2 = le32_to_cpu(desc->des2);
    desc->des3 = le32_to_cpu(desc->des3);
    return 0;
}

static int esp32_emac_write_desc(hwaddr addr, const struct ESP32EMACDesc *desc)
{
    struct ESP32EMACDesc le_desc;
    le_desc.des0 = cpu_to_le32(desc->des0);
    le_desc.des1 = cpu_to_le32(desc->des1);
    le_desc.des2 = cpu_to_le32(desc->des2);
    le_desc.des3 = cpu_to_le32(desc->des3);
    if (dma_memory_write(&address_space_memory, addr, &le_desc,
                         sizeof(le_desc), MEMTXATTRS_UNSPECIFIED)) {
        return -1;
    }
    return 0;
}

static void esp32_emac_try_transmit(ESP32EMACState *state)
{
    if (!(state->regs[DMA_OPERATION / 4] & BIT(13))) {
        return;
    }

    if (state->current_tx_desc == 0) {
        state->current_tx_desc = state->regs[DMA_TX_BASE / 4] & ~3U;
    }
    if (state->current_tx_desc == 0) {
        return;
    }

    uint8_t tx_buf[2048];
    uint32_t pkt_len = 0;
    bool tx_error = false;
    int loop_guard = 256;

    while (loop_guard-- > 0) {
        hwaddr desc_addr = state->current_tx_desc;
        state->regs[DMA_TX_CURRENT / 4] = desc_addr;

        struct ESP32EMACDesc desc;
        if (esp32_emac_read_desc(desc_addr, &desc) != 0) {
            state->regs[DMA_STATUS / 4] |= DMA_STATUS_FBI;
            esp32_emac_update_irq(state);
            break;
        }

        if (!(desc.des0 & TDES0_OWN)) {
            state->regs[DMA_STATUS / 4] |= DMA_STATUS_TU;
            esp32_emac_update_irq(state);
            break;
        }

        uint32_t buf1_size = desc.des1 & TDES1_TBS1_MASK;
        hwaddr buf1_addr = desc.des2;
        state->regs[DMA_TX_BUFFER / 4] = buf1_addr;

        if (desc.des0 & TDES0_FS) {
            pkt_len = 0;
            tx_error = false;
        }

        if (buf1_size > 0 && !tx_error) {
            if (pkt_len + buf1_size > sizeof(tx_buf)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "esp32-emac: oversized TX frame\n");
                tx_error = true;
            } else if (dma_memory_read(&address_space_memory, buf1_addr,
                                       tx_buf + pkt_len, buf1_size,
                                       MEMTXATTRS_UNSPECIFIED) == 0) {
                pkt_len += buf1_size;
            } else {
                state->regs[DMA_STATUS / 4] |= DMA_STATUS_FBI;
                esp32_emac_update_irq(state);
                tx_error = true;
            }
        }

        if (desc.des0 & TDES0_LS) {
            if (!tx_error && pkt_len > 0) {
                qemu_send_packet(qemu_get_queue(state->nic), tx_buf, pkt_len);
            } else if (tx_error) {
                desc.des0 |= TDES0_ES;
            }
            pkt_len = 0;
            tx_error = false;
        }

        desc.des0 &= ~TDES0_OWN;
        if (esp32_emac_write_desc(desc_addr, &desc) != 0) {
            state->regs[DMA_STATUS / 4] |= DMA_STATUS_FBI;
            esp32_emac_update_irq(state);
            break;
        }

        if (desc.des0 & TDES0_IC) {
            state->regs[DMA_STATUS / 4] |= DMA_STATUS_TI;
            esp32_emac_update_irq(state);
        }

        if (desc.des0 & TDES0_TER) {
            state->current_tx_desc = state->regs[DMA_TX_BASE / 4] & ~3U;
        } else if (desc.des0 & TDES0_TCH) {
            state->current_tx_desc = desc.des3 & ~3U;
        } else {
            unsigned stride =
                (state->regs[DMA_BUS_MODE / 4] & BIT(7)) ? 32 : 16;
            stride += ((state->regs[DMA_BUS_MODE / 4] >> 2) & 31) * 4;
            state->current_tx_desc = (desc_addr + stride) & ~3U;
        }
    }
}

static uint64_t esp32_emac_read(void *opaque, hwaddr offset, unsigned size)
{
    ESP32EMACState *state = opaque;
    if (esp32_emac_storage_register(offset)) {
        return state->regs[offset / 4];
    }
    switch (offset) {
    case DMA_TX_POLL:
    case DMA_RX_POLL:
    case DMA_MISSED:
    case MAC_DEBUG:
    case MAC_INT_STATUS:
        return 0;
    case DMA_TX_CURRENT:
        return state->regs[DMA_TX_CURRENT / 4];
    case DMA_RX_CURRENT:
        return state->regs[DMA_RX_CURRENT / 4];
    case DMA_TX_BUFFER:
        return state->regs[DMA_TX_BUFFER / 4];
    case DMA_RX_BUFFER:
        return state->regs[DMA_RX_BUFFER / 4];
    default:
        qemu_log_mask(LOG_UNIMP,
                      "esp32-emac: unsupported MMIO read at 0x%" HWADDR_PRIx
                      "\n", offset);
        return 0;
    }
}

static void esp32_emac_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    ESP32EMACState *state = opaque;
    uint32_t data = value;

    if (offset == DMA_TX_POLL) {
        esp32_emac_try_transmit(state);
        return;
    }
    if (offset == DMA_RX_POLL) {
        if (state->regs[DMA_OPERATION / 4] & BIT(1)) {
            qemu_flush_queued_packets(qemu_get_queue(state->nic));
        }
        return;
    }
    if (!esp32_emac_storage_register(offset)) {
        qemu_log_mask(LOG_UNIMP,
                      "esp32-emac: unsupported MMIO write at 0x%" HWADDR_PRIx
                      "\n", offset);
        return;
    }
    switch (offset) {
    case DMA_BUS_MODE:
        if (data & BIT(0)) {
            esp32_emac_mac_reset(state, false);
            return;
        }
        break;
    case DMA_STATUS:
        state->regs[offset / 4] &= ~(data & 0x1e7ff);
        if (data & DMA_STATUS_RU) {
            if (state->regs[DMA_OPERATION / 4] & BIT(1)) {
                qemu_flush_queued_packets(qemu_get_queue(state->nic));
            }
        }
        esp32_emac_update_irq(state);
        return;
    case DMA_RX_BASE:
        data &= ~3U;
        state->current_rx_desc = data;
        break;
    case DMA_TX_BASE:
        data &= ~3U;
        state->current_tx_desc = data;
        break;
    case DMA_OPERATION:
        data &= ~BIT(20);
        state->regs[offset / 4] = data;
        if (data & BIT(13)) {
            esp32_emac_try_transmit(state);
        }
        if (data & BIT(1)) {
            qemu_flush_queued_packets(qemu_get_queue(state->nic));
        }
        return;
    case MAC_MII_ADDR:
        if (data & BIT(0)) {
            unsigned phy = (data >> 11) & 31;
            unsigned reg = (data >> 6) & 31;
            if (phy == state->phy_addr) {
                if (data & BIT(1)) {
                    esp32_emac_phy_write(state, reg,
                                          state->regs[MAC_MII_DATA / 4]);
                } else {
                    state->regs[MAC_MII_DATA / 4] =
                        esp32_emac_phy_read(state, reg);
                }
            } else if (!(data & BIT(1))) {
                state->regs[MAC_MII_DATA / 4] = 0xffff;
            }
        }
        data &= 0xfffe;
        break;
    case MAC_MII_DATA:
        data &= 0xffff;
        break;
    case MAC_ADDR_HIGH:
        data = (data & 0xffff) | BIT(31);
        break;
    case MAC_FLOW:
        data &= ~BIT(0);
        break;
    default:
        break;
    }
    state->regs[offset / 4] = data;
    esp32_emac_update_irq(state);
}

static bool esp32_emac_can_receive(NetClientState *client)
{
    ESP32EMACState *state = qemu_get_nic_opaque(client);
    if (!esp32_emac_link_is_up(state)) {
        return false;
    }
    if (!(state->regs[DMA_OPERATION / 4] & BIT(1))) {
        return false;
    }
    hwaddr desc_addr = state->current_rx_desc;
    if (desc_addr == 0) {
        desc_addr = state->regs[DMA_RX_BASE / 4] & ~3U;
    }
    if (desc_addr == 0) {
        return false;
    }
    struct ESP32EMACDesc desc;
    if (esp32_emac_read_desc(desc_addr, &desc) != 0) {
        return false;
    }
    return !!(desc.des0 & RDES0_OWN);
}

static hwaddr esp32_emac_next_rx_desc(ESP32EMACState *state, hwaddr address,
                                     const struct ESP32EMACDesc *desc)
{
    if (desc->des1 & RDES1_RER) {
        return state->regs[DMA_RX_BASE / 4] & ~3U;
    }
    if (desc->des1 & RDES1_RCH) {
        return desc->des3 & ~3U;
    }
    unsigned stride = (state->regs[DMA_BUS_MODE / 4] & BIT(7)) ? 32 : 16;
    stride += ((state->regs[DMA_BUS_MODE / 4] >> 2) & 31) * 4;
    return (address + stride) & ~3U;
}

static ssize_t esp32_emac_receive(NetClientState *client,
                                  const uint8_t *buffer, size_t size)
{
    ESP32EMACState *state = qemu_get_nic_opaque(client);
    if (!esp32_emac_can_receive(client)) {
        return 0;
    }

    if (state->current_rx_desc == 0) {
        state->current_rx_desc = state->regs[DMA_RX_BASE / 4] & ~3U;
    }
    if (size > 0x3fff - 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "esp32-emac: oversized RX frame\n");
        return size;
    }
    size_t frame_size = MAX(size, 60);
    struct ESP32EMACDesc descriptors[256];
    hwaddr addresses[256];
    size_t lengths[256];
    size_t remaining = frame_size + 4;
    unsigned count = 0;
    hwaddr next = state->current_rx_desc;
    while (remaining && count < ARRAY_SIZE(descriptors)) {
        for (unsigned previous = 0; previous < count; previous++) {
            if (addresses[previous] == next) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "esp32-emac: RX ring too small for frame\n");
                return size;
            }
        }
        struct ESP32EMACDesc *desc = &descriptors[count];
        if (!next || esp32_emac_read_desc(next, desc) != 0) {
            state->regs[DMA_STATUS / 4] |= DMA_STATUS_FBI;
            esp32_emac_update_irq(state);
            return size;
        }
        if (!(desc->des0 & RDES0_OWN)) {
            state->regs[DMA_STATUS / 4] |= DMA_STATUS_RU;
            esp32_emac_update_irq(state);
            return 0;
        }
        size_t capacity = desc->des1 & RDES1_RBS1_MASK;
        if (!capacity) {
            state->regs[DMA_STATUS / 4] |= DMA_STATUS_FBI;
            esp32_emac_update_irq(state);
            return size;
        }
        addresses[count] = next;
        lengths[count] = MIN(remaining, capacity);
        remaining -= lengths[count];
        next = esp32_emac_next_rx_desc(state, next, desc);
        count++;
    }
    if (remaining) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32-emac: RX descriptor limit exceeded\n");
        return size;
    }
    g_autofree uint8_t *wire_buffer = g_malloc0(frame_size + 4);
    memcpy(wire_buffer, buffer, size);
    stl_le_p(wire_buffer + frame_size, crc32(0, wire_buffer, frame_size));
    size_t copied = 0;
    bool interrupt = false;
    for (unsigned index = 0; index < count; index++) {
        struct ESP32EMACDesc *desc = &descriptors[index];
        state->regs[DMA_RX_CURRENT / 4] = addresses[index];
        state->regs[DMA_RX_BUFFER / 4] = desc->des2;
        if (dma_memory_write(&address_space_memory, desc->des2,
                             wire_buffer + copied, lengths[index],
                             MEMTXATTRS_UNSPECIFIED) != 0) {
            state->regs[DMA_STATUS / 4] |= DMA_STATUS_FBI;
            esp32_emac_update_irq(state);
            return size;
        }
        copied += lengths[index];
        desc->des0 = index == 0 ? RDES0_FS : 0;
        if (index + 1 == count) {
            desc->des0 |= RDES0_LS | ((frame_size + 4) << RDES0_FL_SHIFT);
        }
        if (esp32_emac_write_desc(addresses[index], desc) != 0) {
            state->regs[DMA_STATUS / 4] |= DMA_STATUS_FBI;
            esp32_emac_update_irq(state);
            return size;
        }
        interrupt |= !(desc->des1 & RDES1_DIC);
    }
    state->current_rx_desc = next;
    if (interrupt) {
        state->regs[DMA_STATUS / 4] |= DMA_STATUS_RI;
        esp32_emac_update_irq(state);
    }

    return size;
}

static NetClientInfo esp32_emac_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = esp32_emac_can_receive,
    .receive = esp32_emac_receive,
    .link_status_changed = esp32_emac_set_link,
};

static const MemoryRegionOps esp32_emac_ops = {
    .read = esp32_emac_read,
    .write = esp32_emac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void esp32_emac_reset_hold(Object *obj, ResetType type)
{
    ESP32EMACState *state = ESP32_EMAC(obj);
    esp32_emac_mac_reset(state, true);
    esp32_emac_phy_reset(state);
}

static void esp32_emac_realize(DeviceState *dev, Error **errp)
{
    ESP32EMACState *state = ESP32_EMAC(dev);
    if (state->phy_addr > 31) {
        error_setg(errp, "esp32-emac: phy-address %u must be <= 31",
                   state->phy_addr);
        return;
    }
    qemu_macaddr_default_if_unset(&state->conf.macaddr);
    state->nic = qemu_new_nic(&esp32_emac_net_info, &state->conf,
                              TYPE_ESP32_EMAC, dev->id,
                              &dev->mem_reentrancy_guard, state);
    qemu_format_nic_info_str(qemu_get_queue(state->nic), state->conf.macaddr.a);
}

static void esp32_emac_unrealize(DeviceState *dev)
{
    ESP32EMACState *state = ESP32_EMAC(dev);
    qemu_del_nic(state->nic);
}

static void esp32_emac_init(Object *obj)
{
    ESP32EMACState *state = ESP32_EMAC(obj);
    memory_region_init_io(&state->iomem, obj, &esp32_emac_ops, state,
                          TYPE_ESP32_EMAC, ESP32_EMAC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &state->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &state->irq);
}

static const VMStateDescription esp32_emac_vmstate = {
    .name = TYPE_ESP32_EMAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, ESP32EMACState, ESP32_EMAC_MMIO_SIZE / 4),
        VMSTATE_UINT16(phy_bmcr, ESP32EMACState),
        VMSTATE_UINT16(phy_anar, ESP32EMACState),
        VMSTATE_UINT8(phy_page, ESP32EMACState),
        VMSTATE_UINT32(current_tx_desc, ESP32EMACState),
        VMSTATE_UINT32(current_rx_desc, ESP32EMACState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property esp32_emac_properties[] = {
    DEFINE_NIC_PROPERTIES(ESP32EMACState, conf),
    DEFINE_PROP_UINT8("phy-address", ESP32EMACState, phy_addr, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_emac_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *device = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    device->realize = esp32_emac_realize;
    device->unrealize = esp32_emac_unrealize;
    device->vmsd = &esp32_emac_vmstate;
    device->desc = "ESP32 native EMAC DMA controller & RTL8201F PHY";
    rc->phases.hold = esp32_emac_reset_hold;
    device_class_set_props(device, esp32_emac_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, device->categories);
}

static const TypeInfo esp32_emac_type = {
    .name = TYPE_ESP32_EMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32EMACState),
    .instance_init = esp32_emac_init,
    .class_init = esp32_emac_class_init,
};

static void esp32_emac_register_types(void)
{
    type_register_static(&esp32_emac_type);
}

type_init(esp32_emac_register_types)
