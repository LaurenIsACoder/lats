/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/smp.h>

#if __has_include(<asm/loongarchregs.h>)
#include <asm/loongarchregs.h>
#else
#include <asm/loongarch.h>
#endif

static void disable_stfill(void *failed)
{
    unsigned long *failed_mask = failed;
    uint64_t tmp;

    tmp = read_csr_impctl1();
    tmp &= ~CSR_STFILL;
    write_csr_impctl1(tmp);

    if (tmp != read_csr_impctl1()) {
        set_bit(smp_processor_id(), failed_mask);
    }

    pr_info("hamt-pm csr[%d] %llx\n", smp_processor_id(), tmp);
}

static void enable_stfill(void *failed)
{
    unsigned long *failed_mask = failed;
    uint64_t tmp;

    tmp = read_csr_impctl1();
    tmp |= CSR_STFILL;
    write_csr_impctl1(tmp);

    if (tmp != read_csr_impctl1()) {
        set_bit(smp_processor_id(), failed_mask);
    }

    pr_info("hamt-pm csr[%d] %llx\n", smp_processor_id(), tmp);
}

static int hamt_pm_suspend(struct device *dev)
{
    pr_info("hamt-pm driver suspended\n");
    return 0;
}

static int hamt_pm_resume(struct device *dev)
{
    unsigned long failed = 0;

    on_each_cpu(disable_stfill, &failed, 1);
    if (failed) {
        pr_err("hamt-pm failed to disable stfill on CPU mask %#lx\n", failed);
        return -EIO;
    }

    pr_info("hamt-pm driver resumed\n");
    return 0;
}

static const struct dev_pm_ops hamt_pm_ops = {
    SET_SYSTEM_SLEEP_PM_OPS(hamt_pm_suspend, hamt_pm_resume)
};

static int hamt_pm_probe(struct platform_device *pdev)
{
    pr_info("hamt-pm device probed\n");
    return 0;
}

static int hamt_pm_remove(struct platform_device *pdev)
{
    pr_info("hamt-pm device removed\n");
    return 0;
}

static struct platform_driver hamt_pm_driver = {
    .probe = hamt_pm_probe,
    .remove = hamt_pm_remove,
    .driver = {
        .name = "hamt-pm",
        .pm = &hamt_pm_ops,
        .owner = THIS_MODULE,
    },
};

static struct platform_device *hamt_pm_device;

static int __init hamt_pm_init(void)
{
    unsigned long failed = 0;
    int ret;

    ret = platform_driver_register(&hamt_pm_driver);
    if (ret) {
        pr_err("failed to register hamt-pm driver\n");
        return ret;
    }

    hamt_pm_device = platform_device_register_simple("hamt-pm", -1, NULL, 0);
    if (IS_ERR(hamt_pm_device)) {
        pr_err("failed to register hamt-pm device\n");
        platform_driver_unregister(&hamt_pm_driver);
        return PTR_ERR(hamt_pm_device);
    }

    on_each_cpu(disable_stfill, &failed, 1);
    if (failed) {
        pr_err("hamt-pm failed to disable stfill on CPU mask %#lx\n", failed);
        platform_device_unregister(hamt_pm_device);
        platform_driver_unregister(&hamt_pm_driver);
        return -EIO;
    }

    pr_info("hamt-pm module initialized\n");
    return 0;
}

static void __exit hamt_pm_exit(void)
{
    unsigned long failed = 0;

    platform_device_unregister(hamt_pm_device);
    platform_driver_unregister(&hamt_pm_driver);

    on_each_cpu(enable_stfill, &failed, 1);
    if (failed) {
        pr_err("hamt-pm failed to enable stfill on CPU mask %#lx\n", failed);
    }

    pr_info("hamt-pm module exited\n");
}

module_init(hamt_pm_init);
module_exit(hamt_pm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NiuGenen");
MODULE_DESCRIPTION("Disable LoongArch store fill buffer while HAMT is in use");
