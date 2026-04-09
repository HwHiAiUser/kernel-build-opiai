/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2019-2023. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * Description:
 * Author: huawei
 * Create: 2019-10-15
 */

#include "can_drv_core.h"


#ifdef RUN_IN_AOS
#include <linux/time64.h>
#include <linux/platform_device.h>
#include "aos_dlib.h"
#else
#include <linux/module.h>
#include <linux/platform_device.h>
#endif
#include <linux/clk.h>
#include <linux/timer.h>
#include <linux/of_device.h>
#include <linux/securec.h>
#include <linux/of_gpio.h>
#include <linux/mutex.h>
#include <linux/pinctrl/consumer.h>
#include <linux/sockios.h>

#ifdef SUPPORT_CONFIG_KTIME_TYPE
#include <linux/virt_wall_time.h>
#endif

#include "drv_snapshot.h"
#include "can_drv_interrupt.h"
#include "can_drv_default_cfg.h"
#include "can_drv_fault_mgr.h"
#include "can_drv_log.h"
#include "can_drv_api.h"
#include "bind_core.h"
#include "can_drv_dms_init.h"
#include "can_drv_feature.h"

#ifdef STATIC_SKIP
    #define STATIC
#else
    #define STATIC static
#endif

#ifdef RUN_IN_AOS
#define KBUILD_MODNAME "foo"
#endif

#define MTTCAN_BOOTDOT_INIT(init_state, expect_state) \
drv_snapshot_bootdot_init(CAN_MODULE_ID, init_state, expect_state)

#define MTTCAN_BOOTDOT(curr_state) drv_snapshot_bootdot_set(CAN_MODULE_ID, curr_state)

#define VALID_CAN_NUN   13
#define MAX_CPU_CORE    16
#define CAN_IRQ_NODE_NAME_MAXLEN    10
#define WAIT_FOR_TXB_FREE_US      200
#define MTTCAN_TX_BUSY_DELAY_USECOND 1000
#define invalid_cpu(id)  ((id) < MAX_CPU_CORE)

#define CAN_BUSLOAD_WARNING_THRESHOLD   70    /* 70% */
#define CAN_BUSLOAD_CHECHKING_INTERVAL  1000  /* ms */
#define CAN_ECHO_STSFSLOG_INTERVAL      (1000 * 60 * 5)  /* 5min */
#define CAN_BUSLOAD_IDLE_TIMES_TO_CLEAR 3

#define DEFAULT_BUSOFF_QUICK       50
#define DEFAULT_BUSOFF_SLOW        1000
#define BUSOFF_QUICK_TIMES         10
#define BUSOFF_REPORT_THRESHOLD    1
#define CONTROLLER_REG_LEN         0x200
#define MRAM_REG_LEN               0x1000

#define KTIME_TYPE_REAL    0x0
#define KTIME_TYPE_VIRTUAL 0x1
#define CAN_SIODEVPRIVATE_KTIME_TYPE SIOCDEVPRIVATE

struct net_device *g_can_net_devices[MAX_CAN_NUN] = {0};
static unsigned int valid_can_ids[VALID_CAN_NUN] = {
    0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 14};

static inline u64 bit_count(u64 val)
{
#define SHIFT_3_BIT  3
    return ((val) << SHIFT_3_BIT);
}

struct net_device* get_net_devices(u32 can_id)
{
    int i;
    for (i = 0; i < VALID_CAN_NUN; i++) {
        if (can_id == valid_can_ids[i]) {
            return g_can_net_devices[can_id];
        }
    }

    return NULL;
}

STATIC int mttcan_get_id_and_name(struct mttcan_config *cfg, const struct device_node *dev_node)
{
    int err;

    err = of_property_read_u32(dev_node, "mttcan-id", &cfg->mttcan_id);
    if (err < 0) {
        mttcan_err("failed to get mttcan-id.\n");
        return err;
    }

    err = snprintf_s(cfg->mttcan_name, sizeof(cfg->mttcan_name), sizeof(cfg->mttcan_name) - 1, "mttcan-%02u",
        cfg->mttcan_id);
    if (err < 0) {
        mttcan_err("failed to snprintf_s mttcan_name.\n");
        return err;
    }

    err = snprintf_s(cfg->can_name, sizeof(cfg->can_name), sizeof(cfg->can_name) - 1, "can%u", cfg->mttcan_id);
    if (err < 0) {
        mttcan_err("failed to snprintf_s can_name.\n");
        return err;
    }

    return 0;
}

STATIC int mttcan_get_irq(struct mttcan_config *cfg, struct platform_device *pdev)
{
    cfg->irq0 = platform_get_irq_byname(pdev, "int0");
    if (cfg->irq0 <= 0) {
        mttcan_err("failed to get int0. irq0 = %d.\n", cfg->irq0);
        return -1;
    }

    cfg->irq1 = platform_get_irq_byname(pdev, "int1");
    if (cfg->irq1 <= 0) {
        mttcan_err("failed to get int1. irq1 = %d.\n", cfg->irq1);
        return -1;
    }

    return 0;
}

STATIC int mttcan_get_resource(struct mttcan_config *cfg, struct platform_device *pdev)
{
    cfg->reg_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "reg-base");
    if (cfg->reg_res == NULL) {
        mttcan_err("failed to get reg-base.\n");
        return -1;
    }

    cfg->mram_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mram-base");
    if (cfg->mram_res == NULL) {
        mttcan_err("failed to get mram-base.\n");
        return -1;
    }

    cfg->reset_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "reset-base");
    if (cfg->reset_res == NULL) {
        mttcan_err("failed to get reset-base.\n");
        return -1;
    }

    cfg->gpio_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "gpio-base");
    if (cfg->gpio_res == NULL) {
        mttcan_err("failed to get gpio-base.\n");
        return -1;
    }

    cfg->plat_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "plat-base");
    if (cfg->plat_res == NULL) {
        mttcan_err("failed to get plat-base.\n");
        return -1;
    }

    return 0;
}

STATIC void mttcan_dump_dts_info(const struct mttcan_config *cfg)
{
    mttcan_debug("dump dts info:\n");
    mttcan_debug("mttcan-id = %u.\n", cfg->mttcan_id);
    mttcan_debug("mttcan_name = %s.\n", cfg->mttcan_name);
    mttcan_debug("can_name = %s.\n", cfg->can_name);
    mttcan_debug("cclk_freq_hz = %u.\n", cfg->cclk_freq_hz);
    mttcan_debug("hclk_freq_hz = %u.\n", cfg->hclk_freq_hz);
}

STATIC int mttcan_get_dts_info(struct mttcan_config *cfg, struct platform_device *pdev)
{
    int err;
    struct device_node *dev_node = NULL;

    dev_node = pdev->dev.of_node;
    if (dev_node == NULL) {
        mttcan_err("no device node is found.\n");
        return -1;
    }

    err = of_property_read_u32(dev_node, "cclk-freq-hz", &cfg->cclk_freq_hz);
    if (err < 0) {
        mttcan_err("failed to get cclk-freq-hz.\n");
        return err;
    }

    err = of_property_read_u32(dev_node, "hclk-freq-hz", &cfg->hclk_freq_hz);
    if (err < 0) {
        mttcan_err("failed to get hclk-freq-hz.\n");
        return err;
    }

    err = mttcan_get_id_and_name(cfg, dev_node);
    if (err < 0) {
        mttcan_err("failed to get id and name. err = %d.\n", err);
        return err;
    }

    err = mttcan_get_irq(cfg, pdev);
    if (err < 0) {
        mttcan_err("failed to get irq. err = %d.\n", err);
        return err;
    }

    err = mttcan_get_resource(cfg, pdev);
    if (err < 0) {
        mttcan_err("failed to get resource. err = %d.\n", err);
        return err;
    }

    mttcan_dump_dts_info(cfg);

    return 0;
}


bool mttcan_is_data_size_valid(long dszie)
{
    switch (dszie) {
    case DATA_SZIE_8:
    case DATA_SZIE_12:
    case DATA_SZIE_16:
    case DATA_SZIE_20:
    case DATA_SZIE_24:
    case DATA_SZIE_32:
    case DATA_SZIE_48:
    case DATA_SZIE_64:
        return true;
    default:
        return false;
    }
}

