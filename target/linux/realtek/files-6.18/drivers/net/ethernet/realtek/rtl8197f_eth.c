// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL8197F switch core CPU port Ethernet driver
 *
 * The RTL8197F contains an rtl865x style switch core. Its port 0 is an RGMII
 * MAC that connects to an external switch. This driver only handles the CPU
 * interface (NIC) of the switch core: frames are sent directly to port 0 and
 * the switch core forwards frames from port 0 to the CPU. The switch core
 * itself is expected to be set up by the boot code, which always does so.
 */

#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>

/* CPU interface registers */
#define NIC_CPUICR			0x000
#define   NIC_CPUICR_TXCMD		BIT(31)
#define   NIC_CPUICR_RXCMD		BIT(30)
#define   NIC_CPUICR_BURST		GENMASK(29, 28)
#define   NIC_CPUICR_BURST_128W		2
#define   NIC_CPUICR_MBUF		GENMASK(26, 24)
#define   NIC_CPUICR_MBUF_2048		4
#define   NIC_CPUICR_TXFD		BIT(23)
#define NIC_CPURPDCR(n)			(0x004 + 4 * (n))
#define NIC_CPUTPDCR0			0x020
#define NIC_CPUIIMR			0x028
#define NIC_CPUIISR			0x02c
#define   NIC_INT_RUNOUT0		BIT(17)
#define   NIC_INT_TX_DONE0		BIT(9)
#define   NIC_INT_RX_DONE0		BIT(3)
#define NIC_CPUQDM(n)			(0x030 + 4 * (n))
#define NIC_DMA_CR0			0x03c
#define   NIC_DMA_CR0_LOW_MARK		GENMASK(15, 8)
#define   NIC_DMA_CR0_HIGH_MARK		GENMASK(7, 0)
#define NIC_DMA_CR1			0x040
#define   NIC_DMA_CR1_TX0_MAXLEN	GENMASK(15, 0)
#define NIC_DMA_CR2			0x044
#define   NIC_DMA_CR2_RX_MAXLEN		GENMASK(15, 0)
#define NIC_TXRINGCR			0x078
#define   NIC_TXRINGCR_RING1_EN		BIT(1)
#define   NIC_TXRINGCR_RING0_EN		BIT(0)
#define NIC_DMA_CR4			0x0a0
#define   NIC_DMA_CR4_RX_TAIL		BIT(8)
#define   NIC_DMA_CR4_TX0_TAIL		BIT(0)
#define NIC_CPUICR1			0x0a4
#define   NIC_CPUICR1_RXDSC_SIZE	GENMASK(23, 18)
#define   NIC_CPUICR1_TXDSC_SIZE	GENMASK(17, 12)
#define   NIC_CPUICR1_HDR_TYPE		GENMASK(9, 8)
#define   NIC_CPUICR1_HDR_TYPE_NEW	1
#define   NIC_CPUICR1_LITTLE_ENDIAN	BIT(1)

/* Switch core registers */
#define SW_SIRR				0x4204
#define   SW_SIRR_TRXRDY		BIT(0)
#define SW_VCR0				0x4a00
#define   SW_VCR0_1Q_VID_IGNORE		BIT(31)

/* Switch core tables */
#define SW_TBL_ACL			0x0c0000

/* Descriptor word 0, common to RX and TX */
#define DESC_OWN			BIT(0)	/* owned by the switch core */
#define DESC_EOR			BIT(1)	/* last descriptor of the ring */
#define DESC_LS				BIT(2)
#define DESC_FS				BIT(3)
#define TXD0_LEN			GENMASK(22, 6)
#define TXD2_MLEN			GENMASK(31, 15)
#define TXD4_DP				GENMASK(30, 24)
#define RXD0_BUFSIZE			GENMASK(31, 16)
#define RXD2_LEN			GENMASK(13, 0)

