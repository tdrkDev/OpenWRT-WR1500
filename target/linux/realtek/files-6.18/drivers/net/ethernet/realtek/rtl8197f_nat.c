// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL8197F switch core NAPT offload
 *
 * The switch core can route and NAT IPv4 TCP and UDP connections between the
 * ports of the external switch by itself. This offloads the connections of a
 * netfilter flowtable with "flags offload" on the DSA user ports.
 *
 * Each offloaded direction of a connection is one entry of the NAPT table:
 * an outbound entry for the direction that the flowtable source-NATs, an
 * inbound entry for the direction that it destination-NATs. The entry names
 * a next hop: the network interface to send from and the L2 entry of the MAC
 * address to send to.
 *
 * The switch core only routes frames that are addressed to the MAC address
 * of one of its network interfaces, in the VLAN of that interface, and it
 * only sends to ports in the VLAN of the outgoing interface. A port that
 * carries offloaded connections therefore gets the VLAN of the interface with
 * the router's MAC address on that port as port VLAN, and is a member of it.
 *
 * As in the vendor driver, each LAN subnet with offloaded connections has a
 * route that resolves its hosts through ARP entries, and the default route
 * points to the NAPT engine and the WAN gateway. The switch core only uses
 * the NAPT entries of hosts that have an ARP entry. Packets without a NAPT
 * entry go to the CPU, and so do broadcasts and frames to unknown
 * destinations.
 *
 * An outbound entry does not store the remote address and port. A packet from
 * the same local address and port whose connection hashes to the same group
 * of four entries is translated as well, before conntrack has seen it. Its
 * replies then reach the CPU without a connection to match. So outbound UDP
 * is only offloaded on request (debugfs "udp_outbound"); TCP connections
 * start with a SYN, which the entries leave to the CPU.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/hashtable.h>
#include <linux/inetdevice.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ip.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <net/arp.h>
#include <net/dsa.h>
#include <net/flow_offload.h>

#include "rtl8197f_eth.h"

/* Switch core registers */
#define SW_TEACR			0x4400
#define   SW_TEACR_L4_AGING_OFF		BIT(1)
#define   SW_TEACR_L2_AGING_OFF		BIT(0)
#define SW_TEATCR			0x4404
#define   SW_TEATCR_UDP			GENMASK(23, 18)
#define   SW_TEATCR_TCP_LONG		GENMASK(17, 12)
#define   SW_TEATCR_TCP_MEDIUM		GENMASK(11, 6)
#define   SW_TEATCR_TCP_SHORT		GENMASK(5, 0)
#define SW_ALECR			0x440c
#define   SW_ALECR_TTL_DEC		BIT(16)
#define   SW_ALECR_FRAG_TO_CPU		BIT(15)
#define SW_MSCR				0x4410
#define   SW_MSCR_L4			BIT(2)
#define   SW_MSCR_L3			BIT(1)
#define   SW_MSCR_L2			BIT(0)
#define SW_SWTCR0			0x4418
#define   SW_SWTCR0_STOP_TLU_READY	BIT(19)
#define   SW_SWTCR0_STOP_TLU		BIT(18)
#define   SW_SWTCR0_NETIF_BY		GENMASK(17, 16)	/* 0: by VLAN */
#define   SW_SWTCR0_NAPT_OTHER_TO_CPU	BIT(14)	/* not TCP, UDP or ICMP */
#define   SW_SWTCR0_WAN_ROUTE		GENMASK(4, 3)
#define   SW_SWTCR0_NAPT_AUTO_DELETE	BIT(2)
#define   SW_SWTCR0_NAPT_AUTO_LEARN	BIT(1)
#define   SW_SWTCR0_NAPT_MISS_DROP	BIT(0)	/* inbound misses */
#define SW_SWTCR1			0x441c
#define   SW_SWTCR1_L4_HASH1		BIT(13)	/* enhanced hash 1 */
#define   SW_SWTCR1_FRAG_TO_ACL_PERMIT	BIT(11)
#define   SW_SWTCR1_NAT_T2LOG		BIT(10)
#define   SW_SWTCR1_L4_4WAY		BIT(9)
#define SW_SWTACR			0x4d00
#define   SW_SWTACR_CMD_FORCE		BIT(3)
#define   SW_SWTACR_START		BIT(0)
#define SW_SWTAA			0x4d08
#define SW_TCR(n)			(0x4d20 + 4 * (n))

/* Switch core tables, see also rtl8197f_eth.h */
#define SW_TBL_ADDR(type, idx)		(0xbb000000 + SW_TBL_OFFSET(type, idx))
#define   L2_1_AUTH			BIT(25)
#define   L2_1_NEXT_HOP			BIT(22)	/* kept when aged */
#define   L2_1_AGING			GENMASK(20, 19)
#define   L2_1_STATIC			BIT(18)
#define   L2_1_TO_CPU			BIT(17)
#define   L2_1_MEMBER			GENMASK(13, 8)
#define   L2_1_MAC0			GENMASK(7, 0)
#define SW_TBL_ARP			1
#define   SW_TBL_ARPS			512
#define   SW_TBL_ARP_WORDS		1
#define   ARP_AGE			GENMASK(15, 11)
#define   ARP_L2			GENMASK(10, 1)
#define   ARP_VALID			BIT(0)
#define   ARP_AGE_MAX			31	/* hits reload it */
#define   ARP_AGE_REFRESH		16
#define SW_TBL_ROUTE			2
#define   SW_TBL_ROUTES			8
#define   SW_TBL_ROUTE_WORDS		3
#define   ROUTE1_IP_DOMAIN		GENMASK(27, 25)
#define   ROUTE1_ARP_END		GENMASK(25, 20)	/* ARP entry / 8 */
#define   ROUTE1_NH_ALGO		GENMASK(24, 23)
#define   ROUTE1_ARP_START		GENMASK(19, 14)	/* ARP entry / 8 */
#define   ROUTE1_NH_START		GENMASK(17, 14)	/* next hop / 2 */
#define   ROUTE1_NH_NUM			GENMASK(13, 11)	/* 0: 2 next hops */
#define   ROUTE1_NETIF			GENMASK(13, 11)
#define   ROUTE1_INTERNAL		BIT(9)
#define   ROUTE1_PROCESS		GENMASK(8, 6)
#define   ROUTE1_VALID			BIT(5)
#define   ROUTE1_PREFIX			GENMASK(4, 0)	/* length - 1 */
#define   ROUTE_PROCESS_ARP		2
#define   ROUTE_PROCESS_NAPT		5
#define   ROUTE_NH_ALGO_SIP		2	/* next hop by source IP */
#define   ROUTE_IP_DOMAIN		6	/* as the vendor sets it */
#define SW_TBL_NETIF			4
#define   SW_TBL_NETIFS			8
#define   SW_TBL_NETIF_WORDS		5
#define   NETIF0_MAC_LOW		GENMASK(31, 13)	/* MAC bits 18..0 */
#define   NETIF0_VID			GENMASK(12, 1)
#define   NETIF0_VALID			BIT(0)
#define   NETIF1_IN_ACL_START_L		BIT(31)
#define   NETIF1_ROUTE			BIT(29)
#define   NETIF1_MAC_HIGH		GENMASK(28, 0)	/* MAC bits 47..19 */
#define   NETIF2_MAC_MASK_L		BIT(31)
#define   NETIF2_OUT_ACL_END		GENMASK(30, 23)
#define   NETIF2_OUT_ACL_START		GENMASK(22, 15)
#define   NETIF2_IN_ACL_END		GENMASK(14, 7)
#define   NETIF2_IN_ACL_START_H		GENMASK(6, 0)
#define   NETIF3_MTU			GENMASK(16, 2)
#define   NETIF3_MAC_MASK_H		GENMASK(1, 0)
#define   NETIF_MAC_MASK_ONE		7	/* one MAC address */
#define   NETIF_ACL			253	/* the vendor's permit rule */
#define SW_TBL_EXTIP			5
#define   SW_TBL_EXTIPS			16
#define   SW_TBL_EXTIP_WORDS		3
#define   EXTIP2_VALID			BIT(0)
#define SW_TBL_NAPT			9
#define   SW_TBL_NAPTS			1024
#define   SW_TBL_NAPT_WORDS		3
#define   NAPT1_SEL_E			GENMASK(30, 21)
#define   NAPT1_SEL_IP			GENMASK(20, 17)
#define   NAPT1_STATIC			BIT(16)
#define   NAPT1_COLLISION2		BIT(14)
#define   NAPT1_OFFSET			GENMASK(13, 8)
#define   NAPT1_AGE			GENMASK(7, 2)
#define   NAPT1_COLLISION		BIT(1)
#define   NAPT1_VALID			BIT(0)
#define   NAPT2_NH			GENMASK(29, 25)
#define   NAPT2_NH_VALID		BIT(24)
#define   NAPT2_TCP			BIT(19)
#define   NAPT2_TCP_FLAG		GENMASK(18, 16)
#define   NAPT2_INT_PORT		GENMASK(15, 0)
#define   NAPT_TCP_FLAG_OUTBOUND	3
#define   NAPT_TCP_FLAG_INBOUND		2
#define SW_TBL_ACL			12
#define SW_TBL_NH			13
#define   SW_TBL_NHS			32
#define   SW_TBL_NH_WORDS		1
#define   NH_L2				GENMASK(20, 11)
#define   NH_NETIF			GENMASK(7, 5)
#define   NH_EXTIP			GENMASK(4, 1)