STATIC struct net_device *mttcan_alloc_can_device(struct platform_device *pdev, struct mttcan_config *cfg)
{
    struct net_device *ndev = NULL;
    struct mttcan_priv *priv = NULL;
    int ret;

    ndev = alloc_candev(sizeof(struct mttcan_priv), cfg->echo_skb_max);
    if (ndev == NULL) {
        mttcan_err("alloc_candev failed!\n");
        return NULL;
    }

    priv = netdev_priv(ndev);
    priv->ndev = ndev;
    priv->dev = &pdev->dev;
    spin_lock_init(&priv->tx_lock);
    raw_spin_lock_init(&priv->tx_complete);

#ifdef SUPPORT_MTTCAN_DFX
    if (memset_s(priv->audit_log_count,
        sizeof(priv->audit_log_count), 0, sizeof(priv->audit_log_count))) {
        mttcan_warn("init audit log count but not cleared.\n");
    }

    if (memset_s(priv->audit_log_last_time, sizeof(priv->audit_log_last_time),
        0, sizeof(priv->audit_log_last_time))) {
        mttcan_warn("init audit log time record but not cleared.\n");
    }
    if (memset_s(priv->audit_log_last_msg, sizeof(priv->audit_log_last_msg),
        0, sizeof(priv->audit_log_last_msg)) != EOK) {
        mttcan_warn("init audit log message but not cleared.\n");
    }
#endif

    /* alloc name for net_device */
    ret = dev_alloc_name(ndev, (const char *)cfg->can_name);
    if (ret < 0) {
        mttcan_err("dev_alloc_name get can name failed! ret = %d\n", ret);
        free_candev(ndev);
        return NULL;
    }

    INIT_LIST_HEAD(&priv->fault_event_list);
    INIT_WORK(&priv->fault_report_work, mttcan_fault_event_handler);

    mttcan_info("alloc %s device succeed.\n", ndev->name);
    return ndev;
}

STATIC void mttcan_free_can_device(struct net_device *ndev)
{
    struct mttcan_priv *priv = netdev_priv(ndev);

    tasklet_kill(&priv->tx_tasklet);
    kfifo_free(&priv->tx_skb_kfifo);
    tasklet_kill(&priv->rx_tasklet);
    kfifo_free(&priv->rx_skb_kfifo);
    free_candev(ndev);
}

int mttcan_set_mram_parameter(struct mttcan_priv *priv, const struct mttcan_config *cfg)
{
    /* element size */
    priv->elmt_size.rxf0 = cfg->dsize[DSIZE_RXF0] + ELMT_HEADER_SIZE_RXB;
    priv->elmt_size.rxf1 = cfg->dsize[DSIZE_RXF1] + ELMT_HEADER_SIZE_RXB;
    priv->elmt_size.rxb = cfg->dsize[DSIZE_RXB] + ELMT_HEADER_SIZE_RXB;
    priv->elmt_size.txb = cfg->dsize[DSIZE_TXB] + ELMT_HEADER_SIZE_TXB;

    /* element parameter */
    priv->elmt_param[ELMT_SIDF].off = 0;
    priv->elmt_param[ELMT_SIDF].num = cfg->element_num[ELMT_SIDF];

    priv->elmt_param[ELMT_XIDF].off =
        priv->elmt_param[ELMT_SIDF].off + priv->elmt_param[ELMT_SIDF].num * ELMT_SIZE_SIDF;
    priv->elmt_param[ELMT_XIDF].num = cfg->element_num[ELMT_XIDF];

    priv->elmt_param[ELMT_RXF0].off =
        priv->elmt_param[ELMT_XIDF].off + priv->elmt_param[ELMT_XIDF].num * ELMT_SIZE_XIDF;
    priv->elmt_param[ELMT_RXF0].num = cfg->element_num[ELMT_RXF0];

    priv->elmt_param[ELMT_RXF1].off =
        priv->elmt_param[ELMT_RXF0].off + priv->elmt_param[ELMT_RXF0].num * ELMT_SIZE_RXB_MAX;
    priv->elmt_param[ELMT_RXF1].num = cfg->element_num[ELMT_RXF1];

    priv->elmt_param[ELMT_RXB].off =
        priv->elmt_param[ELMT_RXF1].off + priv->elmt_param[ELMT_RXF1].num * ELMT_SIZE_RXB_MAX;
    priv->elmt_param[ELMT_RXB].num = cfg->element_num[ELMT_RXB];

    priv->elmt_param[ELMT_TXEF].off =
        priv->elmt_param[ELMT_RXB].off + priv->elmt_param[ELMT_RXB].num * ELMT_SIZE_RXB_MAX;
    priv->elmt_param[ELMT_TXEF].num = cfg->element_num[ELMT_TXEF];

    priv->elmt_param[ELMT_TXB].off = priv->elmt_param[ELMT_TXEF].off + priv->elmt_param[ELMT_TXEF].num * ELMT_SIZE_TXEF;
    priv->elmt_param[ELMT_TXB].num = cfg->element_num[ELMT_TXB];

    priv->elmt_param[ELMT_TMC].off =
        priv->elmt_param[ELMT_TXB].off + priv->elmt_param[ELMT_TXB].num * ELMT_SIZE_TXB_MAX;
    priv->elmt_param[ELMT_TMC].num = cfg->element_num[ELMT_TMC];

    if (priv->elmt_param[ELMT_TMC].off + priv->elmt_param[ELMT_TMC].num * ELMT_SIZE_TRIG_MEM >= BYTES_OF_MSG_RAM_MAX) {
        mttcan_err("Incorrect size configuration for message ram!\n");
        return -1;
    }

    return 0;
}

STATIC void get_platform_type(struct mttcan_priv *priv, const void __iomem *plat_base)
{
#define PLAT_TYPE_OFFSET        0x4
#define CHIP_VERSION_MASK       0xF
    u32 chip_version = readl(plat_base);
    u32 plat_reg = readl(plat_base + PLAT_TYPE_OFFSET);

    priv->chip_version = chip_version & CHIP_VERSION_MASK;
    if (plat_reg == 0x00) {
        priv->plat_type = PLAT_TYPE_ASIC;
    } else {
        priv->plat_type = (plat_reg & PLAT_TYPE_MASK) >> PLAT_TYPE_SHIFT;
    }
}

STATIC int mttcan_ioremap_resource(struct mttcan_priv *priv, const struct mttcan_config *cfg)
{
    void __iomem *reg_base = NULL;
    void __iomem *mram_base = NULL;
    void __iomem *reset_base_addr = NULL;
    void __iomem *plat_base_addr = NULL;

    reg_base = devm_ioremap(priv->dev, cfg->reg_res->start, CONTROLLER_REG_LEN);
    if (IS_ERR(reg_base)) {
        mttcan_err("No memory of reg_base is found!\n");
        return -ENOMEM;
    }

    mram_base = devm_ioremap(priv->dev, cfg->mram_res->start, MRAM_REG_LEN);
    if (IS_ERR(mram_base)) {
        mttcan_err("No memory of mram_base is found!\n");
        return -ENOMEM;
    }

    reset_base_addr = devm_ioremap(priv->dev, cfg->reset_res->start, RESET_REG_LEN);
    if (IS_ERR(reset_base_addr)) {
        mttcan_err("No memory of reset_base is found!\n");
        return -ENOMEM;
    }

    plat_base_addr = devm_ioremap(priv->dev, cfg->plat_res->start, PLATFORM_REG_LEN);
    if (IS_ERR(plat_base_addr)) {
        mttcan_err("No memory of plat_base is found!\n");
        return -ENOMEM;
    }

    /* get type FPGA or EVB */
    get_platform_type(priv, plat_base_addr);

    priv->addr.reg_base = reg_base;
    priv->addr.mram_base = mram_base;
    priv->addr.reset_base = reset_base_addr;
    priv->addr.plat_base = plat_base_addr;

    return 0;
}

STATIC void mttcan_set_restart_param(struct mttcan_priv *priv)
{
    priv->busoff_warning = false;
    priv->busoff_cfg_param.busoff_quick = DEFAULT_BUSOFF_QUICK;
    priv->busoff_cfg_param.busoff_slow = DEFAULT_BUSOFF_SLOW;
    priv->busoff_cfg_param.busoff_quick_times = BUSOFF_QUICK_TIMES;
    priv->busoff_cfg_param.busoff_report_threshold = BUSOFF_REPORT_THRESHOLD;
}

STATIC void mttcan_init_perf_record(struct mttcan_priv *priv)
{
    int i;
    priv->can_perf_record[CAN_RECV].latency_threshold = RECV_THRESHOLD;
    priv->can_perf_record[CAN_XMIT].latency_threshold = XMIT_THRESHOLD;
    for (i = 0; i < MTTCAN_ACTION_MAX; i++) {
        priv->can_perf_record[i].index = 0;
    }
}

STATIC int mttcan_alloc_kfifo(struct mttcan_priv *priv)
{
    int err;
#ifdef RUN_IN_AOS
    err = kfifo_alloc(&priv->rx_skb_kfifo, SKB_KFIFO_SIZE * sizeof(struct sk_buff*), GFP_KERNEL);
#else
    err = kfifo_alloc(&priv->rx_skb_kfifo, SKB_KFIFO_SIZE, GFP_KERNEL);
#endif
    if (err != 0) {
        mttcan_err("rx kfifo_alloc failed.\n");
        return err;
    }

#ifdef RUN_IN_AOS
    err = kfifo_alloc(&priv->tx_skb_kfifo, SKB_KFIFO_SIZE * sizeof(struct sk_buff*), GFP_KERNEL);
#else
    err = kfifo_alloc(&priv->tx_skb_kfifo, SKB_KFIFO_SIZE, GFP_KERNEL);
#endif
    if (err != 0) {
        mttcan_err("tx kfifo_alloc failed.\n");
        return err;
    }
    return 0;
}

