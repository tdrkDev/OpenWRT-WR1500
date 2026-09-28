// SPDX-License-Identifier: GPL-2.0-or-later

/* PCIe host controller driver for Realtek SoCs */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/ratelimit.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include "../pci.h"

/* RTL8197F system controller: PERST# of the PCIE_RSTN pin */
#define RTL8197F_SYS_ENABLE	0x50
#define RTL8197F_PCIE_PERST_N	BIT(1)

/* Host config register offsets */
#define RTPCIE_HOSTCFG_CAP	0x70
#define RTPCIE_HOSTCFG_ENABLE	0x80c
#define RTPCIE_ENABLE_BIT17	BIT(17)
#define RTPCIE_LINK_STATUS	0x728
#define RTPCIE_LTSSM		GENMASK(4, 0)
#define RTPCIE_LTSSM_L0		0x11
#define RTPCIE_LTSSM_L1_IDLE	0x14
#define RTPCIE_PCI_CMD_BIT20	BIT(20)

#define RTPCIE_DEVSTA_ERR	(PCI_EXP_DEVSTA_CED | PCI_EXP_DEVSTA_NFED | \
				 PCI_EXP_DEVSTA_FED | PCI_EXP_DEVSTA_URD)
#define RTPCIE_LINK_DOWN_CHECKS	3

static bool panic_on_link_down = true;
module_param(panic_on_link_down, bool, 0644);
MODULE_PARM_DESC(panic_on_link_down, "Panic when the link stays down (default: true)");

/**
 * struct rtpcie_soc_data - SoC specific details
 * @sysctl_perst: PERST# is driven by the system controller, not a GPIO
 * @speed_change: start a speed change after the link is up
 * @link_check: check the link and the error status once a second
 */
struct rtpcie_soc_data {
	bool sysctl_perst;
	bool speed_change;
	bool link_check;
};

/**
 * struct rtpcie_ctrl - Realtek PCIe port information
 * @dev: pointer to PCIe device
 * @soc: SoC specific details
 * @hostcfg_base: host config register base
 * @hostext_base: host extension register base
 * @devcfg_base: device config register base
 * @reset_gpio: gpio reset
 * @sysctl: system controller, for PERST# when @soc->sysctl_perst is set
 * @phy: pointer to PHY control block
 * @bus_number: assigned PCI bus number
 * @bus: root bus
 * @link_work: link check
 * @link_down: number of consecutive checks that found the link down
 */
struct rtpcie_ctrl {
	struct device *dev;
	const struct rtpcie_soc_data *soc;
	void __iomem *hostcfg_base;
	void __iomem *hostext_base;
	void __iomem *devcfg_base;
	struct gpio_desc *reset_gpio;
	struct regmap *sysctl;
	struct phy *phy;
	u8 bus_number;
	struct pci_bus *bus;
	struct delayed_work link_work;
	unsigned int link_down;
};

static void __iomem *rtpcie_map_bus(struct pci_bus *bus, unsigned int devfn, int where)
{
	struct rtpcie_ctrl *pcie = bus->sysdata;

	if (pcie->bus_number == 0xff)
		pcie->bus_number = bus->number;

	if (bus->number != pcie->bus_number)
		return NULL;

	writel(PCI_FUNC(devfn), pcie->hostext_base);

	switch (PCI_SLOT(devfn)) {
	case 0:
		return pcie->hostcfg_base + where;
	case 1:
		return pcie->devcfg_base + where;
	default:
		return NULL;
	}
}

static struct pci_ops rtpcie_ops = {
	.map_bus = rtpcie_map_bus,
	.read = pci_generic_config_read,
	.write = pci_generic_config_write,
};

static void rtpcie_perst(struct rtpcie_ctrl *pcie, bool assert)
{
	if (pcie->soc->sysctl_perst)
		regmap_update_bits(pcie->sysctl, RTL8197F_SYS_ENABLE,
				   RTL8197F_PCIE_PERST_N,
				   assert ? 0 : RTL8197F_PCIE_PERST_N);
	else
		gpiod_set_value_cansleep(pcie->reset_gpio, assert);
}

