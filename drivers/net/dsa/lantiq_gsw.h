// SPDX-License-Identifier: GPL-2.0
/*
 * Lantiq / Intel GSWIP switch driver for VRX200 SoCs
 *
 * Copyright (C) 2010 Lantiq Deutschland
 * Copyright (C) 2012 John Crispin <john@phrozen.org>
 * Copyright (C) 2017 - 2019 Hauke Mehrtens <hauke@hauke-m.de>
 * Copyright (C) 2022 Reliable Controls Corporation,
 * 			Harley Sims <hsims@reliablecontrols.com>
 *
 * The VLAN and bridge model the GSWIP hardware uses does not directly
 * matches the model DSA uses.
 *
 * The hardware has 64 possible table entries for bridges with one VLAN
 * ID, one flow id and a list of ports for each bridge. All entries which
 * match the same flow ID are combined in the mac learning table, they
 * act as one global bridge.
 * The hardware does not support VLAN filter on the port, but on the
 * bridge, this driver converts the DSA model to the hardware.
 *
 * The CPU gets all the exception frames which do not match any forwarding
 * rule and the CPU port is also added to all bridges. This makes it possible
 * to handle all the special cases easily in software.
 * At the initialization the driver allocates one bridge table entry for
 * each switch port which is used when the port is used without an
 * explicit bridge. This prevents the frames from being forwarded
 * between all LAN ports by default.
 */

#ifndef __LANTIQ_GSW_H
#define __LANTIQ_GSW_H

#define RCC_GSW_ENABLE_BREAKPOINTS	(0)
#define RCC_GSW_VERBOSE_KLOG		(0)
#define RCC_GSW_RUN_MDIO_COMM_TESTS 	(0)

#if RCC_GSW_ENABLE_BREAKPOINTS
#define RCC_GSW_BREAKPOINT(priv)	((priv)->hw_info->hw_ops->breakpoint((priv), __func__, __LINE__))
#else
#define RCC_GSW_BREAKPOINT(priv)
#endif

#if RCC_GSW_VERBOSE_KLOG
#define RCC_GSW_PRINTK(...) 			\
({ 						\
	printk("!RCC: %s() ", __func__);	\
	printk(KERN_CONT "" __VA_ARGS__);	\
})
#define RCC_GSW_PRINT_TBL_ENTRY_PTR(e)					\
({									\
	RCC_GSW_PRINTK("***********************");			\
	RCC_GSW_PRINTK("PCE table entry:");				\
	RCC_GSW_PRINTK("\t index (TBL_ADDR_ADDR):%i", e->index);	\
	RCC_GSW_PRINTK("\t table address (TBL_CTRL_ADDR):%i", e->table);\
	RCC_GSW_PRINTK("\t key data:");					\
	RCC_GSW_PRINTK("\t\t 0x%X %X %X %X %X %X %X %X", 		\
		e->key[0], e->key[1], e->key[2], e->key[3],		\
		e->key[4], e->key[5], e->key[6], e->key[7]);		\
	RCC_GSW_PRINTK("\t value data:");				\
	RCC_GSW_PRINTK("\t\t 0x%X %X %X %X %X", 			\
		e->val[0], e->val[1], e->val[2], e->val[3], e->val[4]);	\
	RCC_GSW_PRINTK("\t mask: 0x%X", e->mask);			\
	RCC_GSW_PRINTK("control data:");				\
	RCC_GSW_PRINTK("\t type: %u", e->type);				\
	RCC_GSW_PRINTK("\t valid: %u", e->valid);			\
	RCC_GSW_PRINTK("\t gmap: 0x%X", e->gmap);			\
	RCC_GSW_PRINTK("***********************");			\
})
#define RCC_GSW_PRINT_TBL_ENTRY_STR(s) 		\
({						\
	struct gswip_pce_table_entry* p = &s;	\
	RCC_GSW_PRINT_TBL_ENTRY_PTR(p);		\
})
#else
#define RCC_GSW_PRINTK(...)
#define RCC_GSW_PRINT_TBL_ENTRY_PTR(p)
#define RCC_GSW_PRINT_TBL_ENTRY_STR(s)
#endif