STATIC int mttcan_set_private_data(struct net_device *ndev, struct mttcan_config *cfg, struct platform_device *pdev)
{
    int err;
    struct mttcan_priv *priv = netdev_priv(ndev);

    err = mttcan_set_mram_parameter(priv, cfg);
    if (err < 0) {
        mttcan_err("mttcan_config_message_ram failed.\n");
        return err;
    }

    /* Get parameters of busoff restarting */
    mttcan_set_restart_param(priv);

    /* dedicated tx buffer mask */
    if (cfg->tx_element_num[TX_ELMT_BUF] > 0) {
        priv->dedicated_txb_mask = GENMASK(cfg->tx_element_num[TX_ELMT_BUF] - 1, 0);
    } else {
        priv->dedicated_txb_mask = 0;
    }

    err = mttcan_ioremap_resource(priv, cfg);
    if (err < 0) {
        mttcan_err("mttcan_ioremap_resource failed.\n");
        return err;
    }

    if (priv->plat_type == PLAT_TYPE_FPGA) {
        mttcan_info("platform type is FPGA\n");
        cfg->cclk_freq_hz = FPGA_CORE_CLOCK_FREQ;
        cfg->hclk_freq_hz = FPGA_HOST_CLOCK_FREQ;
    }
    err = memcpy_s(&priv->cfg, sizeof(struct mttcan_config), cfg, sizeof(struct mttcan_config));
    if (unlikely(err != EOK)) {
        mttcan_err("copied cfg failed. err = %d\n", err);
        return err;
    }
    priv->can.clock.freq = cfg->cclk_freq_hz;
    priv->rx_irq = cfg->irq0;
    priv->tx_irq = cfg->irq1;
    priv->timebase_rollover_ms = ((u64)cfg->ts_cnt_prescaler << TIMEBASE_BITS) * MSEC_PER_SEC / cfg->hclk_freq_hz;

    platform_set_drvdata(pdev, ndev);
    SET_NETDEV_DEV(ndev, &pdev->dev);

    err = mttcan_alloc_kfifo(priv);
    if (err != 0) {
        mttcan_err("mttcan_alloc_kfifo failed. err = %d\n", err);
        return err;
    }
    tasklet_init(&priv->rx_tasklet, mttcan_rx_push_skb, (unsigned long)(uintptr_t)priv);
    tasklet_init(&priv->tx_tasklet, mttcan_tx_push_skb, (unsigned long)(uintptr_t)priv);
    mttcan_init_perf_record(priv);

    return 0;
}

static u32 get_reset_reg_bit(u32 mttcan_id)
{
#define DEV_NOT_USE 32
    /* if v1 , reset reg not have can5 and can13 */
    u32 reset_reg_bit[MAX_CAN_NUN] = {
        0, 1, 2, 3, 4, DEV_NOT_USE, 5, 6, 7, 8, 9, 10, 11, DEV_NOT_USE, 12};
    return reset_reg_bit[mttcan_id];
}

STATIC void mttcan_reset_chip(const struct net_device *ndev)
{
    const struct mttcan_priv *priv = netdev_priv(ndev);
    u32 offset;

    if (priv->chip_version == CHIP_VERSION_V1) {
        offset = get_reset_reg_bit(priv->cfg.mttcan_id);
    } else {
        offset = 0;
    }

    mttcan_request_reset(&priv->addr, offset); /* reset */
    udelay(RESET_WIDTH_US);
    mttcan_remove_reset(&priv->addr, offset); /* de-reset */
    mttcan_info("%s:chip reset succeed.\n", ndev->name);
}

STATIC int mttcan_reset_mram(const struct net_device *ndev)
{
    u32 start, end, i;
    const struct mttcan_priv *priv = netdev_priv(ndev);

    start = priv->elmt_param[ELMT_SIDF].off;
    end = priv->elmt_param[ELMT_TMC].off + priv->elmt_param[ELMT_TMC].num * ELMT_SIZE_TRIG_MEM;

    for (i = start; i < end; i += BYTES_OF_WORD) {
        if (write_reg_and_report_fault(ndev, priv->addr.mram_base, i, 0) != 0) {
            mttcan_err("reset message ram failed.\n");
            return -1;
        }
    }

    mttcan_info("%s: reset message ram succeed.\n", ndev->name);
    return 0;
}

int mttcan_init(const struct net_device *ndev)
{
    int err;
    struct mttcan_priv *priv = netdev_priv(ndev);
    
    (void)priv;
    /* reset can ip */
    mttcan_reset_chip(ndev);

    /* reset message ram */
    err = mttcan_reset_mram(ndev);
    if (err < 0) {
        mttcan_err("mttcan_reset_mram failed.\n");
        return err;
    }

    /* test endian and print mttcan core release */
    mttcan_print_revision(ndev);

    /* configure controller */
    err = mttcan_config_controller(ndev);
    if (err < 0) {
        mttcan_err("mttcan_config_controller failed.\n");
        return err;
    }

    err = can_drv_feature_init();
    if (err < 0) {
        mttcan_err("mttcan dmsi init fail, can_id = %d.\n", priv->cfg.mttcan_id);
        return err;
    }

    return 0;
}

STATIC int mttcan_set_normal_work_mode(const struct net_device *ndev)
{
    if (mttcan_set_loopback_mode(ndev, false) != 0) {
        mttcan_err("%s: mttcan_set_loopback_mode failed.\n", ndev->name);
        return -1;
    }
    if (mttcan_set_bus_monitor_mode(ndev, false) != 0) {
        mttcan_err("%s: mttcan_set_bus_monitor_mode failed.\n", ndev->name);
        return -1;
    }
    return 0;
}

STATIC int mttcan_set_ext_loopback_mode(const struct net_device *ndev)
{
    if (mttcan_set_loopback_mode(ndev, true) != 0) {
        mttcan_err("%s: mttcan_set_loopback_mode failed.\n", ndev->name);
        return -1;
    }
    if (mttcan_set_bus_monitor_mode(ndev, false) != 0) {
        mttcan_err("%s: mttcan_set_bus_monitor_mode failed.\n", ndev->name);
        return -1;
    }
    return 0;
}

STATIC int mttcan_set_bus_monitor(const struct net_device *ndev)
{
    if (mttcan_set_loopback_mode(ndev, false) != 0) {
        mttcan_err("%s: mttcan_set_loopback_mode failed.\n", ndev->name);
        return -1;
    }
    if (mttcan_set_bus_monitor_mode(ndev, true) != 0) {
        mttcan_err("%s: mttcan_set_bus_monitor_mode failed.\n", ndev->name);
        return -1;
    }
    return 0;
}

STATIC int mttcan_set_int_loopback_mode(const struct net_device *ndev)
{
    if (mttcan_set_loopback_mode(ndev, true) != 0) {
        mttcan_err("%s: mttcan_set_loopback_mode failed.\n", ndev->name);
        return -1;
    }
    if (mttcan_set_bus_monitor_mode(ndev, true) != 0) {
        mttcan_err("%s: mttcan_set_bus_monitor_mode failed.\n", ndev->name);
        return -1;
    }
    return 0;
}

STATIC int mttcan_set_one_shot_mode(const struct net_device *ndev)
{
    struct mttcan_priv *priv = netdev_priv(ndev);
    if (priv->can.ctrlmode & CAN_CTRLMODE_ONE_SHOT) {
        if (mttcan_set_auto_retrans(ndev, false) != 0) {  /* disable automatic retransmission */
            mttcan_err("%s: mttcan_set_auto_retrans failed.\n", ndev->name);
            return -1;
        }
    } else {
        if (mttcan_set_auto_retrans(ndev, true) != 0) {  /* enable automatic retransmission */
            mttcan_err("%s: mttcan_set_auto_retrans failed.\n", ndev->name);
            return -1;
        }
    }
    return 0;
}

STATIC int mttcan_set_canfd_mode(const struct net_device *ndev)
{
    struct mttcan_priv *priv = netdev_priv(ndev);
    if (priv->can.ctrlmode & CAN_CTRLMODE_FD) {
        if (mttcan_enable_canfd(ndev, true) != 0) {
            mttcan_err("%s: mttcan_enable_canfd failed.\n", ndev->name);
            return -1;
        }
    } else {
        if (mttcan_enable_canfd(ndev, false) != 0) {
            mttcan_err("%s: mttcan_enable_canfd failed.\n", ndev->name);
            return -1;
        }
    }
    return 0;
}

