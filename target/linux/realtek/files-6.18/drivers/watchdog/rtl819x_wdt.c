// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL819x watchdog driver
 *
 * The watchdog is part of the timer block of the RTL819x router SoCs. It counts
 * a clock divided from the peripheral bus clock and resets the SoC when the
 * count reaches 2^(15 + OVSEL). The divider is shared with the timers of the
 * block, which Linux does not use, and the control register with the restart
 * handler, which reboots the SoC through the watchdog.
 */

#include <linux/bitfield.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/watchdog.h>

#define RTL819X_WDT_CDBR		0x18
#define   RTL819X_WDT_CDBR_DIVF		GENMASK(31, 16)
#define RTL819X_WDT_CNR			0x1c
#define   RTL819X_WDT_CNR_WDTE		GENMASK(31, 24)
#define   RTL819X_WDT_CNR_WDTE_STOP	0xa5
#define   RTL819X_WDT_CNR_CLR		BIT(23)
#define   RTL819X_WDT_CNR_OVSEL_LO	GENMASK(22, 21)
#define   RTL819X_WDT_CNR_OVSEL_HI	GENMASK(18, 17)

/*
 * Count at 500 kHz with the largest overflow select: 2^24 counts, a reset
 * 33.5 s after the last ping. That leaves the soft lockup detector (20 s)
 * time to report, or panic on, a CPU that is stuck in the kernel.
 */
#define RTL819X_WDT_RATE		500000
#define RTL819X_WDT_OVSEL		9

struct rtl819x_wdt {
	struct watchdog_device wdd;
	struct regmap *map;
};

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

/*
 * Always write the whole register with the same overflow select: a smaller
 * one could let the running counter overflow at once, and bit 20 (watchdog
 * reset indicator) is write-one-to-clear.
 */
static int rtl819x_wdt_write(struct rtl819x_wdt *wdt, u32 val)
{
	val |= FIELD_PREP(RTL819X_WDT_CNR_OVSEL_LO, RTL819X_WDT_OVSEL & 0x3) |
	       FIELD_PREP(RTL819X_WDT_CNR_OVSEL_HI, RTL819X_WDT_OVSEL >> 2);

	return regmap_write(wdt->map, RTL819X_WDT_CNR, val);
}

/* Any other value than the stop pattern in WDTE runs the watchdog */
static int rtl819x_wdt_ping(struct watchdog_device *wdd)
{
	struct rtl819x_wdt *wdt = watchdog_get_drvdata(wdd);

	return rtl819x_wdt_write(wdt, RTL819X_WDT_CNR_CLR);
}

static int rtl819x_wdt_stop(struct watchdog_device *wdd)
{
	struct rtl819x_wdt *wdt = watchdog_get_drvdata(wdd);
	int ret;

	/* The stop pattern alone may reset the SoC, clear the counter first */
	ret = rtl819x_wdt_ping(wdd);
	if (ret)
		return ret;

	return rtl819x_wdt_write(wdt, FIELD_PREP(RTL819X_WDT_CNR_WDTE,
						 RTL819X_WDT_CNR_WDTE_STOP));
}

/*
 * The overflow select stays fixed: the core pings the watchdog for longer
 * timeouts, and shorter ones take the whole period.
 */
static int rtl819x_wdt_set_timeout(struct watchdog_device *wdd,
				   unsigned int timeout)
{
	unsigned int period;

	period = DIV_ROUND_UP(wdd->max_hw_heartbeat_ms, MSEC_PER_SEC);
	wdd->timeout = max(timeout, period);

	return 0;
}

static const struct watchdog_info rtl819x_wdt_info = {
	.identity = "Realtek RTL819x watchdog",
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops rtl819x_wdt_ops = {
	.owner = THIS_MODULE,
	.start = rtl819x_wdt_ping,
	.stop = rtl819x_wdt_stop,
	.ping = rtl819x_wdt_ping,
	.set_timeout = rtl819x_wdt_set_timeout,
};

static int rtl819x_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct watchdog_device *wdd;
	struct rtl819x_wdt *wdt;
	u32 bus_rate, divf;
	u64 period_ms;
	int ret;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->map = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(wdt->map))
		return dev_err_probe(dev, PTR_ERR(wdt->map),
				     "failed to get timer registers\n");

	ret = device_property_read_u32(dev, "clock-frequency", &bus_rate);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clock-frequency\n");

	/* Dividers of 0 and 1 turn the clock off */
	divf = DIV_ROUND_UP(bus_rate, RTL819X_WDT_RATE);
	if (divf < 2 || divf > FIELD_MAX(RTL819X_WDT_CDBR_DIVF))
		return dev_err_probe(dev, -EINVAL,
				     "unsupported clock-frequency %u\n", bus_rate);

	ret = regmap_write(wdt->map, RTL819X_WDT_CDBR,
			   FIELD_PREP(RTL819X_WDT_CDBR_DIVF, divf));
	if (ret)
		return ret;

	period_ms = div_u64(((u64)MSEC_PER_SEC * divf) << (15 + RTL819X_WDT_OVSEL),
			    bus_rate);

	wdd = &wdt->wdd;
	wdd->info = &rtl819x_wdt_info;
	wdd->ops = &rtl819x_wdt_ops;
	wdd->parent = dev;
	wdd->min_timeout = 1;
	wdd->max_hw_heartbeat_ms = period_ms;
	wdd->timeout = DIV_ROUND_UP(wdd->max_hw_heartbeat_ms, MSEC_PER_SEC);
	watchdog_set_nowayout(wdd, nowayout);
	watchdog_set_drvdata(wdd, wdt);

	return devm_watchdog_register_device(dev, wdd);
}

static const struct of_device_id rtl819x_wdt_of_match[] = {
	{ .compatible = "realtek,rtl8197f-wdt" },
	{}
};
MODULE_DEVICE_TABLE(of, rtl819x_wdt_of_match);

static struct platform_driver rtl819x_wdt_driver = {
	.driver = {
		.name = "rtl819x-wdt",
		.of_match_table = rtl819x_wdt_of_match,
	},
	.probe = rtl819x_wdt_probe,
};
module_platform_driver(rtl819x_wdt_driver);

MODULE_DESCRIPTION("Realtek RTL819x watchdog driver");
MODULE_LICENSE("GPL");