#define RTL8197F_ETH_DESC_WORDS		6
#define RTL8197F_ETH_RX_RING		128
#define RTL8197F_ETH_TX_RING		128
#define RTL8197F_ETH_RX_BUF		2048
#define RTL8197F_ETH_TX_PORTS		BIT(0)	/* port 0: RGMII */
#define RTL8197F_ETH_FIFO_LOW		0xa0
#define RTL8197F_ETH_FIFO_HIGH		0xce

struct rtl8197f_eth_desc {
	u32 w[RTL8197F_ETH_DESC_WORDS];
};

struct rtl8197f_eth {
	struct net_device *ndev;
	struct device *dev;
	void __iomem *nic;
	void __iomem *swcore;
	void __iomem *tables;
	struct napi_struct napi;

	struct rtl8197f_eth_desc *rx_ring;
	dma_addr_t rx_ring_dma;
	struct sk_buff *rx_skb[RTL8197F_ETH_RX_RING];
	dma_addr_t rx_dma[RTL8197F_ETH_RX_RING];
	unsigned int rx_idx;

	struct rtl8197f_eth_desc *tx_ring;
	dma_addr_t tx_ring_dma;
	struct sk_buff *tx_skb[RTL8197F_ETH_TX_RING];
	dma_addr_t tx_dma[RTL8197F_ETH_TX_RING];
	unsigned int tx_head;
	unsigned int tx_tail;
};

static u32 nic_r32(struct rtl8197f_eth *eth, unsigned int reg)
{
	return readl(eth->nic + reg);
}

static void nic_w32(struct rtl8197f_eth *eth, unsigned int reg, u32 val)
{
	writel(val, eth->nic + reg);
}

static void nic_rmw(struct rtl8197f_eth *eth, unsigned int reg, u32 clr, u32 set)
{
	nic_w32(eth, reg, (nic_r32(eth, reg) & ~clr) | set);
}

static unsigned int rtl8197f_eth_tx_free(struct rtl8197f_eth *eth)
{
	return RTL8197F_ETH_TX_RING - 1 - (eth->tx_head - eth->tx_tail);
}

static void rtl8197f_eth_rx_give(struct rtl8197f_eth *eth, unsigned int idx)
{
	struct rtl8197f_eth_desc *desc = &eth->rx_ring[idx];
	u32 w0 = DESC_OWN | FIELD_PREP(RXD0_BUFSIZE, RTL8197F_ETH_RX_BUF);

	if (idx == RTL8197F_ETH_RX_RING - 1)
		w0 |= DESC_EOR;

	desc->w[1] = eth->rx_dma[idx];
	desc->w[2] = 0;
	dma_wmb();
	desc->w[0] = w0;
}

static int rtl8197f_eth_rx_alloc(struct rtl8197f_eth *eth, unsigned int idx)
{
	struct sk_buff *skb;
	dma_addr_t dma;

	skb = netdev_alloc_skb_ip_align(eth->ndev, RTL8197F_ETH_RX_BUF);
	if (!skb)
		return -ENOMEM;

	dma = dma_map_single(eth->dev, skb->data, RTL8197F_ETH_RX_BUF,
			     DMA_FROM_DEVICE);
	if (dma_mapping_error(eth->dev, dma)) {
		dev_kfree_skb_any(skb);
		return -ENOMEM;
	}

	eth->rx_skb[idx] = skb;
	eth->rx_dma[idx] = dma;

	return 0;
}