STATIC int mttcan_set_working_mode(const struct net_device *ndev)
{
    u32 ctrl_mode;
    struct mttcan_priv *priv = netdev_priv(ndev);

    ctrl_mode = priv->can.ctrlmode & CAN_CTRL_MODE_MASK;
    switch (ctrl_mode) {
        case NORMAL_WORK_MODE:
            if (mttcan_set_normal_work_mode(ndev) != 0) {
                mttcan_err("%s: mttcan_set_normal_work_mode failed.\n", ndev->name);
                return -1;
            }
            break;

        case EXT_LOOPBACK_MODE:
            if (mttcan_set_ext_loopback_mode(ndev) != 0) {
                mttcan_err("%s: mttcan_set_ext_loopback_mode failed.\n", ndev->name);
                return -1;
            }
            break;

        case BUS_MONITOR_MODE:
            if (mttcan_set_bus_monitor(ndev) != 0) {
                mttcan_err("%s: mttcan_set_bus_monitor failed.\n", ndev->name);
                return -1;
            }
            break;

        case INT_LOOPBACK_MODE:
            if (mttcan_set_int_loopback_mode(ndev) != 0) {
                mttcan_err("%s: mttcan_set_int_loopback_mode failed.\n", ndev->name);
                return -1;
            }
            break;

        default:
            /* The condition variable is of the enumeration type. */
            break;
    }

    if (mttcan_set_one_shot_mode(ndev) != 0) {
        mttcan_err("%s: mttcan_set_one_shot_mode failed.\n", ndev->name);
        return -1;
    }

    if (mttcan_set_canfd_mode(ndev) != 0) {
        mttcan_err("%s: mttcan_set_canfd_mode failed.\n", ndev->name);
        return -1;
    }

    return 0;
}

STATIC u64 mttcan_read_timebase(struct cyclecounter *cyc_counter)
{
    struct mttcan_priv *priv = container_of(cyc_counter, struct mttcan_priv, cc);

    return (u64)mttcan_read_reg(&priv->addr, REG_TSU_ATB);
}

STATIC void mttcan_init_cyclecounter(struct mttcan_priv *priv)
{
    u32 timebase_prescaler;
    u32 tscfg; /* Timestamp Configuration */

    priv->cc.read = mttcan_read_timebase;
    priv->cc.mask = ATB_TB_MASK;
    priv->cc.shift = 0;

    tscfg = mttcan_read_reg(&priv->addr, REG_TSU_TSCFG);
    timebase_prescaler = ((tscfg & TSCFG_TBPRE_MASK) >> TSCFG_TBPRE_SHIFT) + 1;
    priv->cc.mult = ((u64)NSEC_PER_SEC * timebase_prescaler) / priv->cfg.hclk_freq_hz;
}

STATIC u64 mttcan_get_time_ns(struct mttcan_priv *priv)
{
#ifdef SUPPORT_CONFIG_KTIME_TYPE
    struct timespec64 tv = {0};

    if (priv->ktime_type == KTIME_TYPE_REAL) {
        ktime_get_real_ts64(&tv);
    } else {
        ktime_get_virtual_ts64(&tv);
    }

    return timespec64_to_ns(&tv);
#else
    return ktime_to_ns(ktime_get_real());
#endif
}

STATIC void mttcan_timer_callback(struct timer_list *pram)
{
    struct mttcan_priv *priv = from_timer(priv, pram, timer);
    u64 kernel_time_ns;
    u64 timer_period;
    unsigned long irq_flags;

    raw_spin_lock_irqsave(&priv->tc_lock, irq_flags);
    kernel_time_ns = mttcan_get_time_ns(priv);
    timecounter_init(&priv->tc, &priv->cc, kernel_time_ns);
    raw_spin_unlock_irqrestore(&priv->tc_lock, irq_flags);
    timer_period = TIMER_PERIOD_MS;
    if (unlikely(timer_period > (priv->timebase_rollover_ms >> 1))) {
        timer_period = priv->timebase_rollover_ms >> 1;
    }

    mod_timer(&priv->timer, jiffies + (msecs_to_jiffies(timer_period)));
}

#ifdef SUPPORT_MTTCAN_DFX
STATIC int mttcan_calc_current_busload(struct mttcan_priv *priv)
{
    int busload_percent;
    struct frame_stats *fc = &priv->busload.stats;
    s64 time_now_ns = ktime_to_ns(ktime_get_real());
    s64 time_gaps;
    s64 time_gaps_to_ms;

    /* can frame bus load */
    busload_percent = BUSLOAD_PERCENT_MAX * BUSLOAD_MULTIPLES * (fc->can_ext_packets * CAN_EXF_STUFF_LEN
        + fc->can_std_packets * CAN_SDF_STUFF_LEN
        + fc->can_payload_bits) / priv->can.bittiming.bitrate;

    /* calculate CANFD frame bus load */
    if (priv->can.ctrlmode & CAN_CTRLMODE_FD) {
        busload_percent += BUSLOAD_PERCENT_MAX * BUSLOAD_MULTIPLES * (
            /* CANFD extend BRS frames */
            fc->canfd_ext_brs_packets * CANFD_EXF_LS_STUFF_LEN

            /* CANFD extend normal frames */
            + fc->canfd_ext_normal_packets * (CANFD_EXF_LS_STUFF_LEN + CANFD_EXF_HS_STUFF_LEN)
            + fc->canfd_ext_normal_bits

            /* CANFD standard normal frames */
            + fc->canfd_std_normal_packets * (CANFD_SDF_LS_STUFF_LEN + CANFD_SDF_HS_STUFF_LEN)
            + fc->canfd_std_normal_bits

            /* CANFD standard BRS frames */
            + fc->canfd_std_brs_packets * CANFD_SDF_LS_STUFF_LEN)
            / priv->can.bittiming.bitrate;

        busload_percent += BUSLOAD_PERCENT_MAX * BUSLOAD_MULTIPLES * (
            /* CANFD Extend BRS frames */
            fc->canfd_ext_brs_packets * CANFD_EXF_HS_STUFF_LEN
            + fc->canfd_ext_brs_bits

            /* CANFD Standard BRS frames */
            + fc->canfd_std_brs_packets * CANFD_SDF_HS_STUFF_LEN
            + fc->canfd_std_brs_bits)
            / priv->can.fd.data_bittiming.bitrate;
    }

    time_gaps = time_now_ns - fc->busload_last_time;
    if (time_gaps > 0) {
        time_gaps_to_ms = time_gaps / MS_TO_US / US_TO_NS;
        if (time_gaps_to_ms == 0) {
            mttcan_err("divisor cannot be zero.\n");
            return -1;
        }
        busload_percent = busload_percent * S_TO_MS / time_gaps_to_ms;
    }
    busload_percent /= BUSLOAD_MULTIPLES;

    if (memset_s(fc, sizeof(struct frame_stats), 0, sizeof(struct frame_stats)) != EOK) {
        mttcan_err("init frame count error.\n");
    }
    fc->busload_last_time = time_now_ns;
    return busload_percent;
}

STATIC void mttcan_check_busload(struct timer_list *pram)
{
    struct mttcan_priv *priv = from_timer(priv, pram, busload.cheking_timer);
    int busload_percent;

    if (!mttcan_working(priv)) {
        /* If can in stop status, stop to run timer */
        if (priv->can.state != CAN_STATE_STOPPED) {
            mod_timer(&priv->busload.cheking_timer, jiffies + (msecs_to_jiffies(priv->busload.interval)));
        }
        return;
    }

    busload_percent = mttcan_calc_current_busload(priv);
    busload_percent = (busload_percent >= BUSLOAD_PERCENT_MAX) ? BUSLOAD_PERCENT_MAX : busload_percent;
    pr_debug("[mttcan][%s:%d]can%u: busload_percent = %d\n", __func__, __LINE__, priv->cfg.mttcan_id, busload_percent);

    if (!priv->busload.busload_warning) {
        if (busload_percent >= priv->busload.threshold) {
            mttcan_print_runlog(priv, CAN_LOG_BUSLOAD_WARNING,
                "The CAN bus load exceeds the threshold. %d%%.\n", priv->busload.threshold);
            priv->busload.continuous_idle_nums = 0;
            priv->busload.busload_warning = true;
        }
    } else {
        if (busload_percent >= priv->busload.threshold) {
            priv->busload.continuous_idle_nums = 0;
            mod_timer(&priv->busload.cheking_timer, jiffies + (msecs_to_jiffies(priv->busload.interval)));
            return;
        }

        if (priv->busload.continuous_idle_nums < INT_MAX) {
                priv->busload.continuous_idle_nums++;
        }
        if (priv->busload.continuous_idle_nums >= priv->busload.idle_times_to_clear) {
            mttcan_print_runlog(priv, CAN_LOG_BUSLOAD_WARNING, "The CAN bus load is lower than the threshold "
                "for %d consecutive times, clear the alarm.\n", priv->busload.idle_times_to_clear);
            priv->busload.busload_warning = false;
            priv->busload.continuous_idle_nums = 0;
        }
    }
    mod_timer(&priv->busload.cheking_timer, jiffies + (msecs_to_jiffies(priv->busload.interval)));
}
#endif

STATIC bool is_err_count_update(struct mttcan_priv *priv)
{
    if (memcmp(&priv->status_cnt, &priv->last_status_cnt, sizeof(priv->status_cnt)) != 0) {
        return true;
    }
    return false;
}

