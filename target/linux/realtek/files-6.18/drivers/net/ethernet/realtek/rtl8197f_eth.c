// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL8197F switch core CPU port Ethernet driver
 *
 * The RTL8197F contains an rtl865x style switch core. Its port 0 is an RGMII
 * MAC that connects to an external switch. This driver only handles the CPU
 * interface (NIC) of the switch core: frames are sent directly to port 0 and
 * the switch core forwards frames from port 0 to the CPU. The driver resets
 * the switch core and sets up port 0 as the boot code does; the boot code
 * turns the switch core off before it starts a kernel from flash.
 *
 * With "realtek,cpu-tag", the external switch is a Realtek RTL83xx that tags
 * frames to and from its CPU port with a 4 byte Realtek CPU tag. Port 0 then
 * runs in "router mode": the switch core strips the tag and reports the port
 * of the external switch in the RX descriptor, and inserts a tag for the
 * destination ports given in the TX descriptor. The driver acts as the DSA
 * conduit of the external switch: received frames carry the port as
 * metadata, and the port mask of the "rtl8_4" tag of transmitted frames is
 * moved into the descriptor.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/unaligned.h>
#include <net/dsa.h>
#include <net/dst_metadata.h>
#include <net/flow_offload.h>
#include <net/page_pool/helpers.h>

/* System controller registers */
#define SYS_CLK_MANAGE			0x010
#define   SYS_CLK_ACTIVE_LX1_ARB	BIT(13)
#define   SYS_CLK_ACTIVE_LX1		BIT(12)
#define   SYS_CLK_ACTIVE_SWCORE		BIT(11)

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
#define   NIC_TXRINGCR_BLEN_ADJ		BIT(31)
#define   NIC_TXRINGCR_RING3_FIFO	GENMASK(19, 18)
#define   NIC_TXRINGCR_RING2_FIFO	GENMASK(17, 16)
#define   NIC_TXRINGCR_TXDCP_BP		GENMASK(11, 8)
#define   NIC_TXRINGCR_ROUND		BIT(4)
#define   NIC_TXRINGCR_RING0_EN		BIT(0)
#define NIC_DMA_CR4			0x0a0
#define   NIC_DMA_CR4_RX_TAIL		BIT(8)
#define   NIC_DMA_CR4_TX0_TAIL		BIT(0)
#define NIC_CPUICR1			0x0a4
#define   NIC_CPUICR1_RXDSC_SIZE	GENMASK(23, 18)
#define   NIC_CPUICR1_TXDSC_SIZE	GENMASK(17, 12)
#define   NIC_CPUICR1_HDR_TYPE		GENMASK(9, 8)
#define   NIC_CPUICR1_HDR_TYPE_NEW	1
#define   NIC_CPUICR1_TX_GATHER		BIT(6)
#define   NIC_CPUICR1_TSO_ID_INC	BIT(4)
#define   NIC_CPUICR1_LITTLE_ENDIAN	BIT(1)
#define   NIC_CPUICR1_TXRX_DIV_LX	BIT(0)

/* Switch core registers */
#define SW_MIB_IN(port, off)		(0x1100 + 0x80 * (port) + (off))
#define SW_MIB_OUT(port, off)		(0x1800 + 0x80 * (port) + (off))
#define SW_CSCR				0x4048
#define   SW_CSCR_L4_CHK_CAL		BIT(5)
#define   SW_CSCR_L3_CHK_CAL		BIT(4)
#define SW_MACCR1			0x4058
#define   SW_MACCR1_RMD_TAG		GENMASK(7, 6)
#define   SW_MACCR1_RMD_TAG_NONE	1	/* no VLAN tag in router mode */
#define   SW_MACCR1_P0_ROUTER_MODE	BIT(0)
#define SW_PITCR			0x4100
#define   SW_PITCR_P0_EXT		BIT(0)	/* port 0 is (G/R)MII */
#define SW_PCRP0			0x4104
#define   SW_PCR_EXT_PHY_ID		GENMASK(30, 26)
#define   SW_PCR_FORCE			BIT(25)
#define   SW_PCR_FORCE_LINK		BIT(23)
#define   SW_PCR_AN_STATUS		GENMASK(22, 18)
#define   SW_PCR_FORCE_SPEED		GENMASK(20, 19)
#define   SW_PCR_FORCE_SPEED_1000	2
#define   SW_PCR_FORCE_DUPLEX		BIT(18)
#define   SW_PCR_MII_RXER		BIT(13)
#define   SW_PCR_MAC_NORMAL		BIT(3)	/* MAC out of reset */
#define   SW_PCR_PHY_IF_EN		BIT(0)
#define SW_P0GMIICR			0x414c
#define   SW_P0GMIICR_TX_CPU_TAG	BIT(26)
#define   SW_P0GMIICR_CPU_TAG		BIT(25)
#define   SW_P0GMIICR_CONF_DONE		BIT(6)
#define   SW_P0GMIICR_TX_DELAY		BIT(4)
#define   SW_P0GMIICR_RX_DELAY		GENMASK(2, 0)
#define SW_SIRR				0x4204
#define   SW_SIRR_FULL_RST		BIT(2)	/* reset tables and queues */
#define   SW_SIRR_TRXRDY		BIT(0)
#define SW_MEMCR			0x4234
#define   SW_MEMCR_INIT			GENMASK(6, 0)
#define SW_SWTCR0			0x4418
#define   SW_SWTCR0_STOP_TLU_READY	BIT(19)
#define   SW_SWTCR0_STOP_TLU		BIT(18)
#define SW_FFCR				0x4428
#define   SW_FFCR_UNK_UC_TO_CPU		BIT(1)
#define   SW_FFCR_UNK_MC_TO_CPU		BIT(0)
#define SW_L2_LEARN_LIMIT(n)		(0x4488 + 4 * (n))	/* two ports each */
#define   SW_L2_LEARN_LIMIT_EN		(BIT(16) | BIT(0))	/* limit is 0 */
#define SW_QNUMCR			0x4754
#define   SW_QNUMCR_1Q(port)		BIT(3 * (port))	/* one output queue */
#define SW_VCR0				0x4a00
#define   SW_VCR0_1Q_VID_IGNORE		BIT(31)
#define   SW_VCR0_INGRESS_FILTER	GENMASK(8, 0)
#define SW_PVCR(n)			(0x4a08 + 4 * (n))	/* two ports each */
#define   SW_PVCR_PVID_EVEN		GENMASK(11, 0)
#define   SW_PVCR_PVID_ODD		GENMASK(27, 16)
#define SW_SWTACR			0x4d00
#define   SW_SWTACR_CMD_FORCE		BIT(3)
#define   SW_SWTACR_START		BIT(0)
#define SW_SWTAA			0x4d08
#define SW_TCR(n)			(0x4d20 + 4 * (n))

/* Switch core tables */
#define SW_TBL_ACL			0x0c0000
#define SW_TBL_OFFSET(type, idx)	(((type) << 16) + (idx) * 32)
#define SW_TBL_ADDR(type, idx)		(0xbb000000 + SW_TBL_OFFSET(type, idx))
#define SW_TBL_L2			0
#define   SW_TBL_L2_ENTRIES		1024
#define   SW_TBL_L2_WORDS		2
#define SW_TBL_VLAN			6
#define   SW_TBL_VLAN_WORDS		3
#define   SW_VLAN0_EXT_UNTAG		GENMASK(17, 15)
#define   SW_VLAN0_UNTAG		GENMASK(14, 9)
#define   SW_VLAN0_EXT_MEMBER		GENMASK(8, 6)
#define   SW_VLAN_EXT_CPU		BIT(2)	/* extension port 2 is the CPU */