static int rtl8197f_eth_rx(struct rtl8197f_eth *eth, int budget)
{
	struct net_device *ndev = eth->ndev;
	int done = 0;

	while (done < budget) {
		unsigned int idx = eth->rx_idx;
		struct rtl8197f_eth_desc *desc = &eth->rx_ring[idx];
		struct sk_buff *skb;
		dma_addr_t dma;
		unsigned int len;

		if (READ_ONCE(desc->w[0]) & DESC_OWN)
			break;

		dma_rmb();
		len = FIELD_GET(RXD2_LEN, desc->w[2]);
		skb = eth->rx_skb[idx];
		dma = eth->rx_dma[idx];

		/* The length includes the FCS */
		if (len < ETH_HLEN + ETH_FCS_LEN || len > RTL8197F_ETH_RX_BUF ||
		    rtl8197f_eth_rx_alloc(eth, idx)) {
			ndev->stats.rx_dropped++;
			goto give;
		}

		dma_unmap_single(eth->dev, dma, RTL8197F_ETH_RX_BUF,
				 DMA_FROM_DEVICE);
		skb_put(skb, len - ETH_FCS_LEN);
		skb->protocol = eth_type_trans(skb, ndev);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += skb->len;
		napi_gro_receive(&eth->napi, skb);

give:
		rtl8197f_eth_rx_give(eth, idx);
		eth->rx_idx = (idx + 1) % RTL8197F_ETH_RX_RING;
		done++;
	}

	return done;
}

static void rtl8197f_eth_tx_reclaim(struct rtl8197f_eth *eth)
{
	struct net_device *ndev = eth->ndev;
	unsigned int bytes = 0, pkts = 0;

	while (eth->tx_tail != eth->tx_head) {
		unsigned int idx = eth->tx_tail % RTL8197F_ETH_TX_RING;
		struct sk_buff *skb = eth->tx_skb[idx];

		if (READ_ONCE(eth->tx_ring[idx].w[0]) & DESC_OWN)
			break;

		dma_unmap_single(eth->dev, eth->tx_dma[idx], skb->len,
				 DMA_TO_DEVICE);
		bytes += skb->len;
		pkts++;
		napi_consume_skb(skb, 1);
		eth->tx_skb[idx] = NULL;
		eth->tx_tail++;
	}

	ndev->stats.tx_packets += pkts;
	ndev->stats.tx_bytes += bytes;
	netdev_completed_queue(ndev, pkts, bytes);

	if (netif_queue_stopped(ndev) &&
	    rtl8197f_eth_tx_free(eth) > MAX_SKB_FRAGS)
		netif_wake_queue(ndev);
}

static int rtl8197f_eth_poll(struct napi_struct *napi, int budget)
{
	struct rtl8197f_eth *eth = container_of(napi, struct rtl8197f_eth, napi);
	int done;

	rtl8197f_eth_tx_reclaim(eth);
	done = rtl8197f_eth_rx(eth, budget);

	if (done < budget && napi_complete_done(napi, done))
		nic_w32(eth, NIC_CPUIIMR, NIC_INT_RX_DONE0 | NIC_INT_RUNOUT0 |
					  NIC_INT_TX_DONE0);

	return done;
}

static irqreturn_t rtl8197f_eth_irq(int irq, void *data)
{
	struct rtl8197f_eth *eth = data;
	u32 status;

	status = nic_r32(eth, NIC_CPUIISR) & nic_r32(eth, NIC_CPUIIMR);
	if (!status)
		return IRQ_NONE;

	nic_w32(eth, NIC_CPUIIMR, 0);
	nic_w32(eth, NIC_CPUIISR, status);
	napi_schedule(&eth->napi);

	return IRQ_HANDLED;
}