STATIC void mttcan_echo_sysfs_log(struct timer_list *pram)
{
    int ret;
    struct mttcan_priv *priv = from_timer(priv, pram, echo_sysfs_log_timer);

    if (is_err_count_update(priv)) {
        mttcan_dump_reg(priv->ndev);
        mttcan_info(
            "can%u err_pkt count: "
            "stuff=%llu,form=%llu,"
            "ack=%llu,bit0=%llu,"
            "bit1=%llu,crc=%llu,"
            "rxfl=%llu,txbf=%llu,"
            "unknow=%llu,"
            "rxkf=%llu,txkf=%llu,"
            "rxdelay2ms=%llu,rxdelay5ms=%llu,"
            "txfl=%llu\n",
            priv->cfg.mttcan_id,
            priv->status_cnt.lec_type_stuff_err_cnt, priv->status_cnt.lec_type_form_err_cnt,
            priv->status_cnt.lec_type_ack_err_cnt, priv->status_cnt.lec_type_bit0_err_cnt,
            priv->status_cnt.lec_type_bit1_err_cnt, priv->status_cnt.lec_type_crc_err_cnt,
            priv->status_cnt.rx_fifo_lost_err_cnt, priv->status_cnt.tx_buffer_full_err_cnt,
            priv->status_cnt.unknow_err_cnt,
            priv->status_cnt.rx_kfifo_full_cnt, priv->status_cnt.tx_kfifo_full_cnt,
            priv->status_cnt.rx_tasklet_delay2ms_cnt, priv->status_cnt.rx_tasklet_delay5ms_cnt,
            priv->status_cnt.tx_fifo_lost_err_cnt);
        ret = memcpy_s(&priv->last_status_cnt, sizeof(struct run_status_cnt),
            &priv->status_cnt, sizeof(struct run_status_cnt));
        if (ret != 0) {
            mttcan_err("update last_status_cnt failed.\n");
        }
    }
    mod_timer(&priv->echo_sysfs_log_timer, jiffies + (msecs_to_jiffies(CAN_ECHO_STSFSLOG_INTERVAL)));
}

STATIC void mttcan_setup_timer(struct mttcan_priv *priv)
{
    mttcan_init_cyclecounter(priv);
    raw_spin_lock_init(&priv->tc_lock);
    timer_setup(&priv->timer, mttcan_timer_callback, 0);

#ifdef SUPPORT_MTTCAN_DFX
    priv->busload.busload_warning = false;
    priv->busload.threshold = CAN_BUSLOAD_WARNING_THRESHOLD;
    priv->busload.interval = CAN_BUSLOAD_CHECHKING_INTERVAL;
    priv->busload.idle_times_to_clear = CAN_BUSLOAD_IDLE_TIMES_TO_CLEAR;

    timer_setup(&priv->busload.cheking_timer, mttcan_check_busload, 0);
#endif
    timer_setup(&priv->echo_sysfs_log_timer, mttcan_echo_sysfs_log, 0);

    mttcan_init_fault_recovery_timer(priv->ndev);
}

STATIC int mttcan_wait_busoff_recover(const struct net_device *ndev)
{
    u32 ecr;
    u32 tx_berr_cnt;
    u32 try_times = BUSOFF_RECONVER_TIMEOUT_US;
    u32 try_index = 0;
    u64 busoff_delay_ts;
    u64 busoff_restart_ts;
    u64 busoff_wait_ts;
    struct mttcan_priv *priv = netdev_priv(ndev);
    mttcan_info("%s is waiting for bus-off sequence.\n", ndev->name);

    /* waiting for bus-off sequence (129*11 bits) */
    while (try_index < try_times) {
        ecr = mttcan_read_reg(&priv->addr, REG_ECR);
        tx_berr_cnt = (ecr & ECR_TEC_MASK) >> ECR_TEC_SHIFT;
        if (tx_berr_cnt < ERROR_PASSIVE_ERR_COUNTER) {
            goto busoff_delay;
        } else {
            udelay(1);
        }
        try_index++;
    }

    mttcan_err("%s bus-off recover fail.\n", ndev->name);
    return -1;

busoff_delay:
#define EXPECTED_RECONVER_MAX_TS 11000
    busoff_delay_ts = ktime_to_ns(ktime_get_real()) - priv->busoff_ts;
    pr_debug("%s busoff_delay_ts = %llu\n", ndev->name, busoff_delay_ts);
    busoff_restart_ts = ((s64)priv->busoff_restart_ms * MS_TO_US * US_TO_NS);
    if (busoff_delay_ts < busoff_restart_ts) {
        busoff_wait_ts = (busoff_restart_ts - busoff_delay_ts) / US_TO_NS;
        if (busoff_wait_ts > EXPECTED_RECONVER_MAX_TS) {
            mttcan_info("%s don't wait bus-off recover.\n", ndev->name);
            return 0;
        }
        pr_debug("%s busoff_wait_ts = %llu\n", ndev->name, busoff_wait_ts);
        while (busoff_wait_ts != 0) {
            udelay(1);
            busoff_wait_ts--;
        }
    }
    mttcan_info("%s bus-off recover succ.\n", ndev->name);
    return 0;
}

STATIC void mttcan_set_start_state(u32 psr, struct mttcan_priv *priv)
{
    if (psr & PSR_BO_BIT) {
        priv->can.state = CAN_STATE_BUS_OFF;
    } else if (psr & PSR_EP_BIT) {
        priv->can.state = CAN_STATE_ERROR_PASSIVE;
    } else if (psr & PSR_EW_BIT) {
        priv->can.state = CAN_STATE_ERROR_WARNING;
    } else {
        priv->can.state = CAN_STATE_ERROR_ACTIVE;
    }
}

STATIC int mttcan_start(const struct net_device *ndev)
{
    int err;
    struct mttcan_priv *priv = netdev_priv(ndev);
    u32 psr = mttcan_read_reg(&priv->addr, REG_PSR); /* Protocol Status Register */

    /* set bit CCCR.INIT and CCCR.CCE */
    err = mttcan_set_init_mode(ndev);
    if (unlikely(err < 0)) {
        mttcan_err("%s: mttcan_set_init_mode failed.\n", ndev->name);
        return err;
    }

    if (mttcan_set_working_mode(ndev) != 0) {
        mttcan_err("%s: mttcan_set_working_mode failed.\n", ndev->name);
        return -1;
    }

    priv->tx_obj = 0;
#ifdef SUPPORT_MTTCAN_FAULT_RECOVERY
    priv->last_busy_tx_idx = ELMT_NUM_MAX_TXB;
    priv->last_busy_tx_time = 0;
    priv->last_netif_stop_time = 0;
#endif

    mttcan_clear_all_interrupt_flags(ndev);
    if (mttcan_enable_interrupts(ndev, INT_LINE_ALL) != 0) {
        mttcan_err("%s: mttcan_enable_interrupts failed.\n", ndev->name);
        return -1;
    }
    mttcan_timer_callback(&priv->timer);
#ifdef SUPPORT_MTTCAN_DFX
    mod_timer(&priv->busload.cheking_timer, jiffies + (msecs_to_jiffies(priv->busload.interval)));
#endif
    mod_timer(&priv->echo_sysfs_log_timer, jiffies + (msecs_to_jiffies(CAN_ECHO_STSFSLOG_INTERVAL)));
    /* clear bit CCCR.INIT and CCCR.CCE */
    err = mttcan_set_normal_mode(ndev);
    if (unlikely(err < 0)) {
        mttcan_err("%s: mttcan_set_normal_mode failed.\n", ndev->name);
        return err;
    }

    mttcan_set_start_state(psr, priv);
    if (psr & PSR_BO_BIT) {
        err = mttcan_wait_busoff_recover(ndev);
        if (err) {
            mttcan_err("%s mttcan_wait_busoff_recover fail.\n", ndev->name);
            return err;
        }
        priv->can.state = CAN_STATE_ERROR_ACTIVE;
    }
    return 0;
}

STATIC void mttcan_stop(const struct net_device *ndev)
{
    int err;
    struct mttcan_priv *priv = netdev_priv(ndev);

    priv->can.state = CAN_STATE_STOPPED;
    del_timer_sync(&priv->timer);
#ifdef SUPPORT_MTTCAN_DFX
    del_timer_sync(&priv->busload.cheking_timer);
#endif
    del_timer_sync(&priv->echo_sysfs_log_timer);
    mttcan_disable_interrupts(ndev, INT_LINE_ALL);

    /* set bit CCCR.INIT and CCCR.CCE */
    err = mttcan_set_init_mode(ndev);
    if (unlikely(err < 0)) {
        mttcan_err("%s: failed to set init mode.\n", priv->cfg.mttcan_name);
    }
}

int get_err_counter(const struct net_device *ndev, u32 *rx_err_cnt, u32 *tx_err_cnt)
{
    u32 ecr; /* Error Counter Register */
    u32 psr; /* Protocol Status Register */
    u32 rx_errors;
    u32 tx_errors;
    struct mttcan_priv *priv = NULL;

    if (unlikely(ndev == NULL)) {
        mttcan_err("Invalid input.\n");
        return -1;
    }

    priv = netdev_priv(ndev);
    psr = mttcan_read_reg(&priv->addr, REG_PSR);
    ecr = mttcan_read_reg(&priv->addr, REG_ECR);
    rx_errors = (ecr & ECR_REC_MASK) >> ECR_REC_SHIFT;
    tx_errors = (ecr & ECR_TEC_MASK) >> ECR_TEC_SHIFT;

    if (ecr & ECR_RP_BIT) {
        rx_errors += COUNTS_PER_BERR;
    }

    if (psr & PSR_BO_BIT) {
        tx_errors += COUNTS_PER_BERR;
    }

    *rx_err_cnt = rx_errors;
    *tx_err_cnt = tx_errors;

    return 0;
}