/* Descriptor word 0, common to RX and TX */
#define DESC_OWN			BIT(0)	/* owned by the switch core */
#define DESC_EOR			BIT(1)	/* last descriptor of the ring */
#define DESC_LS				BIT(2)
#define DESC_FS				BIT(3)

/* TX descriptor */
#define TXD0_TYPE			GENMASK(31, 29)
#define TXD0_VLAN			BIT(28)	/* frame contains an 802.1Q tag */
#define TXD0_LEN			GENMASK(22, 6)	/* frame length with FCS */
#define TXD2_MLEN			GENMASK(31, 15)	/* buffer length */
#define TXD3_L3CS			BIT(20)
#define TXD3_L4CS			BIT(19)
#define TXD3_IPV6			BIT(18)
#define TXD3_IPV4			BIT(17)
#define TXD3_IPV4_1ST			BIT(16)
#define TXD4_LSO			BIT(31)
#define TXD4_DP				GENMASK(30, 24)
#define TXD4_IPV6_HLEN			GENMASK(15, 0)
#define TXD5_MSS			GENMASK(29, 16)
#define TXD5_IPV4_HLEN			GENMASK(7, 4)
#define TXD5_TCP_HLEN			GENMASK(3, 0)

/* RX descriptor */
#define RXD0_BUFSIZE			GENMASK(31, 16)
#define RXD2_LEN			GENMASK(13, 0)	/* frame length with FCS */
#define RXD3_TYPE			GENMASK(31, 29)
#define RXD4_SPA			GENMASK(15, 13)
#define RXD4_FRAG			BIT(11)
#define RXD4_IPV6			BIT(9)
#define RXD4_IPV4			BIT(8)
#define RXD5_L3CS_OK			BIT(31)
#define RXD5_L4CS_OK			BIT(30)

/* Packet types in TXD0_TYPE and RXD3_TYPE */
#define PKT_TYPE_TCP			5
#define PKT_TYPE_UDP			6

#define RTL8197F_ETH_DESC_WORDS		6
#define RTL8197F_ETH_RX_RING		256
#define RTL8197F_ETH_TX_RING		256
#define RTL8197F_ETH_TX_PORTS		BIT(0)	/* port 0: RGMII */
#define RTL8197F_ETH_FIFO_LOW		0xa0
#define RTL8197F_ETH_FIFO_HIGH		0xce
#define RTL8197F_ETH_PORTS		8	/* of the external switch */
#define RTL8197F_ETH_VID		1	/* switch core VLAN of all ports */
#define RTL8197F_ETH_P0_PHY_ID		5	/* as the boot code sets it */
#define RTL8197F_ETH_P0_RX_DELAY	5	/* as the boot code sets it */
#define RTL8197F_ETH_DSA_TAG_LEN	8	/* "rtl8_4" tag of transmitted frames */

/* Each RX buffer is a page fragment that becomes the skb head */
#define RTL8197F_ETH_RX_FRAG		2048
#define RTL8197F_ETH_RX_HEADROOM	(NET_SKB_PAD + NET_IP_ALIGN)
#define RTL8197F_ETH_RX_BUF_SIZE	\
	SKB_WITH_OVERHEAD(RTL8197F_ETH_RX_FRAG - RTL8197F_ETH_RX_HEADROOM)

struct rtl8197f_eth_desc {
	u32 w[RTL8197F_ETH_DESC_WORDS];
};

struct rtl8197f_eth_tx_buf {
	struct sk_buff *skb;		/* on the last descriptor of a frame */
	dma_addr_t dma;
	unsigned int len;
	bool frag;
};

struct rtl8197f_eth_stats {
	u64 tx_csum;
	u64 tx_csum_sw;
	u64 tx_tso;
	u64 rx_csum;
	u64 rx_alloc_err;
};

struct rtl8197f_eth {
	struct net_device *ndev;
	struct device *dev;
	void __iomem *nic;
	void __iomem *swcore;
	void __iomem *tables;
	struct regmap *sysctl;
	struct napi_struct napi;
	struct work_struct reset_work;
	struct rtl8197f_eth_stats stats;
	bool cpu_tag;
	struct metadata_dst *dsa_meta[RTL8197F_ETH_PORTS];

	struct page_pool *page_pool;
	struct rtl8197f_eth_desc *rx_ring;
	dma_addr_t rx_ring_dma;
	void *rx_buf[RTL8197F_ETH_RX_RING];
	dma_addr_t rx_dma[RTL8197F_ETH_RX_RING];
	unsigned int rx_idx;