#define NAT_PORTS			6	/* ports 0..5 of the switch core */
#define NAT_VID(netif)			(RTL8197F_ETH_VID + 1 + (netif))
#define NAT_SUBNETS			7	/* routes 0..6 */
#define NAT_SUBNET_PREFIX_MIN		23	/* up to 512 ARP entries */
#define NAT_ARP_BLOCK			8	/* ARP ranges of routes are in 8s */
#define NAT_ROUTE_DEFAULT		7
#define NAT_NH_ROUTE			0	/* 0 and 1: the default route */
#define NAT_NH_FIRST			2
#define NAT_L2_BCAST			0	/* ff:ff:ff:ff:ff:ff hashes to row 0 */
#define NAT_WAYS			4	/* of L2 rows and NAPT groups */
/*
 * NAPT aging time in the units of the switch core, 61 s. Hits reload the
 * age, so the age tells how long ago an entry was last used. It must stay
 * above the flowtable timeouts, which are 30 s by default.
 */
#define NAT_AGE				15

struct rtl8197f_nat_netif {
	u8 mac[ETH_ALEN];
	u8 ports;			/* member ports of its VLAN */
	unsigned int refs;
};

struct rtl8197f_nat_port {
	int netif;			/* whose VLAN is the port VLAN */
	unsigned int refs;
};

struct rtl8197f_nat_extip {
	__be32 addr;
	unsigned int refs;
};

struct rtl8197f_nat_l2 {
	u8 mac[ETH_ALEN];
	u8 port;
	unsigned int refs;
};

/* A LAN subnet: a route with a range of ARP entries, one per host */
struct rtl8197f_nat_subnet {
	__be32 net;
	u8 prefix;
	int netif;
	unsigned int arp_base;
	unsigned int refs;
};

/* An ARP entry holds a reference to the L2 entry of its host */
struct rtl8197f_nat_arp {
	int l2;
	unsigned int refs;
};

/* A next hop holds references to its interface, L2 entry and external IP */
struct rtl8197f_nat_nh {
	int netif;
	int l2;
	int extip;			/* external IP of outbound entries, or -1 */
	unsigned int refs;
};

/* One direction of a connection, as a flowtable rule describes it */
struct rtl8197f_nat_rule {
	int in_port;			/* ports of the external switch */
	int out_port;
	u8 proto;
	__be32 saddr, daddr;		/* as received */
	__be16 sport, dport;
	__be32 nat_saddr, nat_daddr;	/* as sent */
	__be16 nat_sport, nat_dport;
	struct ethhdr eth;		/* as sent */
	u8 in_mac[ETH_ALEN];		/* the router's MAC on the ingress port */
	unsigned int in_mtu, out_mtu;
	__be32 lan_net;			/* the subnet of the LAN host */
	u8 lan_prefix;
	u8 host_mac[ETH_ALEN];		/* the LAN host */
	int host_port;
};

/* One direction of a connection, as the NAPT table sees it */
struct rtl8197f_nat_conn {
	__be32 int_ip, rem_ip, ext_ip;
	__be16 int_port, rem_port, ext_port;
	bool tcp;
	bool outbound;
};

struct rtl8197f_nat_flow {
	struct hlist_node node;
	unsigned long cookie;
	struct rtl8197f_nat_conn conn;
	int napt;			/* the other indices hold references */
	int extip;
	int in_netif;
	int nh;
	int subnet;
	int arp;
	int in_port;
	int out_port;
	u32 entry[SW_TBL_NAPT_WORDS];	/* as written */
	unsigned long lastused;
};

struct rtl8197f_nat_stats {
	u64 added;
	u64 removed;
	u64 rewritten;
	u64 refused_rule;
	u64 refused_udp;
	u64 refused_route;
	u64 refused_host;
	u64 refused_busy;
	u64 refused_full;
	u64 hw_errors;
};

struct rtl8197f_nat {
	struct device *dev;
	struct net_device *ndev;
	void __iomem *swcore;
	void __iomem *tables;
	struct dentry *debugfs;
	bool udp_outbound;

	struct mutex lock;		/* everything below */
	unsigned int blocks;		/* bound flow blocks */
	unsigned int flows;
	int route_nh;			/* next hop of the default route */
	struct rtl8197f_nat_port port[NAT_PORTS];
	struct rtl8197f_nat_netif netif[SW_TBL_NETIFS];
	struct rtl8197f_nat_extip extip[SW_TBL_EXTIPS];
	struct rtl8197f_nat_nh nh[SW_TBL_NHS];
	struct rtl8197f_nat_subnet subnet[NAT_SUBNETS];
	struct rtl8197f_nat_arp arp[SW_TBL_ARPS];
	struct rtl8197f_nat_l2 *l2;
	struct rtl8197f_nat_flow *napt[SW_TBL_NAPTS];
	DECLARE_HASHTABLE(cookies, 8);
	struct rtl8197f_nat_stats stats;
};

static void swcore_rmw(void __iomem *swcore, unsigned int reg, u32 clr, u32 set)
{
	writel((readl(swcore + reg) & ~clr) | set, swcore + reg);
}

int rtl8197f_sw_tbl_write(void __iomem *swcore, unsigned int type,
			  unsigned int idx, const u32 *data, unsigned int words)
{
	u32 val;
	int ret, i;

	/* Stop the table lookup unit while the entry changes */
	swcore_rmw(swcore, SW_SWTCR0, 0, SW_SWTCR0_STOP_TLU);
	ret = readl_poll_timeout_atomic(swcore + SW_SWTCR0, val,
					val & SW_SWTCR0_STOP_TLU_READY, 1, 1000);
	if (!ret)
		ret = readl_poll_timeout_atomic(swcore + SW_SWTACR, val,
						!(val & SW_SWTACR_START), 1, 1000);
	if (!ret) {
		for (i = 0; i < words; i++)
			writel(data[i], swcore + SW_TCR(i));
		writel(SW_TBL_ADDR(type, idx), swcore + SW_SWTAA);
		writel(SW_SWTACR_START | SW_SWTACR_CMD_FORCE, swcore + SW_SWTACR);
		ret = readl_poll_timeout_atomic(swcore + SW_SWTACR, val,
						!(val & SW_SWTACR_START), 1, 1000);
	}
	swcore_rmw(swcore, SW_SWTCR0, SW_SWTCR0_STOP_TLU, 0);

	return ret;
}

static void nat_rmw(struct rtl8197f_nat *nat, unsigned int reg, u32 clr, u32 set)
{
	swcore_rmw(nat->swcore, reg, clr, set);
}

static int nat_write(struct rtl8197f_nat *nat, unsigned int type,
		     unsigned int idx, const u32 *data, unsigned int words)
{
	int ret;

	ret = rtl8197f_sw_tbl_write(nat->swcore, type, idx, data, words);
	if (ret) {
		nat->stats.hw_errors++;
		dev_err_ratelimited(nat->dev, "table %u entry %u: write timed out\n",
				    type, idx);
	}

	return ret;
}

static int nat_clear(struct rtl8197f_nat *nat, unsigned int type,
		     unsigned int idx, unsigned int words)
{
	static const u32 zero[SW_TBL_NETIF_WORDS];

	return nat_write(nat, type, idx, zero, words);
}

/* An unused NAPT entry, as the vendor initialises the table */
static int nat_napt_clear(struct rtl8197f_nat *nat, unsigned int idx)
{
	u32 entry[SW_TBL_NAPT_WORDS] = { 0, NAPT1_COLLISION | NAPT1_COLLISION2 };

	return nat_write(nat, SW_TBL_NAPT, idx, entry, ARRAY_SIZE(entry));
}

static void nat_napt_read(struct rtl8197f_nat *nat, unsigned int idx, u32 *entry)
{
	void __iomem *addr = nat->tables + SW_TBL_OFFSET(SW_TBL_NAPT, idx);
	int i, tries = 10;

	/* The switch core updates the age while the entry is read */
	do {
		for (i = 0; i < SW_TBL_NAPT_WORDS; i++)
			entry[i] = readl(addr + 4 * i);
	} while (readl(addr + 4) != entry[1] && --tries);

	/* The vendor reads an unused entry to refresh the read latch */
	readl(nat->tables + SW_TBL_OFFSET(SW_TBL_ACL, 1024));
}