static int rtpcie_hw_init(struct rtpcie_ctrl *pcie)
{
	u16 devctl, link;
	int err;
	u32 val;

	/* Assert reset pcie */
	rtpcie_perst(pcie, true);

	err = phy_init(pcie->phy);
	if (err) {
		dev_err(pcie->dev, "failed to initialize pcie-phy\n");
		return err;
	}

	/* The system controller PERST# has only been asserted just now */
	if (pcie->soc->sysctl_perst)
		msleep(100);

	/* Deassert reset pcie */
	rtpcie_perst(pcie, false);

	/* Wait for Link Up */
	err = readl_poll_timeout(pcie->hostcfg_base + RTPCIE_LINK_STATUS, val,
				 FIELD_GET(RTPCIE_LTSSM, val) == RTPCIE_LTSSM_L0,
				 10000, 100000);

	if (err) {
		dev_err(pcie->dev, "PCIE Link Up Failed!\n");
		phy_exit(pcie->phy);
		return err;
	}

	msleep(100);

	/* Enable PCIE host */
	val = RTPCIE_PCI_CMD_BIT20 | PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
	writel(val, pcie->hostcfg_base + PCI_COMMAND);

	/* Clear max payload size bits in DEVCTL */
	devctl = readw(pcie->hostcfg_base + RTPCIE_HOSTCFG_CAP + PCI_EXP_DEVCTL);
	devctl &= ~PCI_EXP_DEVCTL_PAYLOAD;
	writew(devctl, pcie->hostcfg_base + RTPCIE_HOSTCFG_CAP + PCI_EXP_DEVCTL);

	if (pcie->soc->speed_change) {
		val = readl(pcie->hostcfg_base + RTPCIE_HOSTCFG_ENABLE);
		writel(val | RTPCIE_ENABLE_BIT17,
		       pcie->hostcfg_base + RTPCIE_HOSTCFG_ENABLE);

		usleep_range(1000, 2000);
	}

	link = readw(pcie->hostcfg_base + RTPCIE_HOSTCFG_CAP + PCI_EXP_LNKSTA);
	dev_info(pcie->dev, "link up, %s\n",
		 pci_speed_string(pcie_link_speed[FIELD_GET(PCI_EXP_LNKSTA_CLS, link)]));

	return err;
}

/* The error bits of the root port are write-one-to-clear */
static u16 rtpcie_clear_errors(struct rtpcie_ctrl *pcie)
{
	u16 devsta;

	pci_bus_read_config_word(pcie->bus, 0,
				 RTPCIE_HOSTCFG_CAP + PCI_EXP_DEVSTA, &devsta);
	devsta &= RTPCIE_DEVSTA_ERR;
	if (devsta)
		pci_bus_write_config_word(pcie->bus, 0,
					  RTPCIE_HOSTCFG_CAP + PCI_EXP_DEVSTA,
					  devsta);

	return devsta;
}

/*
 * Any access to the device after the link went down stalls the SoC bus for
 * good. Nothing can catch that access, but a link that goes down while the
 * device is idle can end in a panic that logs why the system restarts.
 */
static void rtpcie_link_check(struct work_struct *work)
{
	struct rtpcie_ctrl *pcie = container_of(to_delayed_work(work),
						struct rtpcie_ctrl, link_work);
	static DEFINE_RATELIMIT_STATE(err_rs, 60 * HZ, 1);
	u32 ltssm;
	u16 devsta;

	/* Config reads of absent functions end in unsupported requests */
	devsta = rtpcie_clear_errors(pcie) & ~PCI_EXP_DEVSTA_URD;
	if (devsta && __ratelimit(&err_rs))
		dev_warn(pcie->dev, "error status %#x\n", devsta);

	pci_bus_read_config_dword(pcie->bus, 0, RTPCIE_LINK_STATUS, &ltssm);
	ltssm = FIELD_GET(RTPCIE_LTSSM, ltssm);

	/* Between L0 and L1 the link is up */
	if (ltssm >= RTPCIE_LTSSM_L0 && ltssm <= RTPCIE_LTSSM_L1_IDLE) {
		if (pcie->link_down >= RTPCIE_LINK_DOWN_CHECKS)
			dev_info(pcie->dev, "link up\n");
		pcie->link_down = 0;
	} else if (++pcie->link_down == RTPCIE_LINK_DOWN_CHECKS) {
		if (panic_on_link_down)
			panic("%s: link down, LTSSM %#x\n",
			      dev_name(pcie->dev), ltssm);
		dev_err(pcie->dev, "link down, LTSSM %#x\n", ltssm);
	}

	schedule_delayed_work(&pcie->link_work, round_jiffies_relative(HZ));
}