static netdev_tx_t rtl8197f_eth_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct rtl8197f_eth *eth = netdev_priv(ndev);
	struct rtl8197f_eth_desc *desc;
	unsigned int idx, len;
	dma_addr_t dma;
	u32 w0;

	if (skb_put_padto(skb, ETH_ZLEN)) {
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	if (!rtl8197f_eth_tx_free(eth)) {
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}

	dma = dma_map_single(eth->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(eth->dev, dma)) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	idx = eth->tx_head % RTL8197F_ETH_TX_RING;
	desc = &eth->tx_ring[idx];
	eth->tx_skb[idx] = skb;
	eth->tx_dma[idx] = dma;

	/* The switch core appends the FCS, the length includes it */
	len = skb->len + ETH_FCS_LEN;
	w0 = DESC_FS | DESC_LS | FIELD_PREP(TXD0_LEN, len);
	if (idx == RTL8197F_ETH_TX_RING - 1)
		w0 |= DESC_EOR;

	desc->w[1] = dma;
	desc->w[2] = FIELD_PREP(TXD2_MLEN, len);
	desc->w[3] = 0;
	desc->w[4] = FIELD_PREP(TXD4_DP, RTL8197F_ETH_TX_PORTS);
	desc->w[5] = 0;
	dma_wmb();
	desc->w[0] = w0 | DESC_OWN;

	netdev_sent_queue(ndev, skb->len);
	eth->tx_head++;
	if (rtl8197f_eth_tx_free(eth) <= MAX_SKB_FRAGS)
		netif_stop_queue(ndev);

	wmb();
	nic_rmw(eth, NIC_CPUICR, 0, NIC_CPUICR_TXFD);

	return NETDEV_TX_OK;
}

static void rtl8197f_eth_hw_stop(struct rtl8197f_eth *eth)
{
	nic_w32(eth, NIC_CPUIIMR, 0);
	nic_w32(eth, NIC_CPUICR, 0);
	nic_w32(eth, NIC_CPUIISR, nic_r32(eth, NIC_CPUIISR));
}

static void rtl8197f_eth_hw_start(struct rtl8197f_eth *eth)
{
	u32 ring_len = (RTL8197F_ETH_RX_RING - 1) * sizeof(struct rtl8197f_eth_desc);
	int i;

	rtl8197f_eth_hw_stop(eth);

	nic_rmw(eth, NIC_CPUICR1, NIC_CPUICR1_HDR_TYPE,
		NIC_CPUICR1_LITTLE_ENDIAN |
		FIELD_PREP(NIC_CPUICR1_HDR_TYPE, NIC_CPUICR1_HDR_TYPE_NEW));

	/* Deliver all queues to RX ring 0 and use TX ring 0 only */
	for (i = 0; i < 3; i++)
		nic_w32(eth, NIC_CPUQDM(i), 0);
	nic_rmw(eth, NIC_TXRINGCR, NIC_TXRINGCR_RING1_EN, NIC_TXRINGCR_RING0_EN);

	nic_w32(eth, NIC_CPURPDCR(0), eth->rx_ring_dma);
	nic_w32(eth, NIC_CPUTPDCR0, eth->tx_ring_dma);
	nic_rmw(eth, NIC_DMA_CR1, NIC_DMA_CR1_TX0_MAXLEN,
		FIELD_PREP(NIC_DMA_CR1_TX0_MAXLEN,
			   (RTL8197F_ETH_TX_RING - 1) * sizeof(struct rtl8197f_eth_desc)));
	nic_rmw(eth, NIC_DMA_CR2, NIC_DMA_CR2_RX_MAXLEN,
		FIELD_PREP(NIC_DMA_CR2_RX_MAXLEN, ring_len));
	nic_w32(eth, NIC_DMA_CR4, NIC_DMA_CR4_RX_TAIL | NIC_DMA_CR4_TX0_TAIL);

	nic_w32(eth, NIC_CPUICR, NIC_CPUICR_TXCMD | NIC_CPUICR_RXCMD |
		FIELD_PREP(NIC_CPUICR_BURST, NIC_CPUICR_BURST_128W) |
		FIELD_PREP(NIC_CPUICR_MBUF, NIC_CPUICR_MBUF_2048));

	/* Writing CPUICR resets the FIFO marks, restore the vendor values */
	nic_rmw(eth, NIC_DMA_CR0, NIC_DMA_CR0_LOW_MARK | NIC_DMA_CR0_HIGH_MARK,
		FIELD_PREP(NIC_DMA_CR0_LOW_MARK, RTL8197F_ETH_FIFO_LOW) |
		FIELD_PREP(NIC_DMA_CR0_HIGH_MARK, RTL8197F_ETH_FIFO_HIGH));

	/* The vendor driver reads ACL entry 0 to make reception work */
	writel(readl(eth->tables + SW_TBL_ACL), eth->tables + SW_TBL_ACL);

	/*
	 * Frames from the external switch may carry 802.1Q tags that are
	 * unknown to the switch core VLAN table. Classify all frames by the
	 * port VLAN instead, so they pass unmodified.
	 */
	writel(readl(eth->swcore + SW_VCR0) | SW_VCR0_1Q_VID_IGNORE,
	       eth->swcore + SW_VCR0);

	writel(readl(eth->swcore + SW_SIRR) | SW_SIRR_TRXRDY, eth->swcore + SW_SIRR);

	nic_w32(eth, NIC_CPUIISR, nic_r32(eth, NIC_CPUIISR));
	nic_w32(eth, NIC_CPUIIMR, NIC_INT_RX_DONE0 | NIC_INT_RUNOUT0 |
				  NIC_INT_TX_DONE0);
}