#include <net/dsa.h>
#include "lantiq_pce.h"

/* GSWIP MDIO Registers */
#define GSWIP_MDIO_GLOB			0x00
#define  GSWIP_MDIO_GLOB_ENABLE		BIT(15)
#define GSWIP_MDIO_CTRL			0x08
#define  GSWIP_MDIO_CTRL_BUSY		BIT(12)
#define  GSWIP_MDIO_CTRL_RD		BIT(11)
#define  GSWIP_MDIO_CTRL_WR		BIT(10)
#define  GSWIP_MDIO_CTRL_PHYAD_MASK	0x1f
#define  GSWIP_MDIO_CTRL_PHYAD_SHIFT	5
#define  GSWIP_MDIO_CTRL_REGAD_MASK	0x1f
#define GSWIP_MDIO_READ			0x09
#define GSWIP_MDIO_WRITE		0x0A
#define GSWIP_MDIO_MDC_CFG0		0x0B

#define GSWIP_MDIO_MDC_CFG1		0x0C
#define  GSWIP_MDIO_MDC_CFG1_FREQ_MASK	(0x00FF)
/* The MDIO bus clock frequency this corresponds to depends
 * on the system clock of the GSW part. For MaxLinear
 * GSW12x/140 parts, S9 corresponds to a bus frequency
 * of 3.4MHz, while existing comments suggest it corresponds
 * to 2.5MHz for the GSWIP modules embedded into various SoCs.
 */
#define  GSWIP_MDIO_MDC_CFG1_FREQ_S9	(0x0009)

#define GSWIP_MDIO_PHYp(p)		(0x15 - (p))
#define  GSWIP_MDIO_PHY_LINK_MASK	0x6000
#define  GSWIP_MDIO_PHY_LINK_AUTO	0x0000
#define  GSWIP_MDIO_PHY_LINK_DOWN	0x4000
#define  GSWIP_MDIO_PHY_LINK_UP		0x2000
#define  GSWIP_MDIO_PHY_SPEED_MASK	0x1800
#define  GSWIP_MDIO_PHY_SPEED_AUTO	0x1800
#define  GSWIP_MDIO_PHY_SPEED_M10	0x0000
#define  GSWIP_MDIO_PHY_SPEED_M100	0x0800
#define  GSWIP_MDIO_PHY_SPEED_G1	0x1000
#define  GSWIP_MDIO_PHY_FDUP_MASK	0x0600
#define  GSWIP_MDIO_PHY_FDUP_AUTO	0x0000
#define  GSWIP_MDIO_PHY_FDUP_EN		0x0200
#define  GSWIP_MDIO_PHY_FDUP_DIS	0x0600
#define  GSWIP_MDIO_PHY_FCONTX_MASK	0x0180
#define  GSWIP_MDIO_PHY_FCONTX_AUTO	0x0000
#define  GSWIP_MDIO_PHY_FCONTX_EN	0x0100
#define  GSWIP_MDIO_PHY_FCONTX_DIS	0x0180
#define  GSWIP_MDIO_PHY_FCONRX_MASK	0x0060
#define  GSWIP_MDIO_PHY_FCONRX_AUTO	0x0000
#define  GSWIP_MDIO_PHY_FCONRX_EN	0x0020
#define  GSWIP_MDIO_PHY_FCONRX_DIS	0x0060
#define  GSWIP_MDIO_PHY_ADDR_MASK	0x001f
#define  GSWIP_MDIO_PHY_MASK		(GSWIP_MDIO_PHY_ADDR_MASK | \
					 GSWIP_MDIO_PHY_FCONRX_MASK | \
					 GSWIP_MDIO_PHY_FCONTX_MASK | \
					 GSWIP_MDIO_PHY_LINK_MASK | \
					 GSWIP_MDIO_PHY_SPEED_MASK | \
					 GSWIP_MDIO_PHY_FDUP_MASK)

