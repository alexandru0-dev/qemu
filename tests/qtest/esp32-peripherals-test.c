/*
 * QTest testcase for ESP32 optional peripherals
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qlist.h"

#define I2C_BASE       0x3ff53000
#define I2C_CTR        0x04
#define I2C_STATUS     0x08
#define I2C_FIFO_CONF  0x18
#define I2C_FIFO_DATA  0x1c
#define I2C_INT_RAW    0x20
#define I2C_INT_CLR    0x24
#define I2C_CMD        0x58
#define I2C_ACK_CHECK  (1U << 8)
#define I2C_ACK_ERROR  (1U << 10)
#define I2C_COMPLETE   (1U << 7)
#define I2C_DONE       (1U << 31)
#define I2C_WRITE      (1U << 11)
#define I2C_READ       (2U << 11)
#define I2C_STOP       (3U << 11)
#define PCA_ADDRESS   0x2f
#define PCA_AUTOINC   0x80
#define PCA_INPUT     0x00
#define PCA_POLARITY  0x02
#define PCA_BUS_HOLD  0x04
#define PCA_PULL      0x06
#define PCA_CONFIG    0x08
#define PCA_OUTPUT    0x0a
#define PCA_MASK      0x0c
#define PCA_STATUS    0x0e

static void assert_property(QTestState *qts, const char *name, bool expected)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-get', 'arguments':"
        "{'path':'/machine', 'property':%s}}", name);

    g_assert(qdict_haskey(response, "return"));
    g_assert_cmpint(qdict_get_bool(response, "return"), ==, expected);
    qobject_unref(response);
}

static void i2c_transfer(QTestState *qts, const uint8_t *payload,
                         size_t length, const uint32_t *commands,
                         size_t command_count, bool nack)
{
    size_t index;
    uint32_t flags;

    g_assert_cmpuint(length, <=, 32);
    g_assert_cmpuint(command_count, <=, 16);
    qtest_writel(qts, I2C_BASE + I2C_FIFO_CONF, (1U << 12) | (1U << 13));
    qtest_writel(qts, I2C_BASE + I2C_INT_CLR, UINT32_MAX);
    for (index = 0; index < length; index++) {
        qtest_writel(qts, I2C_BASE + I2C_FIFO_DATA, payload[index]);
    }
    for (index = 0; index < command_count; index++) {
        qtest_writel(qts, I2C_BASE + I2C_CMD + 4 * index, commands[index]);
    }
    qtest_writel(qts, I2C_BASE + I2C_CTR, (1U << 4) | (1U << 5));
    flags = qtest_readl(qts, I2C_BASE + I2C_INT_RAW);
    g_assert_cmphex(flags & I2C_ACK_ERROR, ==, nack ? I2C_ACK_ERROR : 0);
    g_assert_cmphex(flags & I2C_COMPLETE, ==, nack ? 0 : I2C_COMPLETE);
    if (!nack) {
        for (index = 0; index < command_count; index++) {
            g_assert_cmphex(qtest_readl(qts, I2C_BASE + I2C_CMD + 4 * index)
                            & I2C_DONE, ==, I2C_DONE);
        }
    }
    g_assert_cmphex(qtest_readl(qts, I2C_BASE + I2C_STATUS) &
                    ((1U << 4) | (0x3fU << 18)), ==, 0);
}

static void pca_write_at(QTestState *qts, uint8_t address, uint8_t reg,
                         uint8_t first, uint8_t second)
{
    const uint8_t payload[] = { address << 1, reg, first, second };
    const uint32_t commands[] = { 0, I2C_WRITE | I2C_ACK_CHECK | 4, I2C_STOP };

    i2c_transfer(qts, payload, sizeof(payload), commands,
                 G_N_ELEMENTS(commands), false);
}

static void pca_assert_pair_at(QTestState *qts, uint8_t address, uint8_t reg,
                               uint8_t first, uint8_t second)
{
    const uint8_t payload[] = {
        address << 1, reg, (address << 1) | 1,
    };
    const uint32_t commands[] = {
        0, I2C_WRITE | I2C_ACK_CHECK | 2,
        0, I2C_WRITE | I2C_ACK_CHECK | 1,
        I2C_READ | 2, I2C_STOP,
    };

    i2c_transfer(qts, payload, sizeof(payload), commands,
                 G_N_ELEMENTS(commands), false);
    g_assert_cmpuint((qtest_readl(qts, I2C_BASE + I2C_STATUS) >> 8) & 0x3f,
                     ==, 2);
    g_assert_cmphex(qtest_readl(qts, I2C_BASE + I2C_FIFO_DATA), ==, first);
    g_assert_cmphex(qtest_readl(qts, I2C_BASE + I2C_FIFO_DATA), ==, second);
    g_assert_cmpuint((qtest_readl(qts, I2C_BASE + I2C_STATUS) >> 8) & 0x3f,
                     ==, 0);
}

static void pca_write(QTestState *qts, uint8_t reg,
                      uint8_t first, uint8_t second)
{
    pca_write_at(qts, PCA_ADDRESS, reg, first, second);
}

static void pca_assert_pair(QTestState *qts, uint8_t reg,
                            uint8_t first, uint8_t second)
{
    pca_assert_pair_at(qts, PCA_ADDRESS, reg, first, second);
}

static void test_defaults(void)
{
    QTestState *qts = qtest_init("-machine esp32 -nic none");
    const uint8_t payload[] = { PCA_ADDRESS << 1 };
    const uint32_t commands[] = { 0, I2C_WRITE | I2C_ACK_CHECK | 1, I2C_STOP };

    assert_property(qts, "pca9575", false);
    i2c_transfer(qts, payload, sizeof(payload), commands,
                 G_N_ELEMENTS(commands), true);
    qtest_quit(qts);
}

static void pca_assert_reset(QTestState *qts)
{
    pca_assert_pair(qts, PCA_AUTOINC | PCA_POLARITY, 0, 0);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_BUS_HOLD, 0, 0);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_PULL, 0xff, 0xff);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_CONFIG, 0xff, 0xff);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_OUTPUT, 0, 0);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_MASK, 0xff, 0xff);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_STATUS, 0, 0);
}

static char *find_pca9575_path(QTestState *qts)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-list', 'arguments':{'path':'/machine/unattached'}}");
    QList *entries = qdict_get_qlist(response, "return");
    const QListEntry *entry;
    char *path = NULL;

    g_assert_nonnull(entries);
    QLIST_FOREACH_ENTRY(entries, entry) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_strcmp0(qdict_get_try_str(item, "type"),
                      "child<pca9575>") == 0) {
            path = g_strdup_printf("/machine/unattached/%s",
                                   qdict_get_str(item, "name"));
            break;
        }
    }
    qobject_unref(response);
    g_assert_nonnull(path);
    return path;
}

static void test_pca9575(void)
{
    QTestState *qts = qtest_init("-machine esp32,pca9575=on -nic none");
    const uint8_t tmp105_payload[] = { 0x48 << 1 };
    const uint32_t probe_commands[] = {
        0, I2C_WRITE | I2C_ACK_CHECK | 1, I2C_STOP
    };
    g_autofree char *pca_path = find_pca9575_path(qts);

    assert_property(qts, "pca9575", true);
    /* Existing TMP105 temperature sensor at 0x48 must remain reachable. */
    i2c_transfer(qts, tmp105_payload, sizeof(tmp105_payload), probe_commands,
                 G_N_ELEMENTS(probe_commands), false);
    pca_assert_reset(qts);
    /* Exercise external input sampling and interrupt status latching/clear. */
    pca_write(qts, PCA_AUTOINC | PCA_MASK, 0x00, 0xf0);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set', 'arguments':"
        "{'path':%s, 'property':'input0', 'value':165}}", pca_path);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set', 'arguments':"
        "{'path':%s, 'property':'input1', 'value':60}}", pca_path);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_STATUS, 0xa5, 0x0c);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_INPUT, 0xa5, 0x3c);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_STATUS, 0x00, 0x00);
    pca_write(qts, PCA_AUTOINC | PCA_OUTPUT, 0xa5, 0x3c);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_OUTPUT, 0xa5, 0x3c);
    pca_write(qts, PCA_AUTOINC | PCA_CONFIG, 0, 0);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_INPUT, 0xa5, 0x3c);
    pca_write(qts, PCA_AUTOINC | PCA_INPUT, 0, 0);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_INPUT, 0xa5, 0x3c);
    pca_write(qts, PCA_AUTOINC | PCA_STATUS, 0xff, 0xff);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_STATUS, 0, 0);
    pca_write(qts, PCA_AUTOINC | PCA_POLARITY, 0xff, 0x0f);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_INPUT, 0x5a, 0x33);
    pca_write(qts, PCA_OUTPUT, 0x12, 0x34);
    pca_assert_pair(qts, PCA_OUTPUT, 0x34, 0x34);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_OUTPUT, 0x34, 0x3c);
    pca_write(qts, PCA_AUTOINC | PCA_BUS_HOLD, 0xff, 0xa5);
    pca_assert_pair(qts, PCA_AUTOINC | PCA_BUS_HOLD, 3, 1);
    pca_write(qts, PCA_AUTOINC | PCA_PULL, 0x12, 0x34);
    pca_write(qts, PCA_AUTOINC | PCA_MASK, 0x56, 0x78);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_qmp_eventwait(qts, "RESET");
    pca_assert_reset(qts);
    qtest_quit(qts);
}