static void rtl8197f_eth_free_rings(struct rtl8197f_eth *eth)
{
	int i;

	for (i = 0; i < RTL8197F_ETH_RX_RING; i++) {
		if (!eth->rx_skb[i])
			continue;
		dma_unmap_single(eth->dev, eth->rx_dma[i], RTL8197F_ETH_RX_BUF,
				 DMA_FROM_DEVICE);
		dev_kfree_skb_any(eth->rx_skb[i]);
		eth->rx_skb[i] = NULL;
	}

	for (i = 0; i < RTL8197F_ETH_TX_RING; i++) {
		if (!eth->tx_skb[i])
			continue;
		dma_unmap_single(eth->dev, eth->tx_dma[i], eth->tx_skb[i]->len,
				 DMA_TO_DEVICE);
		dev_kfree_skb_any(eth->tx_skb[i]);
		eth->tx_skb[i] = NULL;
	}

	if (eth->rx_ring)
		dma_free_coherent(eth->dev, RTL8197F_ETH_RX_RING * sizeof(*eth->rx_ring),
				  eth->rx_ring, eth->rx_ring_dma);
	if (eth->tx_ring)
		dma_free_coherent(eth->dev, RTL8197F_ETH_TX_RING * sizeof(*eth->tx_ring),
				  eth->tx_ring, eth->tx_ring_dma);
	eth->rx_ring = NULL;
	eth->tx_ring = NULL;
}

static int rtl8197f_eth_alloc_rings(struct rtl8197f_eth *eth)
{
	int i;

	eth->rx_ring = dma_alloc_coherent(eth->dev,
					  RTL8197F_ETH_RX_RING * sizeof(*eth->rx_ring),
					  &eth->rx_ring_dma, GFP_KERNEL);
	eth->tx_ring = dma_alloc_coherent(eth->dev,
					  RTL8197F_ETH_TX_RING * sizeof(*eth->tx_ring),
					  &eth->tx_ring_dma, GFP_KERNEL);
	if (!eth->rx_ring || !eth->tx_ring)
		goto err;

	for (i = 0; i < RTL8197F_ETH_RX_RING; i++) {
		if (rtl8197f_eth_rx_alloc(eth, i))
			goto err;
		rtl8197f_eth_rx_give(eth, i);
	}

	/* TX descriptors are owned by the CPU, only mark the ring end */
	eth->tx_ring[RTL8197F_ETH_TX_RING - 1].w[0] = DESC_EOR;

	eth->rx_idx = 0;
	eth->tx_head = 0;
	eth->tx_tail = 0;

	return 0;

err:
	rtl8197f_eth_free_rings(eth);
	return -ENOMEM;
}

static int rtl8197f_eth_open(struct net_device *ndev)
{
	struct rtl8197f_eth *eth = netdev_priv(ndev);
	int ret;

	ret = rtl8197f_eth_alloc_rings(eth);
	if (ret)
		return ret;

	netdev_reset_queue(ndev);
	napi_enable(&eth->napi);
	rtl8197f_eth_hw_start(eth);
	netif_carrier_on(ndev);
	netif_start_queue(ndev);

	return 0;
}