	struct rtl8197f_eth_desc *tx_ring;
	dma_addr_t tx_ring_dma;
	struct rtl8197f_eth_tx_buf tx_buf[RTL8197F_ETH_TX_RING];
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

static void sw_rmw(struct rtl8197f_eth *eth, unsigned int reg, u32 clr, u32 set)
{
	writel((readl(eth->swcore + reg) & ~clr) | set, eth->swcore + reg);
}

static unsigned int rtl8197f_eth_tx_free(struct rtl8197f_eth *eth)
{
	return RTL8197F_ETH_TX_RING - 1 - (eth->tx_head - eth->tx_tail);
}

static void rtl8197f_eth_rx_give(struct rtl8197f_eth *eth, unsigned int idx)
{
	struct rtl8197f_eth_desc *desc = &eth->rx_ring[idx];
	u32 w0 = DESC_OWN | FIELD_PREP(RXD0_BUFSIZE, RTL8197F_ETH_RX_BUF_SIZE);

	if (idx == RTL8197F_ETH_RX_RING - 1)
		w0 |= DESC_EOR;

	desc->w[1] = eth->rx_dma[idx];
	desc->w[2] = 0;
	dma_wmb();
	desc->w[0] = w0;
}

static int rtl8197f_eth_rx_alloc(struct rtl8197f_eth *eth, unsigned int idx)
{
	unsigned int offset;
	struct page *page;
	dma_addr_t dma;

	page = page_pool_dev_alloc_frag(eth->page_pool, &offset,
					RTL8197F_ETH_RX_FRAG);
	if (!page)
		return -ENOMEM;

	/* The buffer may still be dirty in the cache from a previous user */
	dma = page_pool_get_dma_addr(page);
	dma_sync_single_range_for_device(eth->dev, dma,
					 offset + RTL8197F_ETH_RX_HEADROOM,
					 RTL8197F_ETH_RX_BUF_SIZE,
					 DMA_FROM_DEVICE);

	eth->rx_buf[idx] = page_address(page) + offset;
	eth->rx_dma[idx] = dma + offset + RTL8197F_ETH_RX_HEADROOM;

	return 0;
}

static bool rtl8197f_eth_rx_csum_ok(struct rtl8197f_eth_desc *desc)
{
	u32 w3 = READ_ONCE(desc->w[3]);
	u32 w4 = READ_ONCE(desc->w[4]);
	u32 w5 = READ_ONCE(desc->w[5]);

	if (!(w4 & (RXD4_IPV4 | RXD4_IPV6)) || (w4 & RXD4_FRAG))
		return false;

	if (FIELD_GET(RXD3_TYPE, w3) != PKT_TYPE_TCP &&
	    FIELD_GET(RXD3_TYPE, w3) != PKT_TYPE_UDP)
		return false;

	if ((w4 & RXD4_IPV4) && !(w5 & RXD5_L3CS_OK))
		return false;

	return w5 & RXD5_L4CS_OK;
}

static int rtl8197f_eth_rx(struct rtl8197f_eth *eth, int budget)
{
	struct net_device *ndev = eth->ndev;
	int done = 0;

	while (done < budget) {
		unsigned int idx = eth->rx_idx;
		struct rtl8197f_eth_desc *desc = &eth->rx_ring[idx];
		struct sk_buff *skb;
		unsigned int len;
		void *buf;

		if (READ_ONCE(desc->w[0]) & DESC_OWN)
			break;

		dma_rmb();
		len = FIELD_GET(RXD2_LEN, desc->w[2]);
		buf = eth->rx_buf[idx];

		/* The length includes the FCS */
		if (len < ETH_HLEN + ETH_FCS_LEN || len > RTL8197F_ETH_RX_BUF_SIZE) {
			ndev->stats.rx_length_errors++;
			goto give;
		}

		if (rtl8197f_eth_rx_alloc(eth, idx)) {
			eth->stats.rx_alloc_err++;
			ndev->stats.rx_dropped++;
			goto give;
		}

		page_pool_dma_sync_for_cpu(eth->page_pool, virt_to_head_page(buf),
					   offset_in_page(buf) + RTL8197F_ETH_RX_HEADROOM,
					   len);

		skb = napi_build_skb(buf, RTL8197F_ETH_RX_FRAG);
		if (unlikely(!skb)) {
			page_pool_put_full_page(eth->page_pool,
						virt_to_head_page(buf), true);
			ndev->stats.rx_dropped++;
			goto give;
		}

		skb_mark_for_recycle(skb);
		skb_reserve(skb, RTL8197F_ETH_RX_HEADROOM);
		skb_put(skb, len - ETH_FCS_LEN);
		skb->protocol = eth_type_trans(skb, ndev);
		if (eth->cpu_tag && netdev_uses_dsa(ndev)) {
			unsigned int port = FIELD_GET(RXD4_SPA, READ_ONCE(desc->w[4]));

			skb_dst_set_noref(skb, &eth->dsa_meta[port]->dst);
		}

		if ((ndev->features & NETIF_F_RXCSUM) &&
		    rtl8197f_eth_rx_csum_ok(desc)) {
			skb->ip_summed = CHECKSUM_UNNECESSARY;
			eth->stats.rx_csum++;
		}

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

static void rtl8197f_eth_tx_unmap(struct rtl8197f_eth *eth,
				  struct rtl8197f_eth_tx_buf *buf)
{
	if (buf->frag)
		dma_unmap_page(eth->dev, buf->dma, buf->len, DMA_TO_DEVICE);
	else
		dma_unmap_single(eth->dev, buf->dma, buf->len, DMA_TO_DEVICE);
}

static void rtl8197f_eth_tx_reclaim(struct rtl8197f_eth *eth, int budget)
{
	struct net_device *ndev = eth->ndev;
	unsigned int bytes = 0, pkts = 0;
	unsigned int hw_idx;

	/*
	 * The current descriptor pointer is the next descriptor the switch
	 * core will send. This also covers frames of several descriptors,
	 * where the OWN bit is only known to be cleared on the first one.
	 */
	hw_idx = (nic_r32(eth, NIC_CPUTPDCR0) - eth->tx_ring_dma) /
		 sizeof(struct rtl8197f_eth_desc);

	while (eth->tx_tail != eth->tx_head &&
	       eth->tx_tail % RTL8197F_ETH_TX_RING != hw_idx) {
		struct rtl8197f_eth_tx_buf *buf;

		buf = &eth->tx_buf[eth->tx_tail % RTL8197F_ETH_TX_RING];
		rtl8197f_eth_tx_unmap(eth, buf);
		if (buf->skb) {
			bytes += buf->skb->len;
			pkts++;
			napi_consume_skb(buf->skb, budget);
			buf->skb = NULL;
		}
		eth->tx_tail++;
	}

	ndev->stats.tx_packets += pkts;
	ndev->stats.tx_bytes += bytes;
	netdev_completed_queue(ndev, pkts, bytes);

	if (netif_queue_stopped(ndev) &&
	    rtl8197f_eth_tx_free(eth) > MAX_SKB_FRAGS + 1)
		netif_wake_queue(ndev);
}

static int rtl8197f_eth_poll(struct napi_struct *napi, int budget)
{
	struct rtl8197f_eth *eth = container_of(napi, struct rtl8197f_eth, napi);
	int done;

	rtl8197f_eth_tx_reclaim(eth, budget);
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

/*
 * Describe the checksum and segmentation offload of a frame. The switch core
 * does not parse the frame itself but relies on the descriptor flags, so only
 * TCP and UDP directly after an IPv4 or IPv6 header, behind at most one
 * 802.1Q tag, can be offloaded. rtl8197f_eth_features_check() keeps anything
 * else away from segmentation offload.
 */
static int rtl8197f_eth_tx_offload(struct rtl8197f_eth *eth, struct sk_buff *skb,
				   u32 *w0, u32 *w3, u32 *w4, u32 *w5)
{
	unsigned int l3 = ETH_HLEN, l4 = skb_checksum_start_offset(skb);
	__be16 proto = ((struct ethhdr *)skb->data)->h_proto;
	struct tcphdr *th;
	u8 l4proto;

	if (proto == htons(ETH_P_8021Q)) {
		proto = ((struct vlan_ethhdr *)skb->data)->h_vlan_encapsulated_proto;
		l3 += VLAN_HLEN;
		*w0 |= TXD0_VLAN;
	}

	if (proto == htons(ETH_P_IP) && l4 + sizeof(struct udphdr) <= skb_headlen(skb)) {
		struct iphdr *iph = (struct iphdr *)(skb->data + l3);

		if (l4 != l3 + iph->ihl * 4)
			goto sw;
		l4proto = iph->protocol;
		*w3 |= TXD3_IPV4 | TXD3_IPV4_1ST | TXD3_L3CS;
		*w5 |= FIELD_PREP(TXD5_IPV4_HLEN, iph->ihl);
	} else if (proto == htons(ETH_P_IPV6) &&
		   l4 + sizeof(struct udphdr) <= skb_headlen(skb)) {
		struct ipv6hdr *ip6h = (struct ipv6hdr *)(skb->data + l3);

		if (l4 != l3 + sizeof(*ip6h))
			goto sw;
		l4proto = ip6h->nexthdr;
		*w3 |= TXD3_IPV6;
		*w4 |= FIELD_PREP(TXD4_IPV6_HLEN, sizeof(*ip6h));
	} else {
		goto sw;
	}

	if (l4proto == IPPROTO_TCP &&
	    skb->csum_offset == offsetof(struct tcphdr, check))
		*w0 |= FIELD_PREP(TXD0_TYPE, PKT_TYPE_TCP);
	else if (l4proto == IPPROTO_UDP && !skb_is_gso(skb) &&
		 skb->csum_offset == offsetof(struct udphdr, check))
		*w0 |= FIELD_PREP(TXD0_TYPE, PKT_TYPE_UDP);
	else
		goto sw;

	*w3 |= TXD3_L4CS;
	eth->stats.tx_csum++;

	if (!skb_is_gso(skb))
		return 0;

	/* The switch core only segments frames longer than one segment */
	th = (struct tcphdr *)(skb->data + l4);
	if (skb->len - l4 - th->doff * 4 > skb_shinfo(skb)->gso_size) {
		*w4 |= TXD4_LSO;
		*w5 |= FIELD_PREP(TXD5_MSS, skb_shinfo(skb)->gso_size) |
		       FIELD_PREP(TXD5_TCP_HLEN, th->doff);
		eth->stats.tx_tso++;
	}

	return 0;

sw:
	*w0 &= ~TXD0_VLAN;
	*w3 = 0;
	*w4 &= ~TXD4_IPV6_HLEN;
	*w5 = 0;
	if (skb_is_gso(skb))
		return -EINVAL;

	eth->stats.tx_csum_sw++;
	return skb_checksum_help(skb);
}

/*
 * Remove the "rtl8_4" tag the DSA user port put behind the source MAC
 * address and return its forwarding port mask. The switch core inserts the
 * CPU tag of the external switch itself.
 */
static int rtl8197f_eth_dsa_untag(struct sk_buff *skb, u32 *ports)
{
	u8 *tag = skb->data + 2 * ETH_ALEN;

	if (skb_headlen(skb) < 2 * ETH_ALEN + RTL8197F_ETH_DSA_TAG_LEN ||
	    get_unaligned_be16(tag) != ETH_P_REALTEK || tag[2] != 0x04)
		return -EINVAL;

	*ports = get_unaligned_be16(tag + 6) & FIELD_MAX(TXD4_DP);
	memmove(skb->data + RTL8197F_ETH_DSA_TAG_LEN, skb->data, 2 * ETH_ALEN);
	__skb_pull(skb, RTL8197F_ETH_DSA_TAG_LEN);

	return *ports ? 0 : -EINVAL;
}

static netdev_tx_t rtl8197f_eth_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct rtl8197f_eth *eth = netdev_priv(ndev);
	unsigned int nr_frags, first, idx, frame_len, i;
	u32 w0 = 0, w3 = 0, w4, w5 = 0, first_w0 = 0;
	u32 ports = RTL8197F_ETH_TX_PORTS;
	bool kick;

	if (eth->cpu_tag &&
	    (!netdev_uses_dsa(ndev) || rtl8197f_eth_dsa_untag(skb, &ports)))
		goto drop;

	if (skb_put_padto(skb, ETH_ZLEN)) {
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	nr_frags = skb_shinfo(skb)->nr_frags;
	if (unlikely(rtl8197f_eth_tx_free(eth) < nr_frags + 1)) {
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}

	w4 = FIELD_PREP(TXD4_DP, ports);
	if (skb->ip_summed == CHECKSUM_PARTIAL &&
	    rtl8197f_eth_tx_offload(eth, skb, &w0, &w3, &w4, &w5))
		goto drop;

	/* Map all buffers first, so that a failure leaves no descriptor behind */
	first = eth->tx_head;
	for (i = 0, idx = first; i <= nr_frags; i++, idx++) {
		struct rtl8197f_eth_tx_buf *buf = &eth->tx_buf[idx % RTL8197F_ETH_TX_RING];
		unsigned int len;
		dma_addr_t dma;

		if (!i) {
			len = skb_headlen(skb);
			dma = dma_map_single(eth->dev, skb->data, len, DMA_TO_DEVICE);
		} else {
			skb_frag_t *frag = &skb_shinfo(skb)->frags[i - 1];

			len = skb_frag_size(frag);
			dma = skb_frag_dma_map(eth->dev, frag, 0, len, DMA_TO_DEVICE);
		}
		if (dma_mapping_error(eth->dev, dma))
			goto unmap;

		buf->skb = NULL;
		buf->dma = dma;
		buf->len = len;
		buf->frag = i;
	}

	/* The switch core appends the FCS, the lengths include it */
	frame_len = skb->len + ETH_FCS_LEN;
	for (i = 0, idx = first; i <= nr_frags; i++, idx++) {
		unsigned int ring_idx = idx % RTL8197F_ETH_TX_RING;
		struct rtl8197f_eth_desc *desc = &eth->tx_ring[ring_idx];
		struct rtl8197f_eth_tx_buf *buf = &eth->tx_buf[ring_idx];
		unsigned int len = buf->len;
		u32 dw0;

		dw0 = w0 | FIELD_PREP(TXD0_LEN, frame_len);
		if (!i)
			dw0 |= DESC_FS;
		if (i == nr_frags) {
			dw0 |= DESC_LS;
			len += ETH_FCS_LEN;
		}
		if (ring_idx == RTL8197F_ETH_TX_RING - 1)
			dw0 |= DESC_EOR;

		desc->w[1] = buf->dma;
		desc->w[2] = FIELD_PREP(TXD2_MLEN, len);
		desc->w[3] = w3;
		desc->w[4] = w4;
		desc->w[5] = w5;

		/*
		 * The switch core stops at the first descriptor while it is
		 * still owned by the CPU, so that one is handed over last.
		 */
		if (!i) {
			first_w0 = dw0;
		} else {
			dma_wmb();
			desc->w[0] = dw0 | DESC_OWN;
		}
	}

	eth->tx_buf[(idx - 1) % RTL8197F_ETH_TX_RING].skb = skb;
	dma_wmb();
	eth->tx_ring[first % RTL8197F_ETH_TX_RING].w[0] = first_w0 | DESC_OWN;
	eth->tx_head = idx;

	skb_tx_timestamp(skb);
	kick = __netdev_sent_queue(ndev, skb->len, netdev_xmit_more());
	if (rtl8197f_eth_tx_free(eth) <= MAX_SKB_FRAGS + 1) {
		netif_stop_queue(ndev);
		kick = true;
	}

	if (kick) {
		wmb();
		nic_rmw(eth, NIC_CPUICR, 0, NIC_CPUICR_TXFD);
	}

	return NETDEV_TX_OK;

unmap:
	while (idx-- != first)
		rtl8197f_eth_tx_unmap(eth, &eth->tx_buf[idx % RTL8197F_ETH_TX_RING]);
drop:
	dev_kfree_skb_any(skb);
	ndev->stats.tx_dropped++;

	return NETDEV_TX_OK;
}

static netdev_features_t rtl8197f_eth_features_check(struct sk_buff *skb,
						      struct net_device *ndev,
						      netdev_features_t features)
{
	__be16 proto;
	u8 l4proto;

	features = vlan_features_check(skb, features);
	if (!skb_is_gso(skb))
		return features;

	/* Only TCP directly after the IP header can be segmented */
	proto = vlan_get_protocol(skb);
	if (proto == htons(ETH_P_IP))
		l4proto = ip_hdr(skb)->protocol;
	else if (proto == htons(ETH_P_IPV6))
		l4proto = ipv6_hdr(skb)->nexthdr;
	else
		l4proto = 0;

	if (l4proto != IPPROTO_TCP ||
	    skb_transport_offset(skb) + tcp_hdrlen(skb) > skb_headlen(skb))
		features &= ~(NETIF_F_CSUM_MASK | NETIF_F_GSO_MASK);

	return features;
}

static int rtl8197f_eth_tbl_write(struct rtl8197f_eth *eth, unsigned int type,
				  unsigned int idx, const u32 *data,
				  unsigned int words)
{
	u32 val;
	int ret, i;

	/* Stop the table lookup unit while the entry changes */
	sw_rmw(eth, SW_SWTCR0, 0, SW_SWTCR0_STOP_TLU);
	ret = readl_poll_timeout_atomic(eth->swcore + SW_SWTCR0, val,
					val & SW_SWTCR0_STOP_TLU_READY, 1, 1000);
	if (!ret)
		ret = readl_poll_timeout_atomic(eth->swcore + SW_SWTACR, val,
						!(val & SW_SWTACR_START), 1, 1000);
	if (!ret) {
		for (i = 0; i < words; i++)
			writel(data[i], eth->swcore + SW_TCR(i));
		writel(SW_TBL_ADDR(type, idx), eth->swcore + SW_SWTAA);
		writel(SW_SWTACR_START | SW_SWTACR_CMD_FORCE, eth->swcore + SW_SWTACR);
		ret = readl_poll_timeout_atomic(eth->swcore + SW_SWTACR, val,
						!(val & SW_SWTACR_START), 1, 1000);
	}
	sw_rmw(eth, SW_SWTCR0, SW_SWTCR0_STOP_TLU, 0);

	return ret;
}

/*
 * Reset the switch core and set up port 0 for an external switch on RGMII,
 * as the boot code does. The boot code turns the switch core off before it
 * starts a kernel from flash, which loses this setup.
 */
static int rtl8197f_eth_hw_init(struct rtl8197f_eth *eth)
{
	u32 qnum = 0;
	int i, ret;

	ret = regmap_set_bits(eth->sysctl, SYS_CLK_MANAGE,
			      SYS_CLK_ACTIVE_LX1_ARB | SYS_CLK_ACTIVE_LX1 |
			      SYS_CLK_ACTIVE_SWCORE);
	if (ret)
		return ret;

	sw_rmw(eth, SW_SIRR, 0, SW_SIRR_FULL_RST);
	msleep(300);
	regmap_clear_bits(eth->sysctl, SYS_CLK_MANAGE, SYS_CLK_ACTIVE_SWCORE);
	msleep(300);
	regmap_set_bits(eth->sysctl, SYS_CLK_MANAGE, SYS_CLK_ACTIVE_SWCORE);
	msleep(50);

	/*
	 * Clear all tables. The clear runs on after the write, and its done
	 * bits stay set from the last clear, so wait for it instead: tables
	 * written in the meantime are wiped. It takes a few milliseconds.
	 */
	writel(0, eth->swcore + SW_MEMCR);
	writel(SW_MEMCR_INIT, eth->swcore + SW_MEMCR);
	msleep(50);

	/* Port 0: RGMII, forced to 1000 Mbit/s full duplex */
	sw_rmw(eth, SW_PITCR, 0, SW_PITCR_P0_EXT);
	sw_rmw(eth, SW_PCRP0, SW_PCR_EXT_PHY_ID | SW_PCR_AN_STATUS,
	       FIELD_PREP(SW_PCR_EXT_PHY_ID, RTL8197F_ETH_P0_PHY_ID) |
	       SW_PCR_FORCE | SW_PCR_FORCE_LINK |
	       FIELD_PREP(SW_PCR_FORCE_SPEED, SW_PCR_FORCE_SPEED_1000) |
	       SW_PCR_FORCE_DUPLEX | SW_PCR_MII_RXER | SW_PCR_MAC_NORMAL |
	       SW_PCR_PHY_IF_EN);
	sw_rmw(eth, SW_P0GMIICR, SW_P0GMIICR_TX_DELAY | SW_P0GMIICR_RX_DELAY,
	       SW_P0GMIICR_TX_DELAY |
	       FIELD_PREP(SW_P0GMIICR_RX_DELAY, RTL8197F_ETH_P0_RX_DELAY));
	sw_rmw(eth, SW_P0GMIICR, 0, SW_P0GMIICR_CONF_DONE);

	/* One output queue per port, unknown destinations to the CPU */
	for (i = 0; i < 5; i++)
		qnum |= SW_QNUMCR_1Q(i);
	writel(qnum, eth->swcore + SW_QNUMCR);
	writel(SW_FFCR_UNK_UC_TO_CPU | SW_FFCR_UNK_MC_TO_CPU,
	       eth->swcore + SW_FFCR);

	return 0;
}

/*
 * Send everything from port 0 to the CPU: a VLAN with only the CPU as member
 * for all ports and no learning of MAC addresses. In router mode the switch
 * core treats the ports of the external switch as its own ports and would
 * otherwise forward between them. By default, router mode also adds a VLAN
 * tag to frames sent to port 0, which the external switch passes on; turn
 * that off and keep the VLAN untagged everywhere.
 *
 * This runs once at probe, so that resets of the CPU interface keep the
 * tables of the switch core.
 */
static int rtl8197f_eth_setup_switch(struct rtl8197f_eth *eth)
{
	u32 vlan[SW_TBL_VLAN_WORDS] = {
		FIELD_PREP(SW_VLAN0_EXT_MEMBER, SW_VLAN_EXT_CPU) |
		FIELD_PREP(SW_VLAN0_UNTAG, FIELD_MAX(SW_VLAN0_UNTAG)) |
		FIELD_PREP(SW_VLAN0_EXT_UNTAG, FIELD_MAX(SW_VLAN0_EXT_UNTAG)),
	};
	u32 l2[SW_TBL_L2_WORDS] = {};
	int i, ret;

	/*
	 * Frames from the external switch may carry 802.1Q tags that are
	 * unknown to the switch core VLAN table. Classify all frames by the
	 * port VLAN instead, so they pass unmodified.
	 */
	sw_rmw(eth, SW_VCR0, 0, SW_VCR0_1Q_VID_IGNORE);

	/* Checksum calculation of the switch core, as the vendor sets it up */
	sw_rmw(eth, SW_CSCR, 0, SW_CSCR_L4_CHK_CAL | SW_CSCR_L3_CHK_CAL);

	if (eth->cpu_tag) {
		sw_rmw(eth, SW_P0GMIICR, 0,
		       SW_P0GMIICR_CPU_TAG | SW_P0GMIICR_TX_CPU_TAG);
		sw_rmw(eth, SW_MACCR1, SW_MACCR1_RMD_TAG,
		       SW_MACCR1_P0_ROUTER_MODE |
		       FIELD_PREP(SW_MACCR1_RMD_TAG, SW_MACCR1_RMD_TAG_NONE));
	}

	ret = rtl8197f_eth_tbl_write(eth, SW_TBL_VLAN, RTL8197F_ETH_VID, vlan,
				     ARRAY_SIZE(vlan));
	for (i = 0; i < 3; i++)
		writel(FIELD_PREP(SW_PVCR_PVID_EVEN, RTL8197F_ETH_VID) |
		       FIELD_PREP(SW_PVCR_PVID_ODD, RTL8197F_ETH_VID),
		       eth->swcore + SW_PVCR(i));
	sw_rmw(eth, SW_VCR0, SW_VCR0_INGRESS_FILTER, 0);

	for (i = 0; i < 5; i++)
		writel(SW_L2_LEARN_LIMIT_EN, eth->swcore + SW_L2_LEARN_LIMIT(i));
	for (i = 0; !ret && i < SW_TBL_L2_ENTRIES; i++)
		ret = rtl8197f_eth_tbl_write(eth, SW_TBL_L2, i, l2, ARRAY_SIZE(l2));

	/* Without the VLAN, nothing reaches the CPU */
	if (!ret && readl(eth->tables + SW_TBL_OFFSET(SW_TBL_VLAN,
						      RTL8197F_ETH_VID)) != vlan[0])
		ret = -EIO;

	return ret;
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

	/*
	 * New descriptors in little endian, frames of several TX descriptors,
	 * IPv4 ID incremented per segment, TX and RX on separate Lexra buses
	 */
	nic_rmw(eth, NIC_CPUICR1, NIC_CPUICR1_HDR_TYPE,
		NIC_CPUICR1_LITTLE_ENDIAN | NIC_CPUICR1_TX_GATHER |
		NIC_CPUICR1_TSO_ID_INC | NIC_CPUICR1_TXRX_DIV_LX |
		FIELD_PREP(NIC_CPUICR1_HDR_TYPE, NIC_CPUICR1_HDR_TYPE_NEW));

	/*
	 * Deliver all queues to RX ring 0 and use TX ring 0 only, the other
	 * TX ring settings as the boot code sets them
	 */
	for (i = 0; i < 3; i++)
		nic_w32(eth, NIC_CPUQDM(i), 0);
	nic_w32(eth, NIC_TXRINGCR, NIC_TXRINGCR_BLEN_ADJ |
		FIELD_PREP(NIC_TXRINGCR_RING3_FIFO, 2) |
		FIELD_PREP(NIC_TXRINGCR_RING2_FIFO, 1) |
		NIC_TXRINGCR_TXDCP_BP | NIC_TXRINGCR_ROUND |
		NIC_TXRINGCR_RING0_EN);

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

	sw_rmw(eth, SW_SIRR, 0, SW_SIRR_TRXRDY);

	nic_w32(eth, NIC_CPUIISR, nic_r32(eth, NIC_CPUIISR));
	nic_w32(eth, NIC_CPUIIMR, NIC_INT_RX_DONE0 | NIC_INT_RUNOUT0 |
				  NIC_INT_TX_DONE0);
}

static void rtl8197f_eth_free_rings(struct rtl8197f_eth *eth)
{
	int i;

	for (i = 0; i < RTL8197F_ETH_RX_RING; i++) {
		if (!eth->rx_buf[i])
			continue;
		page_pool_put_full_page(eth->page_pool,
					virt_to_head_page(eth->rx_buf[i]), false);
		eth->rx_buf[i] = NULL;
	}

	while (eth->tx_tail != eth->tx_head) {
		struct rtl8197f_eth_tx_buf *buf;

		buf = &eth->tx_buf[eth->tx_tail % RTL8197F_ETH_TX_RING];
		rtl8197f_eth_tx_unmap(eth, buf);
		if (buf->skb)
			dev_kfree_skb_any(buf->skb);
		buf->skb = NULL;
		eth->tx_tail++;
	}

	if (eth->rx_ring)
		dma_free_coherent(eth->dev, RTL8197F_ETH_RX_RING * sizeof(*eth->rx_ring),
				  eth->rx_ring, eth->rx_ring_dma);
	if (eth->tx_ring)
		dma_free_coherent(eth->dev, RTL8197F_ETH_TX_RING * sizeof(*eth->tx_ring),
				  eth->tx_ring, eth->tx_ring_dma);
	eth->rx_ring = NULL;
	eth->tx_ring = NULL;

	if (eth->page_pool)
		page_pool_destroy(eth->page_pool);
	eth->page_pool = NULL;
}

static int rtl8197f_eth_alloc_rings(struct rtl8197f_eth *eth)
{
	struct page_pool_params pp_params = {
		.flags = PP_FLAG_DMA_MAP,
		.pool_size = RTL8197F_ETH_RX_RING,
		.nid = NUMA_NO_NODE,
		.dev = eth->dev,
		.napi = &eth->napi,
		.netdev = eth->ndev,
		.dma_dir = DMA_FROM_DEVICE,
	};
	int i;

	eth->page_pool = page_pool_create(&pp_params);
	if (IS_ERR(eth->page_pool)) {
		int ret = PTR_ERR(eth->page_pool);

		eth->page_pool = NULL;
		return ret;
	}

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

static void rtl8197f_eth_reset_work(struct work_struct *work)
{
	struct rtl8197f_eth *eth = container_of(work, struct rtl8197f_eth,
						reset_work);

	rtnl_lock();
	if (netif_running(eth->ndev)) {
		rtl8197f_eth_stop(eth->ndev);
		if (rtl8197f_eth_open(eth->ndev))
			netdev_err(eth->ndev, "failed to restart after TX timeout\n");
	}
	rtnl_unlock();
}

static void rtl8197f_eth_cancel_reset(void *data)
{
	struct rtl8197f_eth *eth = data;

	cancel_work_sync(&eth->reset_work);
}

static void rtl8197f_eth_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
	struct rtl8197f_eth *eth = netdev_priv(ndev);

	netdev_err(ndev, "TX timeout, head %u tail %u, CPUTPDCR0 %08x CPUICR %08x\n",
		   eth->tx_head, eth->tx_tail, nic_r32(eth, NIC_CPUTPDCR0),
		   nic_r32(eth, NIC_CPUICR));
	schedule_work(&eth->reset_work);
}

/* A connection of the flowtable in one direction */
struct rtl8197f_eth_flow {
	int in_port;			/* ports of the external switch */
	int out_port;
	u8 proto;
	__be32 saddr, daddr;		/* as received */
	__be16 sport, dport;
	__be32 nat_saddr, nat_daddr;	/* as sent */
	__be16 nat_sport, nat_dport;
	struct ethhdr eth;		/* as sent */
};

/* The port of the external switch behind a DSA user port of ours */
static int rtl8197f_eth_flow_port(struct rtl8197f_eth *eth,
				  struct net_device *dev)
{
	struct dsa_port *dp;

	if (!dev)
		return -ENODEV;

	dp = dsa_port_from_netdev(dev);
	if (IS_ERR(dp) || dsa_port_to_conduit(dp) != eth->ndev)
		return -EOPNOTSUPP;

	return dp->index;
}

static int rtl8197f_eth_flow_mangle(struct rtl8197f_eth_flow *flow,
				    const struct flow_action_entry *act)
{
	u32 val = ntohl(act->mangle.val);
	unsigned int len = 4;
	const u8 *src;
	u8 *dst;

	switch (act->mangle.htype) {
	case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
		/*
		 * Each MAC address comes as a 4 and a 2 byte part. The mask
		 * keeps the bytes that the value does not set.
		 */
		if (act->mangle.offset > 8)
			return -EOPNOTSUPP;
		dst = (u8 *)&flow->eth + act->mangle.offset;
		src = (const u8 *)&act->mangle.val;
		if (act->mangle.mask == 0xffff) {
			src += 2;
			dst += 2;
		}
		if (act->mangle.mask)
			len = 2;
		memcpy(dst, src, len);
		return 0;
	case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
		if (act->mangle.offset == offsetof(struct iphdr, saddr))
			flow->nat_saddr = (__force __be32)act->mangle.val;
		else if (act->mangle.offset == offsetof(struct iphdr, daddr))
			flow->nat_daddr = (__force __be32)act->mangle.val;
		else
			return -EOPNOTSUPP;
		return 0;
	case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
	case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
		/* Both ports are in the first word, the mask keeps the other */
		if (act->mangle.offset)
			return -EOPNOTSUPP;
		if (act->mangle.mask == ~htonl(0xffff0000))
			flow->nat_sport = htons(val >> 16);
		else if (act->mangle.mask == ~htonl(0xffff))
			flow->nat_dport = htons(val & 0xffff);
		else
			return -EOPNOTSUPP;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int rtl8197f_eth_flow_parse(struct rtl8197f_eth *eth,
				   struct flow_cls_offload *cls,
				   struct rtl8197f_eth_flow *flow)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls);
	struct flow_match_ipv4_addrs addrs;
	struct flow_match_control control;
	struct flow_match_basic basic;
	struct flow_match_ports ports;
	struct flow_match_meta meta;
	struct flow_action_entry *act;
	int i, ret;

	memset(flow, 0, sizeof(*flow));

	/* Routed IPv4 TCP and UDP, without VLAN or PPPoE */
	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS) ||
	    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN))
		return -EOPNOTSUPP;

	flow_rule_match_control(rule, &control);
	if (control.key->addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS)
		return -EOPNOTSUPP;

	flow_rule_match_basic(rule, &basic);
	flow->proto = basic.key->ip_proto;
	if (flow->proto != IPPROTO_TCP && flow->proto != IPPROTO_UDP)
		return -EOPNOTSUPP;

	flow_rule_match_meta(rule, &meta);
	rcu_read_lock();
	flow->in_port = rtl8197f_eth_flow_port(eth,
		dev_get_by_index_rcu(dev_net(eth->ndev),
				     meta.key->ingress_ifindex));
	rcu_read_unlock();
	if (flow->in_port < 0)
		return flow->in_port;

	flow_rule_match_ipv4_addrs(rule, &addrs);
	flow->saddr = flow->nat_saddr = addrs.key->src;
	flow->daddr = flow->nat_daddr = addrs.key->dst;
	flow_rule_match_ports(rule, &ports);
	flow->sport = flow->nat_sport = ports.key->src;
	flow->dport = flow->nat_dport = ports.key->dst;

	flow->out_port = -EOPNOTSUPP;
	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			ret = rtl8197f_eth_flow_mangle(flow, act);
			if (ret)
				return ret;
			break;
		case FLOW_ACTION_REDIRECT:
			flow->out_port = rtl8197f_eth_flow_port(eth, act->dev);
			break;
		case FLOW_ACTION_CSUM:
			break;
		default:
			return -EOPNOTSUPP;
		}
	}
	if (flow->out_port < 0)
		return flow->out_port;

	if (!is_valid_ether_addr(flow->eth.h_source) ||
	    !is_valid_ether_addr(flow->eth.h_dest))
		return -EINVAL;

	return 0;
}