static void test_pca9575_address(void)
{
    QTestState *qts = qtest_init(
        "-machine esp32,pca9575=on,pca9575-address=0x2e -nic none");
    const uint8_t payload[] = { PCA_ADDRESS << 1 };
    const uint32_t commands[] = { 0, I2C_WRITE | I2C_ACK_CHECK | 1, I2C_STOP };
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-get', 'arguments':"
        "{'path':'/machine', 'property':'pca9575-address'}}");

    g_assert(qdict_haskey(response, "return"));
    g_assert_cmpuint(qdict_get_int(response, "return"), ==, 0x2e);
    qobject_unref(response);
    i2c_transfer(qts, payload, sizeof(payload), commands,
                 G_N_ELEMENTS(commands), true);
    pca_assert_pair_at(qts, 0x2e, PCA_AUTOINC | PCA_CONFIG, 0xff, 0xff);
    pca_write_at(qts, 0x2e, PCA_AUTOINC | PCA_OUTPUT, 0x5a, 0xc3);
    pca_assert_pair_at(qts, 0x2e, PCA_AUTOINC | PCA_OUTPUT, 0x5a, 0xc3);
    response = qtest_qmp(qts,
        "{'execute':'qom-set', 'arguments':{'path':'/machine',"
        "'property':'pca9575-address', 'value':72}}");
    g_assert(qdict_haskey(response, "error"));
    qobject_unref(response);
    response = qtest_qmp(qts,
        "{'execute':'qom-set', 'arguments':{'path':'/machine',"
        "'property':'pca9575-address', 'value':128}}");
    g_assert(qdict_haskey(response, "error"));
    qobject_unref(response);
    response = qtest_qmp(qts,
        "{'execute':'qom-set', 'arguments':{'path':'/machine',"
        "'property':'pca9575-address', 'value':45}}");
    g_assert(qdict_haskey(response, "error"));
    qobject_unref(response);
    pca_assert_pair_at(qts, 0x2e, PCA_AUTOINC | PCA_OUTPUT, 0x5a, 0xc3);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (qtest_has_machine("esp32")) {
        qtest_add_func("/esp32/peripherals/defaults", test_defaults);
        qtest_add_func("/esp32/peripherals/pca9575", test_pca9575);
        qtest_add_func("/esp32/peripherals/pca9575-address",
                       test_pca9575_address);
    }
    return g_test_run();
}