/* Seconds of a NAPT aging time, as the vendor converts it */
static unsigned int nat_age_to_sec(unsigned int age)
{
	unsigned int unit = 0, exp = age >> 3, scale = 1;

	while (exp--) {
		unit += scale << 3;
		scale <<= 2;
	}
	unit += scale * ((age & 7) + 1);

	return (unit - 1) * 5 / 3 - (scale > 1 ? scale : 0);
}

/* Hash 1 of the NAPT table over host order values, as the vendor computes it */
static u32 nat_hash1(bool tcp, u32 sip, u16 sport, u32 dip, u16 dport)
{
	return ((sport & 0x3ff) ^ (sport >> 10 | (sip & 0xf) << 6) ^
		((sip >> 4) & 0x3ff) ^ ((sip >> 14) & 0x3ff) ^
		(sip >> 24 | tcp << 8 | (dport & 1) << 9) ^
		((dport >> 1) & 0x3ff) ^ ((dport >> 11) | (dip & 0x1f) << 5) ^
		((dip >> 5) & 0x3ff) ^ ((dip >> 15) & 0x3ff) ^ dip >> 25) & 0x3ff;
}

static u32 nat_hash_out(const struct rtl8197f_nat_conn *c)
{
	return nat_hash1(c->tcp, ntohl(c->int_ip), ntohs(c->int_port),
			 ntohl(c->rem_ip), ntohs(c->rem_port));
}

static u32 nat_hash_in(const struct rtl8197f_nat_conn *c)
{
	return nat_hash1(c->tcp, ntohl(c->rem_ip), ntohs(c->rem_port),
			 ntohl(c->ext_ip), ntohs(c->ext_port));
}

static void nat_set_pvid(struct rtl8197f_nat *nat, int port, u16 vid)
{
	if (port & 1)
		nat_rmw(nat, SW_PVCR(port / 2), SW_PVCR_PVID_ODD,
			FIELD_PREP(SW_PVCR_PVID_ODD, vid));
	else
		nat_rmw(nat, SW_PVCR(port / 2), SW_PVCR_PVID_EVEN,
			FIELD_PREP(SW_PVCR_PVID_EVEN, vid));
}

static int nat_vlan_write(struct rtl8197f_nat *nat, int netif, u8 ports)
{
	u32 vlan[SW_TBL_VLAN_WORDS] = {
		FIELD_PREP(SW_VLAN0_MEMBER, ports) |
		FIELD_PREP(SW_VLAN0_EXT_MEMBER, SW_VLAN_EXT_CPU) |
		FIELD_PREP(SW_VLAN0_UNTAG, FIELD_MAX(SW_VLAN0_UNTAG)) |
		FIELD_PREP(SW_VLAN0_EXT_UNTAG, FIELD_MAX(SW_VLAN0_EXT_UNTAG)),
	};

	return nat_write(nat, SW_TBL_VLAN, NAT_VID(netif), vlan, ARRAY_SIZE(vlan));
}

static int nat_netif_get(struct rtl8197f_nat *nat, const u8 *mac,
			 unsigned int mtu)
{
	struct rtl8197f_nat_netif *netif;
	u32 entry[SW_TBL_NETIF_WORDS] = {};
	int i, idx = -ENOSPC, ret;

	for (i = 0; i < SW_TBL_NETIFS; i++) {
		netif = &nat->netif[i];
		if (!netif->refs) {
			if (idx < 0)
				idx = i;
		} else if (ether_addr_equal(netif->mac, mac)) {
			netif->refs++;
			return i;
		}
	}
	if (idx < 0)
		return idx;

	entry[0] = NETIF0_VALID | FIELD_PREP(NETIF0_VID, NAT_VID(idx)) |
		   FIELD_PREP(NETIF0_MAC_LOW,
			      (mac[3] << 16 | mac[4] << 8 | mac[5]) & 0x7ffff);
	entry[1] = NETIF1_ROUTE |
		   FIELD_PREP(NETIF1_MAC_HIGH, mac[0] << 21 | mac[1] << 13 |
					       mac[2] << 5 | mac[3] >> 3) |
		   FIELD_PREP(NETIF1_IN_ACL_START_L, NETIF_ACL & 1);
	entry[2] = FIELD_PREP(NETIF2_IN_ACL_START_H, NETIF_ACL >> 1) |
		   FIELD_PREP(NETIF2_IN_ACL_END, NETIF_ACL) |
		   FIELD_PREP(NETIF2_OUT_ACL_START, NETIF_ACL) |
		   FIELD_PREP(NETIF2_OUT_ACL_END, NETIF_ACL) |
		   FIELD_PREP(NETIF2_MAC_MASK_L, NETIF_MAC_MASK_ONE & 1);
	entry[3] = FIELD_PREP(NETIF3_MAC_MASK_H, NETIF_MAC_MASK_ONE >> 1) |
		   FIELD_PREP(NETIF3_MTU, mtu);

	/* A VLAN with only the CPU, until ports join */
	ret = nat_vlan_write(nat, idx, 0);
	if (!ret)
		ret = nat_write(nat, SW_TBL_NETIF, idx, entry, ARRAY_SIZE(entry));
	if (ret)
		return ret;

	netif = &nat->netif[idx];
	ether_addr_copy(netif->mac, mac);
	netif->ports = 0;
	netif->refs = 1;

	return idx;
}

static void nat_netif_put(struct rtl8197f_nat *nat, int idx)
{
	if (--nat->netif[idx].refs)
		return;

	nat_clear(nat, SW_TBL_NETIF, idx, SW_TBL_NETIF_WORDS);
	nat_clear(nat, SW_TBL_VLAN, NAT_VID(idx), SW_TBL_VLAN_WORDS);
}

/* Make the VLAN of an interface the port VLAN of a port */
static int nat_port_get(struct rtl8197f_nat *nat, int port, int netif)
{
	struct rtl8197f_nat_port *p = &nat->port[port];
	u8 ports = nat->netif[netif].ports | BIT(port);
	int ret;

	if (p->refs) {
		if (p->netif != netif)
			return -EBUSY;
		p->refs++;
		return 0;
	}

	/* Membership first, so that routed frames can leave the port */
	ret = nat_vlan_write(nat, netif, ports);
	if (ret)
		return ret;
	nat->netif[netif].ports = ports;
	nat_set_pvid(nat, port, NAT_VID(netif));

	p->netif = netif;
	p->refs = 1;

	return 0;
}

static void nat_port_put(struct rtl8197f_nat *nat, int port)
{
	struct rtl8197f_nat_port *p = &nat->port[port];
	struct rtl8197f_nat_netif *netif = &nat->netif[p->netif];

	if (--p->refs)
		return;

	nat_set_pvid(nat, port, RTL8197F_ETH_VID);
	netif->ports &= ~BIT(port);
	nat_vlan_write(nat, p->netif, netif->ports);
}

static int nat_extip_get(struct rtl8197f_nat *nat, __be32 addr)
{
	struct rtl8197f_nat_extip *extip;
	u32 entry[SW_TBL_EXTIP_WORDS] = { 0, ntohl(addr), EXTIP2_VALID };
	int i, idx = -ENOSPC, ret;

	for (i = 0; i < SW_TBL_EXTIPS; i++) {
		extip = &nat->extip[i];
		if (!extip->refs) {
			if (idx < 0)
				idx = i;
		} else if (extip->addr == addr) {
			extip->refs++;
			return i;
		}
	}
	if (idx < 0)
		return idx;

	ret = nat_write(nat, SW_TBL_EXTIP, idx, entry, ARRAY_SIZE(entry));
	if (ret)
		return ret;

	nat->extip[idx].addr = addr;
	nat->extip[idx].refs = 1;

	return idx;
}

static void nat_extip_put(struct rtl8197f_nat *nat, int idx)
{
	if (!--nat->extip[idx].refs)
		nat_clear(nat, SW_TBL_EXTIP, idx, SW_TBL_EXTIP_WORDS);
}

static int nat_l2_write(struct rtl8197f_nat *nat, int idx, const u8 *mac,
			int port)
{
	u32 entry[SW_TBL_L2_WORDS] = {
		mac[1] << 24 | mac[2] << 16 | mac[3] << 8 | mac[4],
		FIELD_PREP(L2_1_MAC0, mac[0]) |
		FIELD_PREP(L2_1_MEMBER, BIT(port)) |
		FIELD_PREP(L2_1_AGING, 3) | L2_1_STATIC | L2_1_NEXT_HOP |
		L2_1_AUTH,
	};

	return nat_write(nat, SW_TBL_L2, idx, entry, ARRAY_SIZE(entry));
}