static int rtl8197f_eth_flow_replace(struct rtl8197f_eth *eth,
				     struct flow_cls_offload *cls)
{
	struct rtl8197f_eth_flow flow;
	int ret;

	ret = rtl8197f_eth_flow_parse(eth, cls, &flow);
	if (!net_ratelimit())
		return -EOPNOTSUPP;

	if (ret)
		netdev_info(eth->ndev, "flow %lx: not offloadable (%d)\n",
			    cls->cookie, ret);
	else
		netdev_info(eth->ndev,
			    "flow %lx: %s %pI4:%u > %pI4:%u port %d > %d, sent as %pI4:%u > %pI4:%u %pM > %pM\n",
			    cls->cookie,
			    flow.proto == IPPROTO_TCP ? "tcp" : "udp",
			    &flow.saddr, ntohs(flow.sport),
			    &flow.daddr, ntohs(flow.dport),
			    flow.in_port, flow.out_port,
			    &flow.nat_saddr, ntohs(flow.nat_sport),
			    &flow.nat_daddr, ntohs(flow.nat_dport),
			    flow.eth.h_source, flow.eth.h_dest);

	/* Nothing is offloaded yet, the flowtable forwards in software */
	return -EOPNOTSUPP;
}

static int rtl8197f_eth_flow_block_cb(enum tc_setup_type type,
				      void *type_data, void *cb_priv)
{
	struct flow_cls_offload *cls = type_data;
	struct rtl8197f_eth *eth = cb_priv;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		return rtl8197f_eth_flow_replace(eth, cls);
	default:
		return -EOPNOTSUPP;
	}
}

