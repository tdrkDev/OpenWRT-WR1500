/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Realtek RTL8197F switch core: definitions shared by the Ethernet driver
 * and the NAPT offload
 */

#ifndef _RTL8197F_ETH_H
#define _RTL8197F_ETH_H

#include <linux/bits.h>
#include <linux/types.h>

struct device;
struct flow_block_offload;
struct net_device;

/* Switch core registers */
#define SW_PVCR(n)			(0x4a08 + 4 * (n))	/* two ports each */
#define   SW_PVCR_PVID_EVEN		GENMASK(11, 0)
#define   SW_PVCR_PVID_ODD		GENMASK(27, 16)

/* Switch core tables */
#define SW_TBL_L2			0
#define   SW_TBL_L2_ENTRIES		1024
#define   SW_TBL_L2_WORDS		2
#define SW_TBL_VLAN			6
#define   SW_TBL_VLAN_WORDS		3
#define   SW_VLAN0_EXT_UNTAG		GENMASK(17, 15)
#define   SW_VLAN0_UNTAG		GENMASK(14, 9)
#define   SW_VLAN0_EXT_MEMBER		GENMASK(8, 6)
#define   SW_VLAN0_MEMBER		GENMASK(5, 0)
#define   SW_VLAN_EXT_CPU		BIT(2)	/* extension port 2 is the CPU */
#define SW_TBL_OFFSET(type, idx)	(((type) << 16) + (idx) * 32)

/* The VLAN of all ports while they carry no offloaded connections */
#define RTL8197F_ETH_VID		1

struct rtl8197f_nat;

int rtl8197f_sw_tbl_write(void __iomem *swcore, unsigned int type,
			  unsigned int idx, const u32 *data, unsigned int words);

struct rtl8197f_nat *rtl8197f_nat_create(struct device *dev,
					 struct net_device *ndev,
					 void __iomem *swcore,
					 void __iomem *tables);
int rtl8197f_nat_setup_ft(struct rtl8197f_nat *nat, struct flow_block_offload *f);
void rtl8197f_nat_shutdown(struct rtl8197f_nat *nat);

#endif /* _RTL8197F_ETH_H */