/* A static L2 entry of a next hop, in the row that its MAC address hashes to */
static int nat_l2_get(struct rtl8197f_nat *nat, const u8 *mac, int port)
{
	unsigned int row = mac[0] ^ mac[1] ^ mac[2] ^ mac[3] ^ mac[4] ^ mac[5];
	struct rtl8197f_nat_l2 *l2;
	int way, i, idx = -ENOSPC, ret;

	for (way = 0; way < NAT_WAYS; way++) {
		i = row * NAT_WAYS + way;
		l2 = &nat->l2[i];
		if (i == NAT_L2_BCAST)
			continue;
		if (!l2->refs) {
			if (idx < 0)
				idx = i;
		} else if (ether_addr_equal(l2->mac, mac)) {
			/* The host moved: move all connections to it */
			if (l2->port != port) {
				ret = nat_l2_write(nat, i, mac, port);
				if (ret)
					return ret;
				l2->port = port;
			}
			l2->refs++;
			return i;
		}
	}
	if (idx < 0)
		return idx;

	ret = nat_l2_write(nat, idx, mac, port);
	if (ret)
		return ret;

	l2 = &nat->l2[idx];
	ether_addr_copy(l2->mac, mac);
	l2->port = port;
	l2->refs = 1;

	return idx;
}

static void nat_l2_put(struct rtl8197f_nat *nat, int idx)
{
	if (!--nat->l2[idx].refs)
		nat_clear(nat, SW_TBL_L2, idx, SW_TBL_L2_WORDS);
}

static u32 nat_nh_entry(const struct rtl8197f_nat_nh *nh)
{
	return FIELD_PREP(NH_EXTIP, nh->extip < 0 ? 0 : nh->extip) |
	       FIELD_PREP(NH_NETIF, nh->netif) | FIELD_PREP(NH_L2, nh->l2);
}

static void nat_nh_put_refs(struct rtl8197f_nat *nat, int netif, int l2,
			    int extip)
{
	nat_netif_put(nat, netif);
	nat_l2_put(nat, l2);
	if (extip >= 0)
		nat_extip_put(nat, extip);
}

/* Takes over the references to the interface, L2 entry and external IP */
static int nat_nh_get(struct rtl8197f_nat *nat, int netif, int l2, int extip)
{
	struct rtl8197f_nat_nh *nh;
	int i, idx = -ENOSPC, ret;
	u32 entry;

	for (i = NAT_NH_FIRST; i < SW_TBL_NHS; i++) {
		nh = &nat->nh[i];
		if (!nh->refs) {
			if (idx < 0)
				idx = i;
		} else if (nh->netif == netif && nh->l2 == l2 &&
			   nh->extip == extip) {
			nh->refs++;
			nat_nh_put_refs(nat, netif, l2, extip);
			return i;
		}
	}
	if (idx < 0) {
		nat_nh_put_refs(nat, netif, l2, extip);
		return idx;
	}

	nh = &nat->nh[idx];
	nh->netif = netif;
	nh->l2 = l2;
	nh->extip = extip;
	entry = nat_nh_entry(nh);
	ret = nat_write(nat, SW_TBL_NH, idx, &entry, 1);
	if (ret) {
		nat_nh_put_refs(nat, netif, l2, extip);
		return ret;
	}
	nh->refs = 1;

	return idx;
}

static void nat_nh_put(struct rtl8197f_nat *nat, int idx)
{
	struct rtl8197f_nat_nh *nh = &nat->nh[idx];

	if (--nh->refs)
		return;

	nat_clear(nat, SW_TBL_NH, idx, SW_TBL_NH_WORDS);
	nat_nh_put_refs(nat, nh->netif, nh->l2, nh->extip);
}

/* The next hop of a rule, with its interface, L2 entry and external IP */
static int nat_nh_get_rule(struct rtl8197f_nat *nat,
			   const struct rtl8197f_nat_rule *rule,
			   const struct rtl8197f_nat_conn *conn)
{
	int netif, l2, extip = -1;

	netif = nat_netif_get(nat, rule->eth.h_source, rule->out_mtu);
	if (netif < 0)
		return netif;

	l2 = nat_l2_get(nat, rule->eth.h_dest, rule->out_port);
	if (l2 < 0) {
		nat_netif_put(nat, netif);
		return l2;
	}

	/* Outbound entries take the external IP from their next hop */
	if (conn->outbound) {
		extip = nat_extip_get(nat, conn->ext_ip);
		if (extip < 0) {
			nat_l2_put(nat, l2);
			nat_netif_put(nat, netif);
			return extip;
		}
	}

	return nat_nh_get(nat, netif, l2, extip);
}

/*
 * The default route sends packets to the NAPT engine, which takes the next
 * hop from the NAPT entry. The route still needs a next hop that does not
 * lead to the CPU, or everything goes to the CPU. As in the vendor driver it
 * is the WAN gateway, taken from an outbound direction, even one that is not
 * offloaded itself. Takes over the reference to the next hop.
 */
static int nat_route_set(struct rtl8197f_nat *nat, int nh)
{
	u32 entry = nat_nh_entry(&nat->nh[nh]);
	u32 route[SW_TBL_ROUTE_WORDS] = {
		0,
		ROUTE1_VALID | FIELD_PREP(ROUTE1_PROCESS, ROUTE_PROCESS_NAPT) |
		FIELD_PREP(ROUTE1_NH_START, NAT_NH_ROUTE / 2) |
		FIELD_PREP(ROUTE1_NH_ALGO, ROUTE_NH_ALGO_SIP) |
		FIELD_PREP(ROUTE1_IP_DOMAIN, ROUTE_IP_DOMAIN),
	};
	int ret;

	ret = nat_write(nat, SW_TBL_NH, NAT_NH_ROUTE, &entry, 1);
	if (!ret)
		ret = nat_write(nat, SW_TBL_NH, NAT_NH_ROUTE + 1, &entry, 1);
	if (!ret)
		ret = nat_write(nat, SW_TBL_ROUTE, NAT_ROUTE_DEFAULT, route,
				ARRAY_SIZE(route));
	if (ret) {
		nat_clear(nat, SW_TBL_NH, NAT_NH_ROUTE, SW_TBL_NH_WORDS);
		nat_clear(nat, SW_TBL_NH, NAT_NH_ROUTE + 1, SW_TBL_NH_WORDS);
		nat_nh_put(nat, nh);
		return ret;
	}

	nat->route_nh = nh;

	return 0;
}

static int nat_route_learn(struct rtl8197f_nat *nat,
			   const struct rtl8197f_nat_rule *rule,
			   const struct rtl8197f_nat_conn *conn)
{
	int nh;

	if (nat->route_nh >= 0 || !conn->outbound)
		return 0;

	nh = nat_nh_get_rule(nat, rule, conn);
	if (nh < 0)
		return nh;

	return nat_route_set(nat, nh);
}

static void nat_route_clear(struct rtl8197f_nat *nat)
{
	nat_clear(nat, SW_TBL_ROUTE, NAT_ROUTE_DEFAULT, SW_TBL_ROUTE_WORDS);
	nat_clear(nat, SW_TBL_NH, NAT_NH_ROUTE, SW_TBL_NH_WORDS);
	nat_clear(nat, SW_TBL_NH, NAT_NH_ROUTE + 1, SW_TBL_NH_WORDS);
	nat_nh_put(nat, nat->route_nh);
	nat->route_nh = -1;
}

static unsigned int nat_subnet_size(u8 prefix)
{
	return max_t(unsigned int, 1U << (32 - prefix), NAT_ARP_BLOCK);
}

/*
 * The switch core only translates packets of hosts it knows as LAN hosts: a
 * route to their subnet, marked internal, resolves them to a range of ARP
 * entries. Without the route, packets from the LAN that miss the NAPT table
 * are routed out untranslated and packets to the router do not reach the
 * CPU; they do with the route, as do packets to hosts without an ARP entry.
 */