/* GSWIP MII Registers */
#define GSWIP_MII_CFGp(p)		(0x2 * (p))
#define  GSWIP_MII_CFG_RESET		BIT(15)
#define  GSWIP_MII_CFG_EN		BIT(14)
#define  GSWIP_MII_CFG_ISOLATE		BIT(13)
#define  GSWIP_MII_CFG_LDCLKDIS		BIT(12)
#define  GSWIP_MII_CFG_RGMII_IBS	BIT(8)
#define  GSWIP_MII_CFG_RMII_CLK		BIT(7)
#define  GSWIP_MII_CFG_MODE_MIIP	0x0
#define  GSWIP_MII_CFG_MODE_MIIM	0x1
#define  GSWIP_MII_CFG_MODE_RMIIP	0x2
#define  GSWIP_MII_CFG_MODE_RMIIM	0x3
#define  GSWIP_MII_CFG_MODE_RGMII	0x4
#define  GSWIP_MII_CFG_MODE_GMII	0x9
#define  GSWIP_MII_CFG_MODE_MASK	0xf
#define  GSWIP_MII_CFG_RATE_M2P5	0x00
#define  GSWIP_MII_CFG_RATE_M25	0x10
#define  GSWIP_MII_CFG_RATE_M125	0x20
#define  GSWIP_MII_CFG_RATE_M50	0x30
#define  GSWIP_MII_CFG_RATE_AUTO	0x40
#define  GSWIP_MII_CFG_RATE_MASK	0x70
#define GSWIP_MII_PCDU0			0x01
#define GSWIP_MII_PCDU1			0x03
#define GSWIP_MII_PCDU5			0x05
#define  GSWIP_MII_PCDU_TXDLY_MASK	GENMASK(2, 0)
#define  GSWIP_MII_PCDU_RXDLY_MASK	GENMASK(9, 7)

/* GSWIP Core Registers */
#define GSWIP_SWRES			0x000
#define  GSWIP_SWRES_R1			BIT(1)	/* GSWIP Software reset */
#define  GSWIP_SWRES_R0			BIT(0)	/* GSWIP Hardware reset */
#define GSWIP_VERSION			0x013
#define  GSWIP_VERSION_REV_SHIFT	0
#define  GSWIP_VERSION_REV_MASK		GENMASK(7, 0)
#define  GSWIP_VERSION_MOD_SHIFT	8
#define  GSWIP_VERSION_MOD_MASK		GENMASK(15, 8)
#define   GSWIP_VERSION_2_0		0x100
#define   GSWIP_VERSION_2_1		0x021
#define   GSWIP_VERSION_2_2		0x122
#define   GSWIP_VERSION_2_2_ETC		0x022
#define   GSWIP_VERSION_2_3		0x023

/* only applicable to MaxLinear parts */
#define GSWIP_GPIO_ALTSEL0		0x1383
#define GSWIP_GPIO_ALTSEL1		0x1384
#define GSWIP_GPIO_ALTSEL_0_1_MASK	(0x0003)

#define GSWIP_BM_RAM_VAL(x)		(0x043 - (x))
#define GSWIP_BM_RAM_ADDR		0x044
#define GSWIP_BM_RAM_CTRL		0x045
#define  GSWIP_BM_RAM_CTRL_BAS		BIT(15)
#define  GSWIP_BM_RAM_CTRL_OPMOD	BIT(5)
#define  GSWIP_BM_RAM_CTRL_ADDR_MASK	GENMASK(4, 0)
#define GSWIP_BM_QUEUE_GCTRL		0x04A
#define  GSWIP_BM_QUEUE_GCTRL_GL_MOD	BIT(10)
/* buffer management Port Configuration Register */
#define GSWIP_BM_PCFGp(p)		(0x080 + ((p) * 2))
#define  GSWIP_BM_PCFG_CNTEN		BIT(0)	/* RMON Counter Enable */
#define  GSWIP_BM_PCFG_IGCNT		BIT(1)	/* Ingres Special Tag RMON count */
/* buffer management Port Control Register */
#define GSWIP_BM_RMON_CTRLp(p)		(0x81 + ((p) * 2))
#define  GSWIP_BM_CTRL_RMON_RAM1_RES	BIT(0)	/* Software Reset for RMON RAM 1 */
#define  GSWIP_BM_CTRL_RMON_RAM2_RES	BIT(1)	/* Software Reset for RMON RAM 2 */