STATIC int mttcan_get_berr_counter(const struct net_device *ndev, struct can_berr_counter *bec)
{
    int ret;
    u32 rx_err_cnt = 0;
    u32 tx_err_cnt = 0;
    ret = get_err_counter(ndev, &rx_err_cnt, &tx_err_cnt);
    if (ret) {
        mttcan_err("Fail to get err counter, errcode=%d\n", ret);
        return ret;
    }

    bec->rxerr = rx_err_cnt;
    bec->txerr = tx_err_cnt;
    return 0;
}

STATIC int mttcan_set_mode(struct net_device *ndev, enum can_mode mode)
{
    if (unlikely(ndev == NULL)) {
        mttcan_err("Invalid input.\n");
        return -EOPNOTSUPP;
    }

    if (mode != CAN_MODE_START) {
        netif_wake_queue(ndev);
        mttcan_info("%s:unsupported mode.\n", ndev->name);
        return -EOPNOTSUPP;
    }

    mttcan_info("%s:set mode start.\n", ndev->name);
    mttcan_start(ndev);
    netif_wake_queue(ndev);

    return 0;
}

STATIC int mttcan_bind_irq(const struct net_device *ndev)
{
    int err;
    char irq_node_name[CAN_IRQ_NODE_NAME_MAXLEN] = {0};
    struct mttcan_priv *priv = netdev_priv(ndev);

    err = snprintf_s(irq_node_name, sizeof(irq_node_name), sizeof(irq_node_name) - 1, "can%dcpu", priv->cfg.mttcan_id);
    if (err < 0) {
        mttcan_err("failed to snprintf_s irq_node_name.\n");
        return err;
    }
    err = bind_irq_to_core(priv->tx_irq, irq_node_name, "tx_irq_cpu");
    if (err < 0) {
        mttcan_err("device %s irq %d failed to bind tx_irq_cpu.\n", ndev->name, priv->tx_irq);
    }
    err = bind_irq_to_core(priv->rx_irq, irq_node_name, "rx_irq_cpu");
    if (err < 0) {
        mttcan_err("device %s irq %d failed to bind rx_irq_cpu.\n", ndev->name, priv->rx_irq);
    }
    return 0;
}

STATIC void mttcan_unbind_irq(const struct net_device *ndev)
{
    int ret;
    struct mttcan_priv *priv = netdev_priv(ndev);

    ret = irq_set_affinity_hint(priv->rx_irq, NULL);
    if (ret != 0) {
        mttcan_err("irq %d set affinity to null err:%d\n", priv->rx_irq, ret);
    }

    ret = irq_set_affinity_hint(priv->tx_irq, NULL);
    if (ret != 0) {
        mttcan_err("irq %d set affinity to null err:%d\n", priv->tx_irq, ret);
    }
}

STATIC int mttcan_open(struct net_device *ndev)
{
    struct mttcan_priv *priv = netdev_priv(ndev);
    int err;

    err = open_candev(ndev);
    if (unlikely(err < 0)) {
        mttcan_print_oplog(priv, CAN_LOG_DEVICE_UP_FAIL, "device open failed.\n");
        goto exit;
    }

    err = request_irq(priv->rx_irq, mttcan_isr0, IRQF_SHARED, ndev->name, (void *)ndev);
    if (unlikely(err < 0)) {
        mttcan_print_oplog(priv, CAN_LOG_DEVICE_UP_FAIL, "failed to request rx interrupt.\n");
        goto exit_close_candev;
    }

    err = request_irq(priv->tx_irq, mttcan_isr1, IRQF_SHARED, ndev->name, (void *)ndev);
    if (unlikely(err < 0)) {
        mttcan_print_oplog(priv, CAN_LOG_DEVICE_UP_FAIL, "failed to request tx interrupt.\n");
        goto exit_free_irq0;
    }

    err = mttcan_bind_irq(ndev);
    if (unlikely(err < 0)) {
        mttcan_err("device %s failed to bind_irq, it will bind default cpus.\n", ndev->name);
    }

    err = mttcan_start(ndev);
    if (err < 0) {
        mttcan_print_oplog(priv, CAN_LOG_DEVICE_UP_FAIL, "mttcan start failed.\n");
        goto exit_free_irq1;
    }

    netif_start_queue(ndev);
    mttcan_print_oplog(priv, CAN_LOG_DEVICE_UP_SUCC, "open device success.\n");

    return 0;

exit_free_irq1:
    mttcan_unbind_irq(ndev);
    free_irq(priv->tx_irq, ndev);
exit_free_irq0:
    free_irq(priv->rx_irq, ndev);
exit_close_candev:
    close_candev(ndev);
exit:
    return err;
}

STATIC int mttcan_close(struct net_device *ndev)
{
    struct mttcan_priv *priv = netdev_priv(ndev);

    netif_stop_queue(ndev);
    mttcan_stop(ndev);
    mttcan_unbind_irq(ndev);
    free_irq(priv->tx_irq, ndev);
    free_irq(priv->rx_irq, ndev);
    close_candev(ndev);

    mttcan_print_oplog(priv, CAN_LOG_DEVICE_DOWN, "device has been closed success.\n");
    return 0;
}

static netdev_tx_t mttcan_add_tx_request(struct sk_buff *skb, struct net_device *ndev, u32 txb_idx)
{
    struct mttcan_priv *priv = netdev_priv(ndev);
    unsigned long irq_flags;

    raw_spin_lock_irqsave(&priv->tx_complete, irq_flags);
    if ((!netif_carrier_ok(priv->ndev)) || test_and_set_bit(txb_idx, (void *)&priv->tx_obj) == 1) {
        raw_spin_unlock_irqrestore(&priv->tx_complete, irq_flags);
        skb->tstamp = (ktime_t)0;
        mttcan_err("%s: index %u is in use.\n", priv->ndev->name, txb_idx);
        return NETDEV_TX_BUSY;
    }
    /* used for local loopback */
    can_put_echo_skb(skb, ndev, txb_idx, 0);
    mttcan_write_reg(&priv->addr, REG_TXBAR, (1U << txb_idx));
    raw_spin_unlock_irqrestore(&priv->tx_complete, irq_flags);
    return NETDEV_TX_OK;
}

/**
 * Remediate the TX queue status of the CAN device.
 *
 * Check the status of the TX queue when the TX buffer is busy,
 * If the TX buffer of the hardware is full, stop the netif TX queue,
 * Otherwise, the wakeup the TX queue and send data frames.
 */
STATIC void mttcan_remediate_tx_queue(struct net_device *ndev)
{
    struct mttcan_priv *priv = netdev_priv(ndev);

    if (priv->can.state == CAN_STATE_BUS_OFF) {
        return;
    }

    if (mttcan_tx_fifo_full(priv)) {
        if (!netif_queue_stopped(ndev)) {
            netif_stop_queue(ndev);
            priv->last_netif_stop_time = jiffies;
        }
        return;
    }

    if (priv->can.state != CAN_STATE_BUS_OFF) {
        if (netif_queue_stopped(ndev)) {
            netif_wake_queue(ndev);
            priv->last_netif_stop_time = 0;
        }
    }
}

STATIC netdev_tx_t mttcan_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
    int ret;
    u32 txb_idx;
    u32 txfq_free;
    struct mttcan_priv *priv = netdev_priv(ndev);

    bool is_inval_skb = can_dropped_invalid_skb(ndev, skb);
    if (unlikely(is_inval_skb == true)) {
        mttcan_info("%s: invalid skb is dropped.\n", ndev->name);
        return NETDEV_TX_OK;
    }

    /*
     * if mttcan is in bus monitoring mode,
     * the M_TTCAN is able to receive valid data frames and valid remote frames,
     * but cannot start a transmission.
     */
    if (unlikely((priv->can.ctrlmode & CAN_CTRL_MODE_MASK) == BUS_MONITOR_MODE)) {
        mttcan_err("%s: can't start a transmission in bus monitoring mode.\n", ndev->name);
        return NETDEV_TX_OK;
    }

    if (!netif_carrier_ok(ndev) || (priv->can.state == CAN_STATE_SLEEPING)) {
        return NETDEV_TX_BUSY;
    }

    /* write message to dedicated tx buffer or tx fifo/queue */
    ret = mttcan_write_tx_message(ndev, skb, &txb_idx);
    if (unlikely(ret < 0)) {
        mttcan_remediate_tx_queue(ndev);
        priv->status_cnt.tx_buffer_full_err_cnt++;
        return NETDEV_TX_BUSY;
    }
    skb->tstamp = ktime_get_real();

    /* If the TX buffer/FIFO/queue is full, stop tx queue.
     * this step must before call mttcan_add_tx_request function.
     */
    txfq_free = mttcan_tx_fifo_free_level(priv);
    if (txfq_free == 1 && priv->dedicated_txb_mask == 0) {
        netif_stop_queue(ndev);
#ifdef SUPPORT_MTTCAN_FAULT_RECOVERY
        priv->last_netif_stop_time = jiffies;
#endif
    }
    if (txfq_free == 0 && mttcan_tx_buff_free_less_one(priv)) {
        netif_stop_queue(ndev);
#ifdef SUPPORT_MTTCAN_FAULT_RECOVERY
        priv->last_netif_stop_time = jiffies;
#endif
    }
    return mttcan_add_tx_request(skb, ndev, txb_idx);
}