static int nat_subnet_get(struct rtl8197f_nat *nat, __be32 net, u8 prefix,
			  int netif)
{
	u32 route[SW_TBL_ROUTE_WORDS] = {};
	struct rtl8197f_nat_subnet *sn;
	unsigned int size, base;
	int i, idx = -ENOSPC, ret;

	if (prefix < NAT_SUBNET_PREFIX_MIN || prefix > 32)
		return -EOPNOTSUPP;

	for (i = 0; i < NAT_SUBNETS; i++) {
		sn = &nat->subnet[i];
		if (!sn->refs) {
			if (idx < 0)
				idx = i;
		} else if (sn->net == net && sn->prefix == prefix) {
			if (sn->netif != netif)
				return -EBUSY;
			sn->refs++;
			return i;
		}
	}
	if (idx < 0)
		return idx;

	/* The first free range of ARP entries */
	size = nat_subnet_size(prefix);
	for (base = 0; base + size <= SW_TBL_ARPS; base += NAT_ARP_BLOCK) {
		for (i = 0; i < NAT_SUBNETS; i++) {
			sn = &nat->subnet[i];
			if (sn->refs && base < sn->arp_base + nat_subnet_size(sn->prefix) &&
			    sn->arp_base < base + size)
				break;
		}
		if (i == NAT_SUBNETS)
			break;
	}
	if (base + size > SW_TBL_ARPS)
		return -ENOSPC;

	route[0] = ntohl(net);
	route[1] = ROUTE1_VALID | FIELD_PREP(ROUTE1_PREFIX, prefix - 1) |
		   FIELD_PREP(ROUTE1_PROCESS, ROUTE_PROCESS_ARP) |
		   ROUTE1_INTERNAL | FIELD_PREP(ROUTE1_NETIF, netif) |
		   FIELD_PREP(ROUTE1_ARP_START, base / NAT_ARP_BLOCK) |
		   FIELD_PREP(ROUTE1_ARP_END, (base + size - 1) / NAT_ARP_BLOCK);
	ret = nat_write(nat, SW_TBL_ROUTE, idx, route, ARRAY_SIZE(route));
	if (ret)
		return ret;

	sn = &nat->subnet[idx];
	sn->net = net;
	sn->prefix = prefix;
	sn->netif = netif;
	sn->arp_base = base;
	sn->refs = 1;

	return idx;
}

static void nat_subnet_put(struct rtl8197f_nat *nat, int idx)
{
	if (!--nat->subnet[idx].refs)
		nat_clear(nat, SW_TBL_ROUTE, idx, SW_TBL_ROUTE_WORDS);
}

static int nat_arp_write(struct rtl8197f_nat *nat, int idx, int l2)
{
	u32 entry = ARP_VALID | FIELD_PREP(ARP_L2, l2) |
		    FIELD_PREP(ARP_AGE, ARP_AGE_MAX);

	return nat_write(nat, SW_TBL_ARP, idx, &entry, SW_TBL_ARP_WORDS);
}

/*
 * The switch core ages ARP entries and reloads them on use. Without its ARP
 * entry, a host's NAPT entries are not used, so keep the entry as long as
 * connections of the host exist.
 */
static void nat_arp_refresh(struct rtl8197f_nat *nat, int idx)
{
	u32 entry = readl(nat->tables + SW_TBL_OFFSET(SW_TBL_ARP, idx));

	if (!(entry & ARP_VALID) || FIELD_GET(ARP_AGE, entry) < ARP_AGE_REFRESH)
		nat_arp_write(nat, idx, nat->arp[idx].l2);
}

/* The ARP entry of a LAN host. Takes over the reference to its L2 entry. */
static int nat_arp_get(struct rtl8197f_nat *nat, int subnet, __be32 addr,
		       int l2)
{
	const struct rtl8197f_nat_subnet *sn = &nat->subnet[subnet];
	u32 host = sn->prefix < 32 ? ntohl(addr) & (~0U >> sn->prefix) : 0;
	int idx = sn->arp_base + host;
	struct rtl8197f_nat_arp *arp = &nat->arp[idx];
	int ret;

	if (arp->refs && arp->l2 == l2) {
		arp->refs++;
		nat_l2_put(nat, l2);
		return idx;
	}

	/* A new entry, or the address has moved to another host */
	ret = nat_arp_write(nat, idx, l2);
	if (ret) {
		nat_l2_put(nat, l2);
		return ret;
	}
	if (arp->refs)
		nat_l2_put(nat, arp->l2);
	arp->l2 = l2;
	arp->refs++;

	return idx;
}

static void nat_arp_put(struct rtl8197f_nat *nat, int idx)
{
	struct rtl8197f_nat_arp *arp = &nat->arp[idx];

	if (--arp->refs)
		return;

	nat_clear(nat, SW_TBL_ARP, idx, SW_TBL_ARP_WORDS);
	nat_l2_put(nat, arp->l2);
}

/*
 * An inbound entry can only be at the index its hash gives. An outbound entry
 * can be anywhere in the group of four around its hash, but the switch core
 * matches it by the local address and port only.
 */
static int nat_napt_place(struct rtl8197f_nat *nat,
			  const struct rtl8197f_nat_conn *c)
{
	struct rtl8197f_nat_flow *other;
	u32 hash, in, idx;
	int i, free = -ENOSPC;

	in = nat_hash_in(c);
	if (!c->outbound)
		return nat->napt[in] ? -ENOSPC : in;

	hash = nat_hash_out(c);
	for (i = 0; i < NAT_WAYS; i++) {
		idx = round_down(hash, NAT_WAYS) + (hash + i) % NAT_WAYS;
		other = nat->napt[idx];
		if (!other) {
			/* Leave the index of the inbound entry free if possible */
			if (free < 0 || free == in)
				free = idx;
		} else if (other->conn.outbound && other->conn.tcp == c->tcp &&
			   other->conn.int_ip == c->int_ip &&
			   other->conn.int_port == c->int_port) {
			return -EBUSY;
		}
	}

	return free;
}

static void nat_napt_entry(const struct rtl8197f_nat_flow *flow, u32 *entry)
{
	const struct rtl8197f_nat_conn *c = &flow->conn;
	u16 ext_port = ntohs(c->ext_port);
	u32 very;

	entry[0] = ntohl(c->int_ip);
	entry[1] = NAPT1_VALID | NAPT1_COLLISION | NAPT1_COLLISION2 |
		   NAPT1_STATIC | FIELD_PREP(NAPT1_AGE, NAT_AGE);
	entry[2] = FIELD_PREP(NAPT2_INT_PORT, ntohs(c->int_port)) |
		   NAPT2_NH_VALID | FIELD_PREP(NAPT2_NH, flow->nh);
	if (c->tcp)
		entry[2] |= NAPT2_TCP;

	if (c->outbound) {
		/*
		 * The external port. The switch core ignores the index of the
		 * external IP here; the vendor sets it anyway.
		 */
		entry[1] |= FIELD_PREP(NAPT1_OFFSET, ext_port >> 10) |
			    FIELD_PREP(NAPT1_SEL_E, ext_port & 0x3ff) |
			    FIELD_PREP(NAPT1_SEL_IP, flow->extip);
		entry[2] |= FIELD_PREP(NAPT2_TCP_FLAG, NAPT_TCP_FLAG_OUTBOUND);
	} else {
		/* Low bits of the external port and a hash of the remote */
		very = nat_hash1(c->tcp, ntohl(c->rem_ip), ntohs(c->rem_port),
				 0, 0);
		entry[1] |= FIELD_PREP(NAPT1_OFFSET, ext_port & 0x3f) |
			    FIELD_PREP(NAPT1_SEL_IP, (ext_port & 0x3ff) >> 6) |
			    FIELD_PREP(NAPT1_SEL_E, very);
		entry[2] |= FIELD_PREP(NAPT2_TCP_FLAG, NAPT_TCP_FLAG_INBOUND);
	}
}

static struct rtl8197f_nat_flow *nat_flow_find(struct rtl8197f_nat *nat,
					       unsigned long cookie)
{
	struct rtl8197f_nat_flow *flow;

	hash_for_each_possible(nat->cookies, flow, node, cookie)
		if (flow->cookie == cookie)
			return flow;

	return NULL;
}

/* Remove a connection from the switch core and drop its references */
static void nat_flow_put(struct rtl8197f_nat *nat, struct rtl8197f_nat_flow *flow)
{
	if (flow->napt >= 0) {
		nat_napt_clear(nat, flow->napt);
		nat->napt[flow->napt] = NULL;
		hash_del(&flow->node);
		nat->flows--;
	}
	if (flow->out_port >= 0)
		nat_port_put(nat, flow->out_port);
	if (flow->in_port >= 0)
		nat_port_put(nat, flow->in_port);
	if (flow->arp >= 0)
		nat_arp_put(nat, flow->arp);
	if (flow->subnet >= 0)
		nat_subnet_put(nat, flow->subnet);
	if (flow->nh >= 0)
		nat_nh_put(nat, flow->nh);
	if (flow->in_netif >= 0)
		nat_netif_put(nat, flow->in_netif);
	if (flow->extip >= 0)
		nat_extip_put(nat, flow->extip);

	if (!nat->flows && nat->route_nh >= 0)
		nat_route_clear(nat);

	kfree(flow);
}