/* PCE */
#define GSWIP_PCE_TBL_KEY(x)		(0x447 - (x))
#define GSWIP_PCE_TBL_MASK		0x448
#define GSWIP_PCE_TBL_VAL(x)		(0x44D - (x))
#define GSWIP_PCE_TBL_ADDR		0x44E
#define GSWIP_PCE_TBL_CTRL		0x44F
#define  GSWIP_PCE_TBL_CTRL_BAS		BIT(15)
#define  GSWIP_PCE_TBL_CTRL_TYPE	BIT(13)
#define  GSWIP_PCE_TBL_CTRL_VLD		BIT(12)
#define  GSWIP_PCE_TBL_CTRL_KEYFORM	BIT(11)
#define  GSWIP_PCE_TBL_CTRL_GMAP_MASK	GENMASK(10, 7)
#define  GSWIP_PCE_TBL_CTRL_OPMOD_MASK	GENMASK(6, 5)
#define  GSWIP_PCE_TBL_CTRL_OPMOD_ADRD	0x00
#define  GSWIP_PCE_TBL_CTRL_OPMOD_ADWR	0x20
#define  GSWIP_PCE_TBL_CTRL_OPMOD_KSRD	0x40
#define  GSWIP_PCE_TBL_CTRL_OPMOD_KSWR	0x60
#define  GSWIP_PCE_TBL_CTRL_ADDR_MASK	GENMASK(4, 0)
#define GSWIP_PCE_PMAP1			0x453	/* Monitoring port map */
#define GSWIP_PCE_PMAP2			0x454	/* Default Multicast port map */
#define GSWIP_PCE_PMAP3			0x455	/* Default Unknown Unicast port map */
#define  GSWIP_PCE_PMAP_ALL_PORTS	(0x7F)
#define GSWIP_PCE_GCTRL_0		0x456
#define  GSWIP_PCE_GCTRL_0_MTFL		BIT(0)  /* MAC Table Flushing */
#define  GSWIP_PCE_GCTRL_0_MC_VALID	BIT(3)
#define  GSWIP_PCE_GCTRL_0_VLAN		BIT(14) /* VLAN aware Switching */
#define GSWIP_PCE_GCTRL_1		0x457
#define  GSWIP_PCE_GCTRL_1_MAC_LRN_MOD	BIT(0)	/* MAC Address learning mode */
#define  GSWIP_PCE_GCTRL_1_MAC_GLOCK	BIT(2)	/* MAC Address table lock */
#define  GSWIP_PCE_GCTRL_1_MAC_GLOCK_MOD	BIT(3) /* Mac address table lock forwarding mode */
#define  GSWIP_PCE_GCTRL_1_VLANMD	BIT(9)	/* GSWIP2.2 VLAN Mode */
#define GSWIP_PCE_PCTRL_0p(p)		(0x480 + ((p) * 0xA))
#define  GSWIP_PCE_PCTRL_0_TVM		BIT(5)	/* Transparent VLAN mode */
#define  GSWIP_PCE_PCTRL_0_VREP		BIT(6)	/* VLAN Replace Mode */
#define  GSWIP_PCE_PCTRL_0_INGRESS	BIT(11)	/* Accept special tag in ingress */
#define  GSWIP_PCE_PCTRL_0_PSTATE_LISTEN	0x0
#define  GSWIP_PCE_PCTRL_0_PSTATE_RX		0x1
#define  GSWIP_PCE_PCTRL_0_PSTATE_TX		0x2
#define  GSWIP_PCE_PCTRL_0_PSTATE_LEARNING	0x3
#define  GSWIP_PCE_PCTRL_0_PSTATE_FORWARDING	0x7
#define  GSWIP_PCE_PCTRL_0_PSTATE_MASK	GENMASK(2, 0)
#define GSWIP_PCE_VCTRL(p)		(0x485 + ((p) * 0xA))
#define  GSWIP_PCE_VCTRL_UVR		BIT(0)	/* Unknown VLAN Rule */
#define  GSWIP_PCE_VCTRL_VIMR		BIT(3)	/* VLAN Ingress Member violation rule */
#define  GSWIP_PCE_VCTRL_VEMR		BIT(4)	/* VLAN Egress Member violation rule */
#define  GSWIP_PCE_VCTRL_VSR		BIT(5)	/* VLAN Security */
#define  GSWIP_PCE_VCTRL_VID0		BIT(6)	/* Priority Tagged Rule */
#define GSWIP_PCE_DEFPVID(p)		(0x486 + ((p) * 0xA))