STATIC int mttcan_set_ktime_type(struct mttcan_priv *priv, struct ifreq *ifr)
{
    unsigned char flag;

    if (copy_from_user(&flag, ifr->ifr_data, sizeof(unsigned char)) != 0) {
        mttcan_err("failed to copy from user.\n");
        return -EINVAL;
    }

    if (flag != KTIME_TYPE_REAL && flag != KTIME_TYPE_VIRTUAL) {
        mttcan_err("flag is invalid.(flag=%u)\n", flag);
        return -EINVAL;
    }

    priv->ktime_type = flag;
    return 0;
}

STATIC int mttcan_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
    int ret;
    struct mttcan_priv *priv = netdev_priv(dev);

    if (ifr == NULL) {
        mttcan_err("ifr is invalid.\n");
        return -EINVAL;
    }

    switch (cmd) {
        case CAN_SIODEVPRIVATE_KTIME_TYPE:
            ret = mttcan_set_ktime_type(priv, ifr);
            break;
        default:
            mttcan_err("cmd is invalid.(cmd=%d)\n", cmd);
            ret = -EINVAL;
            break;
    }

    return ret;
}

static int mttcan_change_mtu(struct net_device *dev, int new_mtu)
{
    struct can_priv *priv = netdev_priv(dev);

    if (dev->flags & IFF_UP)
        return -EBUSY;

    if (new_mtu != CAN_MTU && new_mtu != CANFD_MTU &&
        !(priv->ctrlmode_supported & CAN_CTRLMODE_XL && can_is_canxl_dev_mtu(new_mtu)))
        return -EINVAL;

    WRITE_ONCE(dev->mtu, new_mtu);
    return 0;
}

static const struct net_device_ops can_drv_netdev_ops = {
    .ndo_open = mttcan_open,
    .ndo_stop = mttcan_close,
    .ndo_change_mtu = mttcan_change_mtu,
    .ndo_start_xmit = mttcan_start_xmit,
    .ndo_do_ioctl = mttcan_ioctl,
};

static const struct can_bittiming_const mttcan_bittiming_const_nominal = {
    .tseg1_min = TSEG1_MIN, /* Time segment 1 = prop_seg + phase_seg1 */
    .tseg1_max = TSEG1_MAX,
    .tseg2_min = TSEG2_MIN, /* Time segment 2 = phase_seg2 */
    .tseg2_max = TSEG2_MAX,
    .brp_min = BRP_MIN,
    .brp_max = BRP_MAX,
    .brp_inc = BRP_INC,
    .sjw_max = SJW_MAX,
    .name = KBUILD_MODNAME,
};

static const struct can_bittiming_const mttcan_bittiming_const_data = {
    .tseg1_min = FD_TSEG1_MIN, /* Time segment 1 = prop_seg + phase_seg1 */
    .tseg1_max = FD_TSEG1_MAX,
    .tseg2_min = FD_TSEG2_MIN, /* Time segment 2 = phase_seg2 */
    .tseg2_max = FD_TSEG2_MAX,
    .brp_min = FD_BRP_MIN,
    .brp_max = FD_BRP_MAX,
    .brp_inc = FD_BRP_INC,
    .sjw_max = FD_SJW_MAX,
    .name = KBUILD_MODNAME,
};

STATIC int mttcan_register_can_device(struct net_device *ndev)
{
    struct mttcan_priv *priv = netdev_priv(ndev);

    priv->can.bittiming_const = &mttcan_bittiming_const_nominal;
    priv->can.fd.data_bittiming_const = &mttcan_bittiming_const_data;
    priv->can.do_get_berr_counter = mttcan_get_berr_counter;     /* Data transmission error counting */
    priv->can.do_set_mode = mttcan_set_mode;                     /* Mode settings */
    priv->can.do_set_bittiming = mttcan_set_bittiming;           /* Bit rate settings */
    priv->can.fd.do_set_data_bittiming = mttcan_set_data_bittiming; /* Data bit rate settings */
    priv->can.ctrlmode_supported = CAN_CTRLMODE_FD | CAN_CTRLMODE_LOOPBACK | CAN_CTRLMODE_LISTENONLY |
        CAN_CTRLMODE_BERR_REPORTING | CAN_CTRLMODE_ONE_SHOT;

    ndev->netdev_ops = &can_drv_netdev_ops;
    ndev->flags |= (IFF_NOARP | IFF_ECHO); /* support local echo */

    return register_candev(ndev);
}

STATIC int mttcan_probe_init(struct mttcan_config *cfg, struct platform_device *pdev)
{
    int err;
    struct device *dev = &pdev->dev;
    const struct of_device_id *match = NULL;

    match = of_match_device(dev->driver->of_match_table, dev);
    if (match == NULL) {
        mttcan_err("No matching device is found!\n");
        return -ENODEV;
    }

    /* Get can device info from dts */
    err = mttcan_get_dts_info(cfg, pdev);
    if (err != 0) {
        mttcan_err("mttcan_get_device_info failed!\n");
        return -ENODEV;
    }

    /* set default configuration */
    mttcan_set_default_cfg(cfg);
    return 0;
}

STATIC int mttcan_iomux_init(struct device *dev, struct mttcan_config *cfg)
{
    struct pinctrl *pinctrl = NULL;
    struct pinctrl_state *pins_default = NULL;
    int ret;

    mttcan_debug("set mttcan gpio\n");
    pinctrl = devm_pinctrl_get(dev);
    if (IS_ERR(pinctrl)) {
        mttcan_info("current envaroment not suport pinctrl.\n");
        pinctrl = NULL;
        return 0;
    }
    pins_default = pinctrl_lookup_state(pinctrl, PINCTRL_STATE_DEFAULT);
    if (IS_ERR(pins_default)) {
        mttcan_err("pinctrl_lookup_state failed\n");
        devm_pinctrl_put(pinctrl);
        pins_default = NULL;
        return -ENOENT;
    }
    ret = pinctrl_select_state(pinctrl, pins_default);
    if (ret < 0) {
        mttcan_err("pinctrl_select_state failed\n");
        devm_pinctrl_put(pinctrl);
        return ret;
    }

    return ret;
}

STATIC int plat_drv_probe(struct platform_device *pdev)
{
    int err;
    struct net_device *ndev = NULL;
    struct mttcan_priv *priv = NULL;
    struct mttcan_config cfg;

    if (mttcan_probe_init(&cfg, pdev)) {
        mttcan_err("mttcan_probe_init failed!\n");
        return -ENODEV;
    }

    /* Create a CAN device instance */
    ndev = mttcan_alloc_can_device(pdev, &cfg);
    if (ndev == NULL) {
        mttcan_err("mttcan_alloc_can_device failed!\n");
        return -ENOMEM;
    }

    /* Set configuration parameters to priv */
    err = mttcan_set_private_data(ndev, &cfg, pdev);
    if (err < 0) {
        mttcan_err("mttcan_set_private_data failed!\n");
        err = -ENODEV;
        goto exit_free;
    }

    /* Initialize mttcan controller */
    err = mttcan_init(ndev);
    if (err < 0) {
        mttcan_err("mttcan_init failed!\n");
        goto exit_clear_drvdata;
    }

    // register to dms in AOS-Core and AOS-Linux
    priv = netdev_priv(ndev);
    err = mttcan_register_dms_node(priv->cfg.mttcan_id);
    if (err < 0) {
        mttcan_err("mttcan_register_dms_node failed, can_id = %d.\n", priv->cfg.mttcan_id);
        goto exit_clear_drvdata;
    }

    /* Set gpio */
    err = mttcan_iomux_init(&pdev->dev, &cfg);
    if (err < 0) {
        mttcan_err("mttcan_iomux_init failed.\n");
        goto exit_unregister_dms;
    }
#ifdef RUN_IN_AOS
    mttcan_proc_create_node(ndev);
#else
    can_drv_sysfs_add_nodes(&pdev->dev);
#endif
    /* Register can network device */
    err = mttcan_register_can_device(ndev);
    if (err < 0) {
        mttcan_err("mttcan_register_can_device failed!\n");
        goto exit_remove_sysfs;
    }

    priv = netdev_priv(ndev);
    mttcan_setup_timer(priv);
    g_can_net_devices[cfg.mttcan_id] = ndev;
    (void)dev_set_name(&pdev->dev, "can%x", cfg.mttcan_id);

    return 0;

exit_remove_sysfs:
#ifdef RUN_IN_AOS
    mttcan_proc_remove_node(ndev);
#else
    can_drv_sysfs_remove_nodes(&pdev->dev);
#endif
exit_unregister_dms:
    can_unregister_dms(priv->cfg.mttcan_id);
exit_clear_drvdata:
    platform_set_drvdata(pdev, NULL);

exit_free:
    mttcan_free_can_device(ndev);
    mttcan_err("plat_drv_probe failed!\n");

    return err;
}