static int nat_flow_add(struct rtl8197f_nat *nat, unsigned long cookie,
			const struct rtl8197f_nat_rule *rule,
			const struct rtl8197f_nat_conn *conn)
{
	struct rtl8197f_nat_flow *flow;
	int napt, lan_netif, l2, ret;

	napt = nat_napt_place(nat, conn);
	if (napt < 0)
		return napt;

	flow = kzalloc(sizeof(*flow), GFP_KERNEL);
	if (!flow)
		return -ENOMEM;

	flow->cookie = cookie;
	flow->conn = *conn;
	flow->napt = -1;
	flow->nh = -1;
	flow->subnet = -1;
	flow->arp = -1;
	flow->in_port = -1;
	flow->out_port = -1;

	flow->extip = nat_extip_get(nat, conn->ext_ip);
	flow->in_netif = nat_netif_get(nat, rule->in_mac, rule->in_mtu);
	if (flow->extip < 0 || flow->in_netif < 0) {
		ret = flow->extip < 0 ? flow->extip : flow->in_netif;
		goto err;
	}

	flow->nh = nat_nh_get_rule(nat, rule, conn);
	if (flow->nh < 0) {
		ret = flow->nh;
		goto err;
	}

	/* The route and ARP entry of the LAN host, before its port routes */
	lan_netif = conn->outbound ? flow->in_netif : nat->nh[flow->nh].netif;
	flow->subnet = nat_subnet_get(nat, rule->lan_net, rule->lan_prefix,
				      lan_netif);
	if (flow->subnet < 0) {
		ret = flow->subnet;
		goto err;
	}
	l2 = nat_l2_get(nat, rule->host_mac, rule->host_port);
	if (l2 < 0) {
		ret = l2;
		goto err;
	}
	flow->arp = nat_arp_get(nat, flow->subnet, conn->int_ip, l2);
	if (flow->arp < 0) {
		ret = flow->arp;
		goto err;
	}

	ret = nat_port_get(nat, rule->in_port, flow->in_netif);
	if (ret)
		goto err;
	flow->in_port = rule->in_port;
	ret = nat_port_get(nat, rule->out_port, nat->nh[flow->nh].netif);
	if (ret)
		goto err;
	flow->out_port = rule->out_port;

	nat_napt_entry(flow, flow->entry);
	ret = nat_write(nat, SW_TBL_NAPT, napt, flow->entry,
			ARRAY_SIZE(flow->entry));
	if (ret)
		goto err;

	flow->napt = napt;
	flow->lastused = jiffies;
	nat->napt[napt] = flow;
	hash_add(nat->cookies, &flow->node, cookie);
	nat->flows++;
	nat->stats.added++;

	return 0;

err:
	nat_flow_put(nat, flow);

	return ret;
}

/* Whether the switch core still has the entry of a connection */
static bool nat_flow_in_hw(const struct rtl8197f_nat_flow *flow,
			   const u32 *entry)
{
	return (entry[1] & NAPT1_VALID) && entry[0] == flow->entry[0] &&
	       FIELD_GET(NAPT2_INT_PORT, entry[2]) ==
	       FIELD_GET(NAPT2_INT_PORT, flow->entry[2]);
}

/*
 * The flowtable offloads a connection again when the CPU forwards a packet of
 * it. Write the entry again if the switch core has aged it out.
 */
static int nat_flow_refresh(struct rtl8197f_nat *nat,
			    struct rtl8197f_nat_flow *flow)
{
	u32 entry[SW_TBL_NAPT_WORDS];
	int ret;

	nat_arp_refresh(nat, flow->arp);
	nat_napt_read(nat, flow->napt, entry);
	if (nat_flow_in_hw(flow, entry))
		return 0;

	ret = nat_write(nat, SW_TBL_NAPT, flow->napt, flow->entry,
			ARRAY_SIZE(flow->entry));
	if (!ret)
		nat->stats.rewritten++;

	return ret;
}

/* The port of the external switch behind a DSA user port of ours */
static int nat_dev_port(struct rtl8197f_nat *nat, struct net_device *dev)
{
	struct dsa_port *dp;

	if (!IS_ENABLED(CONFIG_NET_DSA))
		return -EOPNOTSUPP;
	if (!dev)
		return -ENODEV;

	dp = dsa_port_from_netdev(dev);
	if (IS_ERR(dp) || dsa_port_to_conduit(dp) != nat->ndev ||
	    dp->index >= NAT_PORTS)
		return -EOPNOTSUPP;

	return dp->index;
}

/* The device with the router's address on a DSA user port, under RCU */
static struct net_device *nat_l3_dev(struct net_device *dev)
{
	struct net_device *upper = netdev_master_upper_dev_get_rcu(dev);

	return upper && netif_is_bridge_master(upper) ? upper : dev;
}

static int nat_rule_mangle(struct rtl8197f_nat_rule *rule,
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
		dst = (u8 *)&rule->eth + act->mangle.offset;
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
			rule->nat_saddr = (__force __be32)act->mangle.val;
		else if (act->mangle.offset == offsetof(struct iphdr, daddr))
			rule->nat_daddr = (__force __be32)act->mangle.val;
		else
			return -EOPNOTSUPP;
		return 0;
	case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
	case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
		/* Both ports are in the first word, the mask keeps the other */
		if (act->mangle.offset)
			return -EOPNOTSUPP;
		if (act->mangle.mask == ~htonl(0xffff0000))
			rule->nat_sport = htons(val >> 16);
		else if (act->mangle.mask == ~htonl(0xffff))
			rule->nat_dport = htons(val & 0xffff);
		else
			return -EOPNOTSUPP;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* NAPT terms of a direction that is either source or destination NATed */
static int nat_rule_conn(const struct rtl8197f_nat_rule *rule,
			 struct rtl8197f_nat_conn *c)
{
	bool snat = rule->saddr != rule->nat_saddr || rule->sport != rule->nat_sport;
	bool dnat = rule->daddr != rule->nat_daddr || rule->dport != rule->nat_dport;

	c->tcp = rule->proto == IPPROTO_TCP;

	if (snat && !dnat) {
		c->outbound = true;
		c->int_ip = rule->saddr;
		c->int_port = rule->sport;
		c->rem_ip = rule->daddr;
		c->rem_port = rule->dport;
		c->ext_ip = rule->nat_saddr;
		c->ext_port = rule->nat_sport;
	} else if (dnat && !snat) {
		c->outbound = false;
		c->rem_ip = rule->saddr;
		c->rem_port = rule->sport;
		c->ext_ip = rule->daddr;
		c->ext_port = rule->dport;
		c->int_ip = rule->nat_daddr;
		c->int_port = rule->nat_dport;
	} else {
		return -EOPNOTSUPP;
	}

	return 0;
}

/* The LAN host of a direction: its subnet, MAC address and port, under RCU */
static int nat_rule_host(struct rtl8197f_nat_rule *rule,
			 const struct rtl8197f_nat_conn *conn,
			 struct net_device *lan_dev)
{
	struct in_device *in_dev = __in_dev_get_rcu(lan_dev);
	struct in_ifaddr *ifa;
	struct neighbour *n;

	if (!in_dev)
		return -EHOSTUNREACH;

	in_dev_for_each_ifa_rcu(ifa, in_dev) {
		if (inet_ifa_match(conn->int_ip, ifa)) {
			rule->lan_net = ifa->ifa_address & ifa->ifa_mask;
			rule->lan_prefix = ifa->ifa_prefixlen;
			break;
		}
	}
	if (!rule->lan_prefix)
		return -EHOSTUNREACH;

	if (!conn->outbound) {
		ether_addr_copy(rule->host_mac, rule->eth.h_dest);
		rule->host_port = rule->out_port;
		return 0;
	}

	/* The outbound direction knows the host by its address only */
	n = __ipv4_neigh_lookup_noref(lan_dev, (__force u32)conn->int_ip);
	if (!n || !(READ_ONCE(n->nud_state) & NUD_VALID))
		return -EHOSTUNREACH;
	neigh_ha_snapshot(rule->host_mac, n, lan_dev);
	rule->host_port = rule->in_port;

	return 0;
}

static int nat_rule_parse(struct rtl8197f_nat *nat, struct flow_cls_offload *cls,
			  struct rtl8197f_nat_rule *rule,
			  struct rtl8197f_nat_conn *conn)
{
	struct flow_rule *fr = flow_cls_offload_flow_rule(cls);
	struct flow_match_ipv4_addrs addrs;
	struct flow_match_control control;
	struct flow_match_basic basic;
	struct flow_match_ports ports;
	struct flow_match_meta meta;
	struct net_device *dev, *in_l3, *out_l3, *out_dev = NULL;
	struct flow_action_entry *act;
	int i, ret;

	memset(rule, 0, sizeof(*rule));

	/* Routed IPv4 TCP and UDP, without VLAN or PPPoE */
	if (!flow_rule_match_key(fr, FLOW_DISSECTOR_KEY_META) ||
	    !flow_rule_match_key(fr, FLOW_DISSECTOR_KEY_CONTROL) ||
	    !flow_rule_match_key(fr, FLOW_DISSECTOR_KEY_BASIC) ||
	    !flow_rule_match_key(fr, FLOW_DISSECTOR_KEY_PORTS) ||
	    flow_rule_match_key(fr, FLOW_DISSECTOR_KEY_VLAN))
		return -EOPNOTSUPP;

	flow_rule_match_control(fr, &control);
	if (control.key->addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS)
		return -EOPNOTSUPP;

	flow_rule_match_basic(fr, &basic);
	rule->proto = basic.key->ip_proto;
	if (rule->proto != IPPROTO_TCP && rule->proto != IPPROTO_UDP)
		return -EOPNOTSUPP;

	/* The mangle actions below change what is sent */
	flow_rule_match_ipv4_addrs(fr, &addrs);
	rule->saddr = addrs.key->src;
	rule->daddr = addrs.key->dst;
	rule->nat_saddr = rule->saddr;
	rule->nat_daddr = rule->daddr;
	flow_rule_match_ports(fr, &ports);
	rule->sport = ports.key->src;
	rule->dport = ports.key->dst;
	rule->nat_sport = rule->sport;
	rule->nat_dport = rule->dport;

	flow_action_for_each(i, act, &fr->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			ret = nat_rule_mangle(rule, act);
			if (ret)
				return ret;
			break;
		case FLOW_ACTION_REDIRECT:
			out_dev = act->dev;
			break;
		case FLOW_ACTION_CSUM:
			break;
		default:
			return -EOPNOTSUPP;
		}
	}

	if (!is_valid_ether_addr(rule->eth.h_source) ||
	    !is_valid_ether_addr(rule->eth.h_dest))
		return -EINVAL;

	flow_rule_match_meta(fr, &meta);
	rcu_read_lock();
	dev = dev_get_by_index_rcu(dev_net(nat->ndev), meta.key->ingress_ifindex);
	rule->in_port = nat_dev_port(nat, dev);
	rule->out_port = nat_dev_port(nat, out_dev);
	if (rule->in_port < 0) {
		ret = rule->in_port;
	} else if (rule->out_port < 0) {
		ret = rule->out_port;
	} else if (rule->in_port == rule->out_port) {
		ret = -EOPNOTSUPP;
	} else {
		in_l3 = nat_l3_dev(dev);
		out_l3 = nat_l3_dev(out_dev);
		ether_addr_copy(rule->in_mac, in_l3->dev_addr);
		rule->in_mtu = READ_ONCE(in_l3->mtu);
		rule->out_mtu = READ_ONCE(out_l3->mtu);
		ret = nat_rule_conn(rule, conn);
		if (!ret)
			ret = nat_rule_host(rule, conn,
					    conn->outbound ? in_l3 : out_l3);
	}
	rcu_read_unlock();

	return ret;
}