#define GSWIP_MAC_FLEN			0x8C5
#define GSWIP_MAC_CTRL_0p(p)		(0x903 + ((p) * 0xC))
#define  GSWIP_MAC_CTRL_0_PADEN		BIT(8)
#define  GSWIP_MAC_CTRL_0_FCS_EN	BIT(7)
#define  GSWIP_MAC_CTRL_0_FCON_MASK	0x0070
#define  GSWIP_MAC_CTRL_0_FCON_AUTO	0x0000
#define  GSWIP_MAC_CTRL_0_FCON_RX	0x0010
#define  GSWIP_MAC_CTRL_0_FCON_TX	0x0020
#define  GSWIP_MAC_CTRL_0_FCON_RXTX	0x0030
#define  GSWIP_MAC_CTRL_0_FCON_NONE	0x0040
#define  GSWIP_MAC_CTRL_0_FDUP_MASK	0x000C
#define  GSWIP_MAC_CTRL_0_FDUP_AUTO	0x0000
#define  GSWIP_MAC_CTRL_0_FDUP_EN	0x0004
#define  GSWIP_MAC_CTRL_0_FDUP_DIS	0x000C
#define  GSWIP_MAC_CTRL_0_GMII_MASK	0x0003
#define  GSWIP_MAC_CTRL_0_GMII_AUTO	0x0000
#define  GSWIP_MAC_CTRL_0_GMII_MII	0x0001
#define  GSWIP_MAC_CTRL_0_GMII_RGMII	0x0002
#define GSWIP_MAC_CTRL_2p(p)		(0x905 + ((p) * 0xC))
#define GSWIP_MAC_CTRL_2_MLEN		BIT(3) /* Maximum Untagged Frame Lnegth */

/* Ethernet Switch Fetch DMA Port Control Register */
#define GSWIP_FDMA_PCTRLp(p)		(0xA80 + ((p) * 0x6))
#define  GSWIP_FDMA_PCTRL_EN		BIT(0)	/* FDMA Port Enable */
#define  GSWIP_FDMA_PCTRL_STEN		BIT(1)	/* Special Tag Insertion Enable */
#define  GSWIP_FDMA_PCTRL_VLANMOD_MASK	GENMASK(4, 3)	/* VLAN Modification Control */
#define  GSWIP_FDMA_PCTRL_VLANMOD_SHIFT	3	/* VLAN Modification Control */
#define  GSWIP_FDMA_PCTRL_VLANMOD_DIS	(0x0 << GSWIP_FDMA_PCTRL_VLANMOD_SHIFT)
#define  GSWIP_FDMA_PCTRL_VLANMOD_PRIO	(0x1 << GSWIP_FDMA_PCTRL_VLANMOD_SHIFT)
#define  GSWIP_FDMA_PCTRL_VLANMOD_ID	(0x2 << GSWIP_FDMA_PCTRL_VLANMOD_SHIFT)
#define  GSWIP_FDMA_PCTRL_VLANMOD_BOTH	(0x3 << GSWIP_FDMA_PCTRL_VLANMOD_SHIFT)