static LIST_HEAD(rtl8197f_eth_block_cb_list);

/*
 * The flowtable offload of the DSA user ports ends up here. All user ports
 * share the flowtable's block, so bind it once and count the users.
 */
static int rtl8197f_eth_setup_tc(struct net_device *ndev,
				 enum tc_setup_type type, void *type_data)
{
	struct rtl8197f_eth *eth = netdev_priv(ndev);
	flow_setup_cb_t *cb = rtl8197f_eth_flow_block_cb;
	struct flow_block_offload *f = type_data;
	struct flow_block_cb *block_cb;

	if (type != TC_SETUP_FT || !eth->cpu_tag ||
	    f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &rtl8197f_eth_block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, cb, eth);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}
		block_cb = flow_block_cb_alloc(cb, eth, eth, NULL);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);
		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &rtl8197f_eth_block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, cb, eth);
		if (!block_cb)
			return -ENOENT;
		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct net_device_ops rtl8197f_eth_netdev_ops = {
	.ndo_open = rtl8197f_eth_open,
	.ndo_stop = rtl8197f_eth_stop,
	.ndo_start_xmit = rtl8197f_eth_xmit,
	.ndo_features_check = rtl8197f_eth_features_check,
	.ndo_tx_timeout = rtl8197f_eth_tx_timeout,
	.ndo_setup_tc = rtl8197f_eth_setup_tc,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

/* Driver counters, followed by switch core MIB counters */
static const struct {
	char name[ETH_GSTRING_LEN];
	unsigned int offset;
} rtl8197f_eth_sw_stats[] = {
	{ "tx_csum_offload", offsetof(struct rtl8197f_eth_stats, tx_csum) },
	{ "tx_csum_sw", offsetof(struct rtl8197f_eth_stats, tx_csum_sw) },
	{ "tx_tso", offsetof(struct rtl8197f_eth_stats, tx_tso) },
	{ "rx_csum_ok", offsetof(struct rtl8197f_eth_stats, rx_csum) },
	{ "rx_alloc_errors", offsetof(struct rtl8197f_eth_stats, rx_alloc_err) },
}, rtl8197f_eth_mib_stats[] = {
	/* Port 0 is the RGMII port, port 6 the CPU port */
	{ "p0_in_octets", SW_MIB_IN(0, 0x00) },
	{ "p0_in_ucast", SW_MIB_IN(0, 0x08) },
	{ "p0_in_mcast", SW_MIB_IN(0, 0x3c) },
	{ "p0_in_bcast", SW_MIB_IN(0, 0x40) },
	{ "p0_in_discards", SW_MIB_IN(0, 0x44) },
	{ "p0_in_drops", SW_MIB_IN(0, 0x48) },
	{ "p0_in_fcs_errors", SW_MIB_IN(0, 0x4c) },
	{ "p0_out_octets", SW_MIB_OUT(0, 0x00) },
	{ "p0_out_ucast", SW_MIB_OUT(0, 0x08) },
	{ "p0_out_mcast", SW_MIB_OUT(0, 0x0c) },
	{ "p0_out_bcast", SW_MIB_OUT(0, 0x10) },
	{ "p0_out_discards", SW_MIB_OUT(0, 0x14) },
	{ "cpu_in_discards", SW_MIB_IN(6, 0x44) },
	{ "cpu_in_drops", SW_MIB_IN(6, 0x48) },
	{ "cpu_out_discards", SW_MIB_OUT(6, 0x14) },
};

static void rtl8197f_eth_get_strings(struct net_device *ndev, u32 sset, u8 *data)
{
	int i;

	if (sset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(rtl8197f_eth_sw_stats); i++)
		ethtool_puts(&data, rtl8197f_eth_sw_stats[i].name);
	for (i = 0; i < ARRAY_SIZE(rtl8197f_eth_mib_stats); i++)
		ethtool_puts(&data, rtl8197f_eth_mib_stats[i].name);
}

static int rtl8197f_eth_get_sset_count(struct net_device *ndev, int sset)
{
	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;

	return ARRAY_SIZE(rtl8197f_eth_sw_stats) + ARRAY_SIZE(rtl8197f_eth_mib_stats);
}

static void rtl8197f_eth_get_ethtool_stats(struct net_device *ndev,
					   struct ethtool_stats *stats, u64 *data)
{
	struct rtl8197f_eth *eth = netdev_priv(ndev);
	int i;

	for (i = 0; i < ARRAY_SIZE(rtl8197f_eth_sw_stats); i++)
		*data++ = *(u64 *)((u8 *)&eth->stats + rtl8197f_eth_sw_stats[i].offset);
	for (i = 0; i < ARRAY_SIZE(rtl8197f_eth_mib_stats); i++)
		*data++ = readl(eth->swcore + rtl8197f_eth_mib_stats[i].offset);
}

static const unsigned int rtl8197f_eth_nic_regs[] = {
	NIC_CPUICR, NIC_CPURPDCR(0), NIC_CPUTPDCR0, NIC_CPUIIMR, NIC_CPUIISR,
	NIC_CPUQDM(0), NIC_CPUQDM(1), NIC_CPUQDM(2), NIC_DMA_CR0, NIC_DMA_CR1,
	NIC_DMA_CR2, NIC_TXRINGCR, NIC_DMA_CR4, NIC_CPUICR1,
};

static const unsigned int rtl8197f_eth_sw_regs[] = {
	SW_CSCR, SW_SIRR, SW_VCR0, SW_MACCR1, SW_P0GMIICR,
};

static int rtl8197f_eth_get_regs_len(struct net_device *ndev)
{
	return (ARRAY_SIZE(rtl8197f_eth_nic_regs) +
		ARRAY_SIZE(rtl8197f_eth_sw_regs)) * sizeof(u32);
}

static void rtl8197f_eth_get_regs(struct net_device *ndev,
				  struct ethtool_regs *regs, void *p)
{
	struct rtl8197f_eth *eth = netdev_priv(ndev);
	u32 *buf = p;
	int i;

	regs->version = 1;
	for (i = 0; i < ARRAY_SIZE(rtl8197f_eth_nic_regs); i++)
		*buf++ = nic_r32(eth, rtl8197f_eth_nic_regs[i]);
	for (i = 0; i < ARRAY_SIZE(rtl8197f_eth_sw_regs); i++)
		*buf++ = readl(eth->swcore + rtl8197f_eth_sw_regs[i]);
}

static void rtl8197f_eth_get_ringparam(struct net_device *ndev,
				       struct ethtool_ringparam *ring,
				       struct kernel_ethtool_ringparam *kernel_ring,
				       struct netlink_ext_ack *extack)
{
	ring->rx_max_pending = RTL8197F_ETH_RX_RING;
	ring->tx_max_pending = RTL8197F_ETH_TX_RING;
	ring->rx_pending = RTL8197F_ETH_RX_RING;
	ring->tx_pending = RTL8197F_ETH_TX_RING;
}

static const struct ethtool_ops rtl8197f_eth_ethtool_ops = {
	.get_link = ethtool_op_get_link,
	.get_strings = rtl8197f_eth_get_strings,
	.get_sset_count = rtl8197f_eth_get_sset_count,
	.get_ethtool_stats = rtl8197f_eth_get_ethtool_stats,
	.get_regs_len = rtl8197f_eth_get_regs_len,
	.get_regs = rtl8197f_eth_get_regs,
	.get_ringparam = rtl8197f_eth_get_ringparam,
};

static void rtl8197f_eth_free_dsa_meta(void *data)
{
	struct rtl8197f_eth *eth = data;
	int i;

	for (i = 0; i < ARRAY_SIZE(eth->dsa_meta); i++)
		if (eth->dsa_meta[i])
			metadata_dst_free(eth->dsa_meta[i]);
}

static int rtl8197f_eth_alloc_dsa_meta(struct rtl8197f_eth *eth)
{
	int i, ret;

	ret = devm_add_action_or_reset(eth->dev, rtl8197f_eth_free_dsa_meta, eth);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(eth->dsa_meta); i++) {
		eth->dsa_meta[i] = metadata_dst_alloc(0, METADATA_HW_PORT_MUX,
						      GFP_KERNEL);
		if (!eth->dsa_meta[i])
			return -ENOMEM;

		eth->dsa_meta[i]->u.port_info.port_id = i;
		eth->dsa_meta[i]->u.port_info.lower_dev = eth->ndev;
	}

	return 0;
}

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
	INIT_WORK(&eth->reset_work, rtl8197f_eth_reset_work);

	eth->nic = devm_platform_ioremap_resource_byname(pdev, "nic");
	if (IS_ERR(eth->nic))
		return PTR_ERR(eth->nic);

	eth->swcore = devm_platform_ioremap_resource_byname(pdev, "swcore");
	if (IS_ERR(eth->swcore))
		return PTR_ERR(eth->swcore);

	eth->tables = devm_platform_ioremap_resource_byname(pdev, "tables");
	if (IS_ERR(eth->tables))
		return PTR_ERR(eth->tables);

	eth->sysctl = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,sysctl");
	if (IS_ERR(eth->sysctl))
		return dev_err_probe(dev, PTR_ERR(eth->sysctl),
				     "no system controller\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ret = of_get_ethdev_address(dev->of_node, ndev);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (ret) {
		eth_hw_addr_random(ndev);
		dev_warn(dev, "using random MAC address %pM\n", ndev->dev_addr);
	}

	ret = rtl8197f_eth_hw_init(eth);
	if (ret)
		return dev_err_probe(dev, ret, "cannot reset the switch core\n");

	/* The descriptor layout below assumes 6 word descriptors */
	if (FIELD_GET(NIC_CPUICR1_RXDSC_SIZE, nic_r32(eth, NIC_CPUICR1)) != RTL8197F_ETH_DESC_WORDS ||
	    FIELD_GET(NIC_CPUICR1_TXDSC_SIZE, nic_r32(eth, NIC_CPUICR1)) != RTL8197F_ETH_DESC_WORDS)
		return dev_err_probe(dev, -ENODEV, "unexpected descriptor size, CPUICR1 %08x\n",
				     nic_r32(eth, NIC_CPUICR1));

	rtl8197f_eth_hw_stop(eth);

	eth->cpu_tag = of_property_read_bool(dev->of_node, "realtek,cpu-tag");
	ret = rtl8197f_eth_setup_switch(eth);
	if (ret)
		return dev_err_probe(dev, ret, "cannot set up the switch core tables\n");

	if (eth->cpu_tag) {
		ret = rtl8197f_eth_alloc_dsa_meta(eth);
		if (ret)
			return ret;
	}

	ndev->netdev_ops = &rtl8197f_eth_netdev_ops;
	ndev->ethtool_ops = &rtl8197f_eth_ethtool_ops;
	ndev->irq = irq;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = ETH_DATA_LEN;
	/* DSA user ports add their tag, which is removed before transmission */
	if (eth->cpu_tag)
		ndev->max_mtu += RTL8197F_ETH_DSA_TAG_LEN;
	ndev->hw_features = NETIF_F_SG | NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM |
			    NETIF_F_TSO | NETIF_F_TSO6 | NETIF_F_RXCSUM;
	ndev->features = ndev->hw_features;
	ndev->vlan_features = NETIF_F_SG | NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM |
			      NETIF_F_TSO | NETIF_F_TSO6;
	/* The frame length field of the descriptors holds 17 bits */
	netif_set_tso_max_size(ndev, SZ_64K - 1 - ETH_FCS_LEN);
	netif_napi_add(ndev, &eth->napi, rtl8197f_eth_poll);

	ret = devm_add_action_or_reset(dev, rtl8197f_eth_cancel_reset, eth);
	if (ret)
		return ret;

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