static int nat_flow_replace(struct rtl8197f_nat *nat,
			    struct flow_cls_offload *cls)
{
	struct rtl8197f_nat_flow *flow;
	struct rtl8197f_nat_rule rule;
	struct rtl8197f_nat_conn conn;
	int ret;

	ret = nat_rule_parse(nat, cls, &rule, &conn);

	mutex_lock(&nat->lock);
	if (!ret || ret == -EHOSTUNREACH)
		nat_route_learn(nat, &rule, &conn);

	if (ret == -EHOSTUNREACH) {
		nat->stats.refused_host++;
	} else if (ret) {
		nat->stats.refused_rule++;
	} else if (conn.outbound && !conn.tcp && !READ_ONCE(nat->udp_outbound)) {
		nat->stats.refused_udp++;
		ret = -EOPNOTSUPP;
	} else if (nat->route_nh < 0) {
		/* Until an outbound direction shows the way to the WAN */
		nat->stats.refused_route++;
		ret = -EOPNOTSUPP;
	} else {
		flow = nat_flow_find(nat, cls->cookie);
		if (flow)
			ret = nat_flow_refresh(nat, flow);
		else
			ret = nat_flow_add(nat, cls->cookie, &rule, &conn);
		if (ret == -EBUSY)
			nat->stats.refused_busy++;
		else if (ret == -ENOSPC)
			nat->stats.refused_full++;
	}
	mutex_unlock(&nat->lock);

	return ret;
}

static int nat_flow_destroy(struct rtl8197f_nat *nat,
			    struct flow_cls_offload *cls)
{
	struct rtl8197f_nat_flow *flow;
	int ret = -ENOENT;

	mutex_lock(&nat->lock);
	flow = nat_flow_find(nat, cls->cookie);
	if (flow) {
		nat_flow_put(nat, flow);
		nat->stats.removed++;
		ret = 0;
	}
	mutex_unlock(&nat->lock);

	return ret;
}

/*
 * The switch core counts no packets per entry. Hits reload the age of an
 * entry, which then counts down, so the age tells when it was last used.
 */
static int nat_flow_stats(struct rtl8197f_nat *nat, struct flow_cls_offload *cls)
{
	struct rtl8197f_nat_flow *flow;
	u32 entry[SW_TBL_NAPT_WORDS];
	unsigned int age, idle;
	unsigned long lastused;
	int ret = -ENOENT;

	mutex_lock(&nat->lock);
	flow = nat_flow_find(nat, cls->cookie);
	if (flow) {
		nat_arp_refresh(nat, flow->arp);
		nat_napt_read(nat, flow->napt, entry);
		age = FIELD_GET(NAPT1_AGE, entry[1]);
		if (nat_flow_in_hw(flow, entry)) {
			idle = age < NAT_AGE ? nat_age_to_sec(NAT_AGE) -
					       nat_age_to_sec(age) : 0;
			lastused = jiffies - idle * HZ;
			if (time_after(lastused, flow->lastused))
				flow->lastused = lastused;
		}
		flow_stats_update(&cls->stats, 0, 0, 0, flow->lastused,
				  FLOW_ACTION_HW_STATS_DELAYED);
		ret = 0;
	}
	mutex_unlock(&nat->lock);

	return ret;
}

static int nat_block_cb(enum tc_setup_type type, void *type_data, void *cb_priv)
{
	struct flow_cls_offload *cls = type_data;
	struct rtl8197f_nat *nat = cb_priv;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		return nat_flow_replace(nat, cls);
	case FLOW_CLS_DESTROY:
		return nat_flow_destroy(nat, cls);
	case FLOW_CLS_STATS:
		return nat_flow_stats(nat, cls);
	default:
		return -EOPNOTSUPP;
	}
}

static int nat_clear_tables(struct rtl8197f_nat *nat)
{
	int i, ret = 0;

	for (i = 0; !ret && i < SW_TBL_NETIFS; i++) {
		ret = nat_clear(nat, SW_TBL_NETIF, i, SW_TBL_NETIF_WORDS);
		if (!ret)
			ret = nat_clear(nat, SW_TBL_VLAN, NAT_VID(i),
					SW_TBL_VLAN_WORDS);
	}
	for (i = 0; !ret && i < SW_TBL_EXTIPS; i++)
		ret = nat_clear(nat, SW_TBL_EXTIP, i, SW_TBL_EXTIP_WORDS);
	for (i = 0; !ret && i < SW_TBL_NHS; i++)
		ret = nat_clear(nat, SW_TBL_NH, i, SW_TBL_NH_WORDS);
	for (i = 0; !ret && i < SW_TBL_ROUTES; i++)
		ret = nat_clear(nat, SW_TBL_ROUTE, i, SW_TBL_ROUTE_WORDS);
	for (i = 0; !ret && i < SW_TBL_ARPS; i++)
		ret = nat_clear(nat, SW_TBL_ARP, i, SW_TBL_ARP_WORDS);
	for (i = 0; !ret && i < SW_TBL_NAPTS; i++)
		ret = nat_napt_clear(nat, i);

	return ret;
}