/* Ethernet Switch Store DMA Port Control Register */
#define GSWIP_SDMA_PCTRLp(p)		(0xBC0 + ((p) * 0x6))
#define  GSWIP_SDMA_PCTRL_EN		BIT(0)	/* SDMA Port Enable */
#define  GSWIP_SDMA_PCTRL_FCEN		BIT(1)	/* Flow Control Enable */
#define  GSWIP_SDMA_PCTRL_PAUFWD	BIT(3)	/* Pause Frame Forwarding */

#define GSWIP_TABLE_ACTIVE_VLAN		0x01
#define GSWIP_TABLE_VLAN_MAPPING	0x02
#define GSWIP_TABLE_MAC_BRIDGE		0x0b
#define  GSWIP_TABLE_MAC_BRIDGE_STATIC	0x01	/* Static not, aging entry */

#define XRX200_GPHY_FW_ALIGN	(16 * 1024)

struct gswip_priv;
struct gsw_hw_ops {
	u32 	(*read)(struct gswip_priv *priv, void *base, u32 offset);
	void 	(*write)(struct gswip_priv *priv, void *base, u32 offset, \
			u32 val);
	int 	(*poll_timeout)(struct gswip_priv *priv, void *base, \
			u32 offset, u32 cleared, u32 sleep_us, u32 timeout_us);
	bool 	(*check_interface_support)(int port, phy_interface_t interface);
#if RCC_GSW_ENABLE_BREAKPOINTS
	void	(*breakpoint)(struct gswip_priv *priv, const char* func_name, int line);
#endif
};

struct gsw_hw_info {
	int max_ports;
	int cpu_port;
	const struct gsw_hw_ops *hw_ops;
	const struct gswip_pce_microcode (*microcode)[];
	enum dsa_tag_protocol dsa_tag_proto;
};

struct xway_gphy_match_data {
	char *fe_firmware_name;
	char *ge_firmware_name;
};

struct gswip_gphy_fw {
	struct clk *clk_gate;
	struct reset_control *reset;
	u32 fw_addr_offset;
	char *fw_name;
};

struct gswip_vlan {
	struct net_device *bridge;
	u16 vid;
	u8 fid;
};

struct gswip_priv {
	__iomem void *gswip;
	__iomem void *mdio;
	__iomem void *mii;
	const struct gsw_hw_info *hw_info;
	const struct xway_gphy_match_data *gphy_fw_name_cfg;
	struct dsa_switch *ds;
	struct device *dev;
	struct regmap *rcu_regmap;
	struct gswip_vlan vlans[64];
	int num_gphy_fw;
	struct gswip_gphy_fw *gphy_fw;
	u32 port_vlan_filter;
	struct mutex pce_table_lock;
};

struct gswip_pce_table_entry {
	u16 index;      // PCE_TBL_ADDR.ADDR = pData->table_index
	u16 table;      // PCE_TBL_CTRL.ADDR = pData->table
	u16 key[8];
	u16 val[5];
	u16 mask;
	u8 gmap;
	bool type;
	bool valid;
	bool key_mode;
};

struct gswip_rmon_cnt_desc {
	unsigned int size;
	unsigned int offset;
	const char *name;
};

#define MIB_DESC(_size, _offset, _name) {.size = _size, .offset = _offset, .name = _name}

int gsw_core_probe(struct gswip_priv *priv, struct device *dev);
int gsw_core_remove(struct gswip_priv *priv);
void gsw_core_shutdown(struct gswip_priv *priv);

#endif