STATIC void plat_drv_remove(struct platform_device *pdev)
{
    struct net_device *ndev = platform_get_drvdata(pdev);
    struct mttcan_priv *priv = netdev_priv(ndev);

    mttcan_stop_fault_recovery_timer(ndev);

    can_unregister_dms(priv->cfg.mttcan_id);
    can_drv_feature_uninit();

    unregister_candev(ndev);
    platform_set_drvdata(pdev, NULL);
#ifdef RUN_IN_AOS
    mttcan_proc_remove_node(ndev);
#else
    can_drv_sysfs_remove_nodes(&pdev->dev);
#endif
    mttcan_free_can_device(ndev);

    
}

STATIC int mttcan_resume_bitrate(struct net_device *ndev)
{
    struct mttcan_priv *priv = netdev_priv(ndev);

    if (priv->can.bittiming.bitrate != 0) {
        if (mttcan_set_bittiming(ndev) != 0) {
            mttcan_err("%s: mttcan_set_bittiming failed.\n", ndev->name);
            return -1;
        }
    }

    if ((priv->can.ctrlmode & CAN_CTRLMODE_FD) && (priv->can.fd.data_bittiming.bitrate != 0)) {
        if (mttcan_set_data_bittiming(ndev) != 0) {
            mttcan_err("%s: mttcan_set_data_bittiming failed.\n", ndev->name);
            return -1;
        }
    }
    return 0;
}

#ifdef RUN_IN_AOS
STATIC void mttcan_reset_kfifo(struct mttcan_priv *priv)
{
    struct sk_buff *skb = NULL;
    while (kfifo_out(&priv->rx_skb_kfifo, &skb, sizeof(struct sk_buff*))) {
        kfree_skb(skb);
    }
    while (kfifo_out(&priv->tx_skb_kfifo, &skb, sizeof(struct sk_buff*))) {
        kfree_skb(skb);
    }
}

#else
STATIC void mttcan_reset_kfifo(struct mttcan_priv *priv)
{
    struct sk_buff *skb = NULL;
    while (kfifo_get(&priv->rx_skb_kfifo, &skb)) {
        kfree_skb(skb);
    }
    while (kfifo_get(&priv->tx_skb_kfifo, &skb)) {
        kfree_skb(skb);
    }
}
#endif

STATIC int plat_drv_resume(struct device *dev)
{
    struct net_device *ndev = NULL;
    struct mttcan_priv *priv = NULL;
    int err;

    MTTCAN_BOOTDOT_INIT(SNAPSHOT_STATUS_RESUME, CAN_RESUME_EXPECT);
    MTTCAN_BOOTDOT(CAN_RESUME_START);
    ndev = dev_get_drvdata(dev);
    priv = netdev_priv(ndev);

    MTTCAN_BOOTDOT(CAN_RESUMU_MODULE_INIT);
    err = mttcan_init(ndev);
    if (err < 0) {
        mttcan_print_runlog(priv, CAN_LOG_PM_RESUME_FAIL, "mttcan init failed.\n");
        return err;
    }

    MTTCAN_BOOTDOT(CAN_RESUMU_BITRATE);
    if (mttcan_resume_bitrate(ndev) != 0) {
        mttcan_print_runlog(priv, CAN_LOG_PM_RESUME_FAIL, "mttcan_resume_bitrate failed.\n");
        return -1;
    }

    MTTCAN_BOOTDOT(CAN_RESUMU_SET_WORKMODE);
    if (netif_running(ndev)) {
        open_candev(ndev);
        err = mttcan_bind_irq(ndev);
        if (unlikely(err < 0)) {
            mttcan_err("failed to bind_irq, it will bind default cpus.\n");
        }
        err = mttcan_start(ndev);
        if (err < 0) {
            mttcan_print_runlog(priv, CAN_LOG_PM_RESUME_FAIL, "mttcan start failed.\n");
            return err;
        }
        netif_device_attach(ndev);
        netif_start_queue(ndev);
    } else {
        priv->can.state = CAN_STATE_STOPPED;
        if (mttcan_set_working_mode(ndev) != 0) {
            mttcan_print_runlog(priv, CAN_LOG_PM_RESUME_FAIL, "mttcan_set_working_mode failed.\n");
            return -1;
        }
    }
    mttcan_print_runlog(priv, CAN_LOG_PM_RESUME_SUCC, "mttcan has resumed.\n");
    MTTCAN_BOOTDOT(CAN_RESUME_EXPECT);

    return 0;
}
STATIC void mttcan_stop_work(struct net_device *ndev)
{
    int ret;
    if (netif_running(ndev)) {
        netif_stop_queue(ndev);
        netif_device_detach(ndev);
        /* wait time at most WAIT_FOR_TX_HANDLE_US + 100 us */
        usleep_range(WAIT_FOR_TX_HANDLE_US, WAIT_FOR_TX_HANDLE_US + 100);
        ret = mttcan_wait_clock_stop_ack(ndev);
        if (ret < 0) {
            mttcan_warn("%s: mttcan wait clock stop timeout,ignore it and force to stop device.\n",
                ndev->name);
        }
        mttcan_stop(ndev);
        close_candev(ndev);
    }
    mttcan_unbind_irq(ndev);
}

/**
* plat_drv_suspend - Power Down or sleep mode.
* brief: according to MTTCAN user manual v330 3.1.8 chapter.
*
* In case of a heavily disturbed CAN bus, it may happen that idle state is
* never reached and CCCR.INIT is therefore not set by the M_TTCAN. This
* situation can be detected by polling PSR.ACT. In case the M_CAN does
* not enter idle state, the software can write CCCR.INIT = 1 which stops
* CAN communication of the M_TTCAN immediately, regardless whether there
* is a transmission/reception ongoing or not.
*/
STATIC int plat_drv_suspend(struct device *dev)
{
    struct net_device *ndev = NULL;
    struct mttcan_priv *priv = NULL;

    MTTCAN_BOOTDOT_INIT(SNAPSHOT_STATUS_SUSPEND, CAN_SUSPEND_EXPECT);
    MTTCAN_BOOTDOT(CAN_SUSPEND_START);
    ndev = dev_get_drvdata(dev);
    priv = netdev_priv(ndev);
    MTTCAN_BOOTDOT(CAN_SUSPEND_STOP_WORK);
    mttcan_stop_work(ndev);
    MTTCAN_BOOTDOT(CAN_SUSPEND_RESET_KFIFO);
    mttcan_reset_kfifo(priv);
    priv->can.state = CAN_STATE_SLEEPING;
    mttcan_print_runlog(priv, CAN_LOG_PM_SUSPEND, "mttcan is suspended.\n");
    MTTCAN_BOOTDOT(CAN_SUSPEND_EXPECT);

    return 0;
}
STATIC void plat_drv_shutdown(struct platform_device *pdev)
{
    struct net_device *ndev = platform_get_drvdata(pdev);
    struct mttcan_priv *priv = netdev_priv(ndev);

    mttcan_stop_work(ndev);
    priv->can.state = CAN_STATE_STOPPED;
    mttcan_info("%s:mttcan is shutdown.\n", ndev->name);
}
/*
 * aos not support freeze thaw poweroff restore
 */
static const struct dev_pm_ops can_drv_pm_ops = {
    .suspend = plat_drv_suspend,
    .resume = plat_drv_resume,
#ifndef RUN_IN_AOS
    .freeze = plat_drv_suspend,
    .thaw = plat_drv_resume,
    .poweroff = plat_drv_suspend,
    .restore = plat_drv_resume,
#endif
};

static const struct of_device_id can_drv_of_table[] = {
    { .compatible = "hisi,mttcan", .data = NULL},
    {},
};
#ifndef RUN_IN_AOS
MODULE_DEVICE_TABLE(of, can_drv_of_table);
#endif

static struct platform_driver platform_can_driver = {
    .driver = {
        .name = KBUILD_MODNAME,
#ifndef RUN_IN_AOS
        .owner = THIS_MODULE,
#endif
        .of_match_table = can_drv_of_table,
        .pm = &can_drv_pm_ops,
    },
    .probe = plat_drv_probe,
    .remove = plat_drv_remove,
    .shutdown = plat_drv_shutdown,
};

module_platform_driver(platform_can_driver);
#ifndef RUN_IN_AOS
MODULE_AUTHOR("Huawei Technologies Co., Ltd.");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CAN Driver for M_TTCAN Controller");
#endif