/* Turn on routing and NAPT, which does nothing until a port gets a netif */
static int nat_enable(struct rtl8197f_nat *nat)
{
	u32 bcast[SW_TBL_L2_WORDS] = {
		0xffffffff,
		FIELD_PREP(L2_1_MAC0, 0xff) | L2_1_TO_CPU | L2_1_STATIC |
		FIELD_PREP(L2_1_AGING, 3) | L2_1_AUTH,
	};
	int ret;

	ret = nat_clear_tables(nat);
	if (ret)
		return ret;

	/*
	 * Broadcasts only to the CPU: the external switch forwards them
	 * between its ports, and ports can share a VLAN here
	 */
	ret = nat_write(nat, SW_TBL_L2, NAT_L2_BCAST, bcast, ARRAY_SIZE(bcast));
	if (ret)
		return ret;

	writel(FIELD_PREP(SW_TEATCR_TCP_SHORT, NAT_AGE) |
	       FIELD_PREP(SW_TEATCR_TCP_MEDIUM, NAT_AGE) |
	       FIELD_PREP(SW_TEATCR_TCP_LONG, NAT_AGE) |
	       FIELD_PREP(SW_TEATCR_UDP, NAT_AGE), nat->swcore + SW_TEATCR);
	nat_rmw(nat, SW_TEACR, SW_TEACR_L4_AGING_OFF | SW_TEACR_L2_AGING_OFF, 0);
	/* Fragments to the CPU, which reassembles them for conntrack */
	nat_rmw(nat, SW_ALECR, 0, SW_ALECR_TTL_DEC | SW_ALECR_FRAG_TO_CPU);
	/* Interfaces by VLAN, NAPT misses and other protocols to the CPU */
	nat_rmw(nat, SW_SWTCR0,
		SW_SWTCR0_NETIF_BY | SW_SWTCR0_WAN_ROUTE |
		SW_SWTCR0_NAPT_AUTO_LEARN | SW_SWTCR0_NAPT_MISS_DROP,
		SW_SWTCR0_NAPT_AUTO_DELETE | SW_SWTCR0_NAPT_OTHER_TO_CPU);
	nat_rmw(nat, SW_SWTCR1, 0,
		SW_SWTCR1_L4_HASH1 | SW_SWTCR1_L4_4WAY |
		SW_SWTCR1_FRAG_TO_ACL_PERMIT | SW_SWTCR1_NAT_T2LOG);
	writel(SW_MSCR_L2 | SW_MSCR_L3 | SW_MSCR_L4, nat->swcore + SW_MSCR);

	return 0;
}

static void nat_disable(struct rtl8197f_nat *nat)
{
	struct rtl8197f_nat_flow *flow;
	struct hlist_node *tmp;
	int bkt;

	writel(SW_MSCR_L2, nat->swcore + SW_MSCR);

	hash_for_each_safe(nat->cookies, bkt, tmp, flow, node)
		nat_flow_put(nat, flow);
	/* Learnt from a direction that was not offloaded */
	if (nat->route_nh >= 0)
		nat_route_clear(nat);

	nat_clear(nat, SW_TBL_L2, NAT_L2_BCAST, SW_TBL_L2_WORDS);
}

static LIST_HEAD(nat_block_cb_list);

/*
 * All DSA user ports share the flowtable's block, so bind it once and count
 * the users
 */
int rtl8197f_nat_setup_ft(struct rtl8197f_nat *nat, struct flow_block_offload *f)
{
	struct flow_block_cb *block_cb;
	int ret;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &nat_block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, nat_block_cb, nat);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}

		block_cb = flow_block_cb_alloc(nat_block_cb, nat, nat, NULL);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);

		mutex_lock(&nat->lock);
		ret = nat->blocks ? 0 : nat_enable(nat);
		if (!ret)
			nat->blocks++;
		mutex_unlock(&nat->lock);
		if (ret) {
			flow_block_cb_free(block_cb);
			return ret;
		}

		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &nat_block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, nat_block_cb, nat);
		if (!block_cb)
			return -ENOENT;

		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);

			mutex_lock(&nat->lock);
			if (!--nat->blocks)
				nat_disable(nat);
			mutex_unlock(&nat->lock);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* Before a restart: back to plain switching, the boot code expects that */
void rtl8197f_nat_shutdown(struct rtl8197f_nat *nat)
{
	writel(SW_MSCR_L2, nat->swcore + SW_MSCR);
}

static int nat_debugfs_show(struct seq_file *m, void *v)
{
	struct rtl8197f_nat *nat = m->private;
	struct rtl8197f_nat_stats *s = &nat->stats;
	struct rtl8197f_nat_flow *flow;
	u32 entry[SW_TBL_NAPT_WORDS];
	int i, bkt;

	mutex_lock(&nat->lock);

	seq_printf(m, "blocks %u flows %u route_nh %d udp_outbound %d\n",
		   nat->blocks, nat->flows, nat->route_nh,
		   READ_ONCE(nat->udp_outbound));
	seq_printf(m, "added %llu removed %llu rewritten %llu hw_errors %llu\n",
		   s->added, s->removed, s->rewritten, s->hw_errors);
	seq_printf(m, "refused: rule %llu udp_outbound %llu route %llu host %llu busy %llu full %llu\n",
		   s->refused_rule, s->refused_udp, s->refused_route,
		   s->refused_host, s->refused_busy, s->refused_full);

	for (i = 0; i < SW_TBL_NETIFS; i++)
		if (nat->netif[i].refs)
			seq_printf(m, "netif %d: %pM vid %d ports %#x refs %u\n",
				   i, nat->netif[i].mac, NAT_VID(i),
				   nat->netif[i].ports, nat->netif[i].refs);
	for (i = 0; i < NAT_PORTS; i++)
		if (nat->port[i].refs)
			seq_printf(m, "port %d: netif %d refs %u\n", i,
				   nat->port[i].netif, nat->port[i].refs);
	for (i = 0; i < NAT_SUBNETS; i++)
		if (nat->subnet[i].refs)
			seq_printf(m, "subnet %d: %pI4/%u netif %d arp %u refs %u\n",
				   i, &nat->subnet[i].net, nat->subnet[i].prefix,
				   nat->subnet[i].netif, nat->subnet[i].arp_base,
				   nat->subnet[i].refs);
	for (i = 0; i < SW_TBL_ARPS; i++)
		if (nat->arp[i].refs)
			seq_printf(m, "arp %d: l2 %d refs %u hw %08x\n", i,
				   nat->arp[i].l2, nat->arp[i].refs,
				   readl(nat->tables + SW_TBL_OFFSET(SW_TBL_ARP, i)));
	for (i = 0; i < SW_TBL_EXTIPS; i++)
		if (nat->extip[i].refs)
			seq_printf(m, "extip %d: %pI4 refs %u\n", i,
				   &nat->extip[i].addr, nat->extip[i].refs);
	for (i = 0; i < SW_TBL_NHS; i++)
		if (nat->nh[i].refs)
			seq_printf(m, "nh %d: netif %d l2 %d extip %d refs %u\n",
				   i, nat->nh[i].netif, nat->nh[i].l2,
				   nat->nh[i].extip, nat->nh[i].refs);
	for (i = 0; i < SW_TBL_L2_ENTRIES; i++)
		if (nat->l2[i].refs)
			seq_printf(m, "l2 %d: %pM port %u refs %u\n", i,
				   nat->l2[i].mac, nat->l2[i].port,
				   nat->l2[i].refs);

	hash_for_each(nat->cookies, bkt, flow, node) {
		struct rtl8197f_nat_conn *c = &flow->conn;

		nat_napt_read(nat, flow->napt, entry);
		seq_printf(m, "%s %s %pI4:%u %pI4:%u %pI4:%u port %d>%d nh %d napt %d %08x %08x %08x age %lu idle %u\n",
			   c->tcp ? "tcp" : "udp", c->outbound ? "out" : "in",
			   &c->int_ip, ntohs(c->int_port),
			   &c->ext_ip, ntohs(c->ext_port),
			   &c->rem_ip, ntohs(c->rem_port),
			   flow->in_port, flow->out_port, flow->nh, flow->napt,
			   entry[0], entry[1], entry[2],
			   FIELD_GET(NAPT1_AGE, entry[1]),
			   jiffies_to_msecs(jiffies - flow->lastused) / 1000);
	}

	mutex_unlock(&nat->lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nat_debugfs);

static void nat_debugfs_remove(void *data)
{
	struct rtl8197f_nat *nat = data;

	debugfs_remove_recursive(nat->debugfs);
}

struct rtl8197f_nat *rtl8197f_nat_create(struct device *dev,
					 struct net_device *ndev,
					 void __iomem *swcore,
					 void __iomem *tables)
{
	struct rtl8197f_nat *nat;
	int ret;

	nat = devm_kzalloc(dev, sizeof(*nat), GFP_KERNEL);
	if (!nat)
		return ERR_PTR(-ENOMEM);

	nat->l2 = devm_kcalloc(dev, SW_TBL_L2_ENTRIES, sizeof(*nat->l2),
			       GFP_KERNEL);
	if (!nat->l2)
		return ERR_PTR(-ENOMEM);

	ret = devm_mutex_init(dev, &nat->lock);
	if (ret)
		return ERR_PTR(ret);

	nat->dev = dev;
	nat->ndev = ndev;
	nat->swcore = swcore;
	nat->tables = tables;
	nat->route_nh = -1;
	hash_init(nat->cookies);

	nat->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("nat", 0400, nat->debugfs, nat, &nat_debugfs_fops);
	debugfs_create_bool("udp_outbound", 0600, nat->debugfs,
			    &nat->udp_outbound);
	ret = devm_add_action_or_reset(dev, nat_debugfs_remove, nat);
	if (ret)
		return ERR_PTR(ret);

	return nat;
}