static int rtl8197f_eth_stop(struct net_device *ndev)
{
	struct rtl8197f_eth *eth = netdev_priv(ndev);

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	rtl8197f_eth_hw_stop(eth);
	napi_disable(&eth->napi);
	rtl8197f_eth_free_rings(eth);

	return 0;
}

static const struct net_device_ops rtl8197f_eth_netdev_ops = {
	.ndo_open = rtl8197f_eth_open,
	.ndo_stop = rtl8197f_eth_stop,
	.ndo_start_xmit = rtl8197f_eth_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static int rtl8197f_eth_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl8197f_eth *eth;
	struct net_device *ndev;
	int irq, ret;

	ndev = devm_alloc_etherdev(dev, sizeof(*eth));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, dev);
	eth = netdev_priv(ndev);
	eth->ndev = ndev;
	eth->dev = dev;

	eth->nic = devm_platform_ioremap_resource_byname(pdev, "nic");
	if (IS_ERR(eth->nic))
		return PTR_ERR(eth->nic);

	eth->swcore = devm_platform_ioremap_resource_byname(pdev, "swcore");
	if (IS_ERR(eth->swcore))
		return PTR_ERR(eth->swcore);

	eth->tables = devm_platform_ioremap_resource_byname(pdev, "tables");
	if (IS_ERR(eth->tables))
		return PTR_ERR(eth->tables);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	/* The descriptor layout below assumes 6 word descriptors */
	if (FIELD_GET(NIC_CPUICR1_RXDSC_SIZE, nic_r32(eth, NIC_CPUICR1)) != RTL8197F_ETH_DESC_WORDS ||
	    FIELD_GET(NIC_CPUICR1_TXDSC_SIZE, nic_r32(eth, NIC_CPUICR1)) != RTL8197F_ETH_DESC_WORDS)
		return dev_err_probe(dev, -ENODEV, "unexpected descriptor size, CPUICR1 %08x\n",
				     nic_r32(eth, NIC_CPUICR1));

	rtl8197f_eth_hw_stop(eth);

	ret = of_get_ethdev_address(dev->of_node, ndev);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (ret) {
		eth_hw_addr_random(ndev);
		dev_warn(dev, "using random MAC address %pM\n", ndev->dev_addr);
	}

	ndev->netdev_ops = &rtl8197f_eth_netdev_ops;
	ndev->irq = irq;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = ETH_DATA_LEN;
	netif_napi_add(ndev, &eth->napi, rtl8197f_eth_poll);

	ret = devm_request_irq(dev, irq, rtl8197f_eth_irq, 0, dev_name(dev), eth);
	if (ret)
		return ret;

	ret = devm_register_netdev(dev, ndev);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, eth);

	netdev_info(ndev, "RTL8197F switch core CPU port, irq %d, MAC %pM\n",
		    irq, ndev->dev_addr);

	return 0;
}

static void rtl8197f_eth_shutdown(struct platform_device *pdev)
{
	struct rtl8197f_eth *eth = platform_get_drvdata(pdev);

	if (eth)
		rtl8197f_eth_hw_stop(eth);
}

static const struct of_device_id rtl8197f_eth_of_match[] = {
	{ .compatible = "realtek,rtl8197f-eth" },
	{}
};
MODULE_DEVICE_TABLE(of, rtl8197f_eth_of_match);

static struct platform_driver rtl8197f_eth_driver = {
	.driver = {
		.name = "rtl8197f-eth",
		.of_match_table = rtl8197f_eth_of_match,
	},
	.probe = rtl8197f_eth_probe,
	.shutdown = rtl8197f_eth_shutdown,
};
module_platform_driver(rtl8197f_eth_driver);

MODULE_DESCRIPTION("Realtek RTL8197F switch core Ethernet driver");
MODULE_LICENSE("GPL");