static int rtpcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtpcie_ctrl *pcie;
	struct pci_host_bridge *bridge;
	int ret;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!bridge)
		return -ENOMEM;

	pcie = pci_host_bridge_priv(bridge);
	pcie->dev = dev;
	pcie->soc = of_device_get_match_data(dev);
	pcie->bus_number = 0xff;
	INIT_DELAYED_WORK(&pcie->link_work, rtpcie_link_check);
	platform_set_drvdata(pdev, pcie);

	pcie->hostcfg_base = devm_platform_ioremap_resource_byname(pdev, "hostcfg");
	if (IS_ERR(pcie->hostcfg_base))
		return dev_err_probe(dev, PTR_ERR(pcie->hostcfg_base),
				     "failed to map hostcfg\n");

	pcie->hostext_base = devm_platform_ioremap_resource_byname(pdev, "hostext");
	if (IS_ERR(pcie->hostext_base))
		return dev_err_probe(dev, PTR_ERR(pcie->hostext_base),
				     "failed to map hostext\n");

	pcie->devcfg_base = devm_platform_ioremap_resource_byname(pdev, "devcfg");
	if (IS_ERR(pcie->devcfg_base))
		return dev_err_probe(dev, PTR_ERR(pcie->devcfg_base),
				     "failed to map devcfg\n");

	if (pcie->soc->sysctl_perst) {
		pcie->sysctl = syscon_regmap_lookup_by_phandle(dev->of_node,
							       "realtek,sysctl");
		if (IS_ERR(pcie->sysctl))
			return dev_err_probe(dev, PTR_ERR(pcie->sysctl),
					     "failed to get system controller\n");
	} else {
		pcie->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
		if (IS_ERR(pcie->reset_gpio))
			return dev_err_probe(dev, PTR_ERR(pcie->reset_gpio),
					     "failed to get reset GPIO\n");
	}

	pcie->phy = devm_of_phy_get(dev, dev->of_node, NULL);
	if (IS_ERR(pcie->phy))
		return dev_err_probe(dev, PTR_ERR(pcie->phy), "failed to get pcie-phy");

	ret = rtpcie_hw_init(pcie);
	if (ret)
		return ret;

	bridge->sysdata = pcie;
	bridge->ops = &rtpcie_ops;

	ret = pci_host_probe(bridge);
	if (ret) {
		phy_exit(pcie->phy);
		return ret;
	}

	pcie->bus = bridge->bus;

	if (pcie->soc->link_check) {
		/* Start with the errors of link training and enumeration cleared */
		rtpcie_clear_errors(pcie);
		schedule_delayed_work(&pcie->link_work, HZ);
	}

	return 0;
}

static void rtpcie_shutdown(struct platform_device *pdev)
{
	struct rtpcie_ctrl *pcie = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&pcie->link_work);
}

static const struct rtpcie_soc_data rtl8197f_pcie_data = {
	.sysctl_perst = true,
	.link_check = true,
};

static const struct rtpcie_soc_data rtl9607c_pcie_data = {
	.speed_change = true,
};

static const struct of_device_id rtpcie_of_match[] = {
	{ .compatible = "realtek,rtl8197f-pcie", .data = &rtl8197f_pcie_data },
	{ .compatible = "realtek,rtl9607c-pcie", .data = &rtl9607c_pcie_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rtpcie_of_match);

static struct platform_driver rtpcie_driver = {
	.probe = rtpcie_probe,
	.shutdown = rtpcie_shutdown,
	.driver = {
		.name = "realtek-pcie",
		.of_match_table = rtpcie_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rtpcie_driver);

MODULE_DESCRIPTION("PCIe host controller driver for Realtek SoC");
MODULE_LICENSE("GPL");
