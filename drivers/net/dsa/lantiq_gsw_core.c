// SPDX-License-Identifier: GPL-2.0
/*
 * Lantiq / Intel / MaxLinear GSW switch driver
 * 	for VRX200, xRX300 and xRX330 SoCs, and GSW120/125/140 switches
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

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_device.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <dt-bindings/mips/lantiq_rcu_gphy.h>

#include "lantiq_gsw.h"

static const struct gswip_rmon_cnt_desc gswip_rmon_cnt[] = {
	/** Receive Packet Count (only packets that are accepted and not discarded). */
	MIB_DESC(1, 0x1F, "RxGoodPkts"),
	MIB_DESC(1, 0x23, "RxUnicastPkts"),
	MIB_DESC(1, 0x22, "RxMulticastPkts"),
	MIB_DESC(1, 0x21, "RxFCSErrorPkts"),
	MIB_DESC(1, 0x1D, "RxUnderSizeGoodPkts"),
	MIB_DESC(1, 0x1E, "RxUnderSizeErrorPkts"),
	MIB_DESC(1, 0x1B, "RxOversizeGoodPkts"),
	MIB_DESC(1, 0x1C, "RxOversizeErrorPkts"),
	MIB_DESC(1, 0x20, "RxGoodPausePkts"),
	MIB_DESC(1, 0x1A, "RxAlignErrorPkts"),
	MIB_DESC(1, 0x12, "Rx64BytePkts"),
	MIB_DESC(1, 0x13, "Rx127BytePkts"),
	MIB_DESC(1, 0x14, "Rx255BytePkts"),
	MIB_DESC(1, 0x15, "Rx511BytePkts"),
	MIB_DESC(1, 0x16, "Rx1023BytePkts"),
	/** Receive Size 1024-1522 (or more, if configured) Packet Count. */
	MIB_DESC(1, 0x17, "RxMaxBytePkts"),
	MIB_DESC(1, 0x18, "RxDroppedPkts"),
	MIB_DESC(1, 0x19, "RxFilteredPkts"),
	MIB_DESC(2, 0x24, "RxGoodBytes"),
	MIB_DESC(2, 0x26, "RxBadBytes"),
	MIB_DESC(1, 0x11, "TxAcmDroppedPkts"),
	MIB_DESC(1, 0x0C, "TxGoodPkts"),
	MIB_DESC(1, 0x06, "TxUnicastPkts"),
	MIB_DESC(1, 0x07, "TxMulticastPkts"),
	MIB_DESC(1, 0x00, "Tx64BytePkts"),
	MIB_DESC(1, 0x01, "Tx127BytePkts"),
	MIB_DESC(1, 0x02, "Tx255BytePkts"),
	MIB_DESC(1, 0x03, "Tx511BytePkts"),
	MIB_DESC(1, 0x04, "Tx1023BytePkts"),
	/** Transmit Size 1024-1522 (or more, if configured) Packet Count. */
	MIB_DESC(1, 0x05, "TxMaxBytePkts"),
	MIB_DESC(1, 0x08, "TxSingleCollCount"),
	MIB_DESC(1, 0x09, "TxMultCollCount"),
	MIB_DESC(1, 0x0A, "TxLateCollCount"),
	MIB_DESC(1, 0x0B, "TxExcessCollCount"),
	MIB_DESC(1, 0x0D, "TxPauseCount"),
	MIB_DESC(1, 0x10, "TxDroppedPkts"),
	MIB_DESC(2, 0x0E, "TxGoodBytes"),
};

static u32 gswip_switch_r(struct gswip_priv *priv, u32 offset)
{
	return priv->hw_info->hw_ops->read(priv, priv->gswip, offset);
}

static void gswip_switch_w(struct gswip_priv *priv, u32 val, u32 offset)
{
	priv->hw_info->hw_ops->write(priv, priv->gswip, offset, val);
}

static void gswip_switch_mask(struct gswip_priv *priv, u32 clear, u32 set,
			      u32 offset)
{
	u32 val = gswip_switch_r(priv, offset);

	val &= ~(clear);
	val |= set;
	gswip_switch_w(priv, val, offset);
}

static int gswip_switch_r_timeout(struct gswip_priv *priv, u32 offset,
				  u32 cleared)
{
	return priv->hw_info->hw_ops->poll_timeout(priv, priv->gswip, \
					offset, cleared, 20, 50000);
}

static u32 gswip_slave_mdio_r(struct gswip_priv *priv, u32 offset)
{
	return priv->hw_info->hw_ops->read(priv, priv->mdio, offset);
}

static void gswip_slave_mdio_w(struct gswip_priv *priv, u32 val, u32 offset)
{
	priv->hw_info->hw_ops->write(priv, priv->mdio, offset, val);
}

static void gswip_slave_mdio_mask(struct gswip_priv *priv, u32 clear, u32 set,
			    u32 offset)
{
	u32 val = gswip_slave_mdio_r(priv, offset);

	val &= ~(clear);
	val |= set;
	gswip_slave_mdio_w(priv, val, offset);
}

static u32 gswip_mii_r(struct gswip_priv *priv, u32 offset)
{
	return priv->hw_info->hw_ops->read(priv, priv->mii, offset);
}

static void gswip_mii_w(struct gswip_priv *priv, u32 val, u32 offset)
{
	priv->hw_info->hw_ops->write(priv, priv->mii, offset, val);
}

static void gswip_mii_mask(struct gswip_priv *priv, u32 clear, u32 set,
			   u32 offset)
{
	u32 val = gswip_mii_r(priv, offset);

	val &= ~(clear);
	val |= set;
	gswip_mii_w(priv, val, offset);
}

static void gswip_mii_mask_cfg(struct gswip_priv *priv, u32 clear, u32 set,
			       int port)
{
	/* There's no MII_CFG register for the CPU port */
	if (!dsa_is_cpu_port(priv->ds, port))
		gswip_mii_mask(priv, clear, set, GSWIP_MII_CFGp(port));
}

static void gswip_mii_mask_pcdu(struct gswip_priv *priv, u32 clear, u32 set,
				int port)
{
	switch (port) {
	case 0:
		gswip_mii_mask(priv, clear, set, GSWIP_MII_PCDU0);
		break;
	case 1:
		gswip_mii_mask(priv, clear, set, GSWIP_MII_PCDU1);
		break;
	case 5:
		gswip_mii_mask(priv, clear, set, GSWIP_MII_PCDU5);
		break;
	}
}

static int gswip_slave_mdio_poll(struct gswip_priv *priv)
{
	int cnt = 100;

	while (likely(cnt--)) {
		u32 ctrl = gswip_slave_mdio_r(priv, GSWIP_MDIO_CTRL);

		if ((ctrl & GSWIP_MDIO_CTRL_BUSY) == 0)
			return 0;
		usleep_range(20, 40);
	}

	return -ETIMEDOUT;
}

static int gswip_slave_mdio_wr(struct mii_bus *bus, int addr, int reg, u16 val)
{
	struct gswip_priv *priv = bus->priv;
	int err;

	err = gswip_slave_mdio_poll(priv);
	if (err) {
		dev_err(&bus->dev, "waiting for MDIO bus busy timed out\n");
		return err;
	}

	gswip_slave_mdio_w(priv, val, GSWIP_MDIO_WRITE);
	gswip_slave_mdio_w(priv, GSWIP_MDIO_CTRL_BUSY | GSWIP_MDIO_CTRL_WR |
		((addr & GSWIP_MDIO_CTRL_PHYAD_MASK) << GSWIP_MDIO_CTRL_PHYAD_SHIFT) |
		(reg & GSWIP_MDIO_CTRL_REGAD_MASK),
		GSWIP_MDIO_CTRL);

	return 0;
}

static int gswip_slave_mdio_rd(struct mii_bus *bus, int addr, int reg)
{
	struct gswip_priv *priv = bus->priv;
	int err;

	err = gswip_slave_mdio_poll(priv);
	if (err) {
		dev_err(&bus->dev, "waiting for MDIO bus busy timed out\n");
		return err;
	}

	gswip_slave_mdio_w(priv, GSWIP_MDIO_CTRL_BUSY | GSWIP_MDIO_CTRL_RD |
		((addr & GSWIP_MDIO_CTRL_PHYAD_MASK) << GSWIP_MDIO_CTRL_PHYAD_SHIFT) |
		(reg & GSWIP_MDIO_CTRL_REGAD_MASK),
		GSWIP_MDIO_CTRL);

	err = gswip_slave_mdio_poll(priv);
	if (err) {
		dev_err(&bus->dev, "waiting for MDIO bus busy timed out\n");
		return err;
	}

	return gswip_slave_mdio_r(priv, GSWIP_MDIO_READ);
}

static int gswip_slave_mdio(struct gswip_priv *priv, struct device_node *mdio_np)
{
	struct dsa_switch *ds = priv->ds;
	int err;

	ds->slave_mii_bus = mdiobus_alloc();
	if (!ds->slave_mii_bus)
		return -ENOMEM;

	ds->slave_mii_bus->priv = priv;
	ds->slave_mii_bus->read = gswip_slave_mdio_rd;
	ds->slave_mii_bus->write = gswip_slave_mdio_wr;
	ds->slave_mii_bus->name = "lantiq,xrx200-mdio";
	snprintf(ds->slave_mii_bus->id, MII_BUS_ID_SIZE, "%s-mii",
		 dev_name(priv->dev));
	ds->slave_mii_bus->parent = priv->dev;
	ds->slave_mii_bus->phy_mask = ~ds->phys_mii_mask;

	err = of_mdiobus_register(ds->slave_mii_bus, mdio_np);
	if (err)
		mdiobus_free(ds->slave_mii_bus);

	return err;
}

static int gswip_pce_table_entry_read(struct gswip_priv *priv,
				      struct gswip_pce_table_entry *tbl)
{
	int i;
	int err;
	u16 crtl;
	u16 addr_mode = tbl->key_mode ? GSWIP_PCE_TBL_CTRL_OPMOD_KSRD :
					GSWIP_PCE_TBL_CTRL_OPMOD_ADRD;

	mutex_lock(&priv->pce_table_lock);

	err = gswip_switch_r_timeout(priv, GSWIP_PCE_TBL_CTRL,
				     GSWIP_PCE_TBL_CTRL_BAS);
	if (err) {
		mutex_unlock(&priv->pce_table_lock);
		return err;
	}

	gswip_switch_w(priv, tbl->index, GSWIP_PCE_TBL_ADDR);
	gswip_switch_mask(priv, GSWIP_PCE_TBL_CTRL_ADDR_MASK |
				GSWIP_PCE_TBL_CTRL_OPMOD_MASK,
			  tbl->table | addr_mode | GSWIP_PCE_TBL_CTRL_BAS,
			  GSWIP_PCE_TBL_CTRL);

	err = gswip_switch_r_timeout(priv, GSWIP_PCE_TBL_CTRL,
				     GSWIP_PCE_TBL_CTRL_BAS);
	if (err) {
		mutex_unlock(&priv->pce_table_lock);
		return err;
	}

	for (i = 0; i < ARRAY_SIZE(tbl->key); i++)
		tbl->key[i] = gswip_switch_r(priv, GSWIP_PCE_TBL_KEY(i));

	for (i = 0; i < ARRAY_SIZE(tbl->val); i++)
		tbl->val[i] = gswip_switch_r(priv, GSWIP_PCE_TBL_VAL(i));

	tbl->mask = gswip_switch_r(priv, GSWIP_PCE_TBL_MASK);

	crtl = gswip_switch_r(priv, GSWIP_PCE_TBL_CTRL);

	tbl->type = !!(crtl & GSWIP_PCE_TBL_CTRL_TYPE);
	tbl->valid = !!(crtl & GSWIP_PCE_TBL_CTRL_VLD);
	tbl->gmap = (crtl & GSWIP_PCE_TBL_CTRL_GMAP_MASK) >> 7;

	mutex_unlock(&priv->pce_table_lock);

	return 0;
}

static int gswip_pce_table_entry_write(struct gswip_priv *priv,
				       struct gswip_pce_table_entry *tbl)
{
	int i;
	int err;
	u16 crtl;
	u16 addr_mode = tbl->key_mode ? GSWIP_PCE_TBL_CTRL_OPMOD_KSWR :
					GSWIP_PCE_TBL_CTRL_OPMOD_ADWR;

	mutex_lock(&priv->pce_table_lock);

	err = gswip_switch_r_timeout(priv, GSWIP_PCE_TBL_CTRL,
				     GSWIP_PCE_TBL_CTRL_BAS);
	if (err) {
		mutex_unlock(&priv->pce_table_lock);
		return err;
	}

	RCC_GSW_PRINTK("WRITING PCE table entry");
	RCC_GSW_PRINT_TBL_ENTRY_PTR(tbl);

	gswip_switch_w(priv, tbl->index, GSWIP_PCE_TBL_ADDR);
	gswip_switch_mask(priv, GSWIP_PCE_TBL_CTRL_ADDR_MASK |
				GSWIP_PCE_TBL_CTRL_OPMOD_MASK,
			  tbl->table | addr_mode,
			  GSWIP_PCE_TBL_CTRL);

	for (i = 0; i < ARRAY_SIZE(tbl->key); i++)
		gswip_switch_w(priv, tbl->key[i], GSWIP_PCE_TBL_KEY(i));

	for (i = 0; i < ARRAY_SIZE(tbl->val); i++)
		gswip_switch_w(priv, tbl->val[i], GSWIP_PCE_TBL_VAL(i));

	gswip_switch_mask(priv, GSWIP_PCE_TBL_CTRL_ADDR_MASK |
				GSWIP_PCE_TBL_CTRL_OPMOD_MASK,
			  tbl->table | addr_mode,
			  GSWIP_PCE_TBL_CTRL);

	gswip_switch_w(priv, tbl->mask, GSWIP_PCE_TBL_MASK);

	crtl = gswip_switch_r(priv, GSWIP_PCE_TBL_CTRL);
	crtl &= ~(GSWIP_PCE_TBL_CTRL_TYPE | GSWIP_PCE_TBL_CTRL_VLD |
		  GSWIP_PCE_TBL_CTRL_GMAP_MASK);
	if (tbl->type)
		crtl |= GSWIP_PCE_TBL_CTRL_TYPE;
	if (tbl->valid)
		crtl |= GSWIP_PCE_TBL_CTRL_VLD;
	crtl |= (tbl->gmap << 7) & GSWIP_PCE_TBL_CTRL_GMAP_MASK;
	crtl |= GSWIP_PCE_TBL_CTRL_BAS;
	gswip_switch_w(priv, crtl, GSWIP_PCE_TBL_CTRL);

	mutex_unlock(&priv->pce_table_lock);

	return gswip_switch_r_timeout(priv, GSWIP_PCE_TBL_CTRL,
				      GSWIP_PCE_TBL_CTRL_BAS);
}

/* Add the LAN port into a bridge with the CPU port by
 * default. This prevents automatic forwarding of
 * packages between the LAN ports when no explicit
 * bridge is configured.
 */
static int gswip_add_single_port_br(struct gswip_priv *priv, int port, bool add)
{
	struct gswip_pce_table_entry vlan_active = {0,};
	struct gswip_pce_table_entry vlan_mapping = {0,};
	unsigned int cpu_port = priv->hw_info->cpu_port;
	unsigned int max_ports = priv->hw_info->max_ports;
	int err;

	RCC_GSW_PRINTK("port:%d & add:%d", port, add);

	if (port >= max_ports) {
		dev_err(priv->dev, "single port for %i supported\n", port);
		return -EIO;
	}

	vlan_active.index = port + 1;
	vlan_active.table = GSWIP_TABLE_ACTIVE_VLAN;
	vlan_active.key[0] = 0; /* vid */
	vlan_active.val[0] = port + 1 /* fid */;
	vlan_active.valid = add;
	err = gswip_pce_table_entry_write(priv, &vlan_active);
	if (err) {
		dev_err(priv->dev, "failed to write active VLAN: %d\n", err);
		return err;
	}

	if (!add)
		return 0;

	vlan_mapping.index = port + 1;
	vlan_mapping.table = GSWIP_TABLE_VLAN_MAPPING;
	vlan_mapping.val[0] = 0 /* vid */;
	vlan_mapping.val[1] = BIT(port) | BIT(cpu_port); /* port map */
	vlan_mapping.val[2] = 0; /* tagged port map */

	err = gswip_pce_table_entry_write(priv, &vlan_mapping);
	if (err) {
		dev_err(priv->dev, "failed to write VLAN mapping: %d\n", err);
		return err;
	}

	return 0;
}

static int gswip_port_enable(struct dsa_switch *ds, int port,
			     struct phy_device *phydev)
{
	struct gswip_priv *priv = ds->priv;
	int err;

	RCC_GSW_PRINTK("port:%d", port);

	if ((!dsa_is_user_port(ds, port)) && (!dsa_is_cpu_port(ds, port))) {
		RCC_GSW_PRINTK("port is not of type DSA_PORT_TYPE_USER, aborting...");
		return 0;
	}

	if (!dsa_is_cpu_port(ds, port)) {
		RCC_GSW_PRINTK("port is not of type DSA_PORT_TYPE_CPU, adding to single port bridge...");
		err = gswip_add_single_port_br(priv, port, true);
		if (err)
			return err;
	}

	RCC_GSW_PRINTK("proceeding to enable port...");

	/* RMON Counter Enable for port */
	gswip_switch_w(priv, GSWIP_BM_PCFG_CNTEN, GSWIP_BM_PCFGp(port));

	/* enable port fetch/store dma & VLAN Modification */
	gswip_switch_mask(priv, 0, GSWIP_FDMA_PCTRL_EN |
				   GSWIP_FDMA_PCTRL_VLANMOD_BOTH,
			 GSWIP_FDMA_PCTRLp(port));
	gswip_switch_mask(priv, 0, GSWIP_SDMA_PCTRL_EN,
			  GSWIP_SDMA_PCTRLp(port));

	if (!dsa_is_cpu_port(ds, port)) {
		u32 mdio_phy = 0;

		if (phydev)
			mdio_phy = phydev->mdio.addr & GSWIP_MDIO_PHY_ADDR_MASK;

		gswip_slave_mdio_mask(priv, GSWIP_MDIO_PHY_ADDR_MASK, mdio_phy,
				GSWIP_MDIO_PHYp(port));
	}

	return 0;
}

static void gswip_port_disable(struct dsa_switch *ds, int port)
{
	struct gswip_priv *priv = ds->priv;

	RCC_GSW_PRINTK("port:%d", port);

	if (!dsa_is_user_port(ds, port)) {
		RCC_GSW_PRINTK("port is not of type DSA_PORT_TYPE_USER.");
		return;
	}

	RCC_GSW_PRINTK("proceeding to disable port...");

	gswip_switch_mask(priv, GSWIP_FDMA_PCTRL_EN, 0,
			  GSWIP_FDMA_PCTRLp(port));
	gswip_switch_mask(priv, GSWIP_SDMA_PCTRL_EN, 0,
			  GSWIP_SDMA_PCTRLp(port));
}

static int gswip_pce_load_microcode(struct gswip_priv *priv)
{
	const struct gswip_pce_microcode (*mc)[];
	int i, err;

	RCC_GSW_PRINTK();

	gswip_switch_mask(priv, GSWIP_PCE_TBL_CTRL_ADDR_MASK |
				GSWIP_PCE_TBL_CTRL_OPMOD_MASK,
			  GSWIP_PCE_TBL_CTRL_OPMOD_ADWR, GSWIP_PCE_TBL_CTRL);
	gswip_switch_w(priv, 0, GSWIP_PCE_TBL_MASK);

	mc = priv->hw_info->microcode;
	for (i = 0; i < MC_ENTRIES; i++) {
		gswip_switch_w(priv, i, GSWIP_PCE_TBL_ADDR);
		RCC_GSW_PRINTK("MC write: entry %i data %X %X %X %X",\
			i, (*mc)[i].val_0, (*mc)[i].val_1, (*mc)[i].val_2, (*mc)[i].val_3);
		gswip_switch_w(priv, (*mc)[i].val_0, GSWIP_PCE_TBL_VAL(0));
		gswip_switch_w(priv, (*mc)[i].val_1, GSWIP_PCE_TBL_VAL(1));
		gswip_switch_w(priv, (*mc)[i].val_2, GSWIP_PCE_TBL_VAL(2));
		gswip_switch_w(priv, (*mc)[i].val_3, GSWIP_PCE_TBL_VAL(3));

		/* start the table access: */
		gswip_switch_mask(priv, 0, GSWIP_PCE_TBL_CTRL_BAS,
				  GSWIP_PCE_TBL_CTRL);
		err = gswip_switch_r_timeout(priv, GSWIP_PCE_TBL_CTRL,
					     GSWIP_PCE_TBL_CTRL_BAS);
		if (err)
			return err;
	}

	/* tell the switch that the microcode is loaded */
	gswip_switch_mask(priv, 0, GSWIP_PCE_GCTRL_0_MC_VALID,
			  GSWIP_PCE_GCTRL_0);

	return 0;
}

static int gswip_port_vlan_filtering(struct dsa_switch *ds, int port,
				     bool vlan_filtering,
				     struct netlink_ext_ack *extack)
{
	struct net_device *bridge = dsa_to_port(ds, port)->bridge_dev;
	struct gswip_priv *priv = ds->priv;

	RCC_GSW_PRINTK("port:%d filter:%d", port, vlan_filtering);

	/* Do not allow changing the VLAN filtering options while in bridge */
	if (bridge && !!(priv->port_vlan_filter & BIT(port)) != vlan_filtering) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Dynamic toggling of vlan_filtering not supported");
		RCC_GSW_PRINTK("bridge/VLAN filter check failed, aborting.");
		return -EIO;
	}

	RCC_GSW_PRINTK("passed bridge checks. Proceeding.");
	if (vlan_filtering) {
		RCC_GSW_PRINTK("setting UVR/VIMR/VEMR, clearing VSR");
		/* Configure port for VLAN filtering by clearing VLAN Security,
		 * setting Unknown VLAN rule & VLAN Ingress/Egress Member
		 * Violation rules
		 */
		gswip_switch_mask(priv,
				  GSWIP_PCE_VCTRL_VSR,
				  GSWIP_PCE_VCTRL_UVR | GSWIP_PCE_VCTRL_VIMR |
				  GSWIP_PCE_VCTRL_VEMR,
				  GSWIP_PCE_VCTRL(port));
		gswip_switch_mask(priv, GSWIP_PCE_PCTRL_0_TVM, 0,
				  GSWIP_PCE_PCTRL_0p(port));
	} else {
		/* Use port based VLAN tag (i.e. associate all ingress traffic
		 * on this port with the port-based VLAN group) by setting
		 * VLAN Security, clearing Unknown VLAN rule &
		 * VLAN Ingress/Egress Member Violation rules
		 */
		RCC_GSW_PRINTK("setting VSR, clearing UVR/VIMR/VEMR");
		gswip_switch_mask(priv,
				  GSWIP_PCE_VCTRL_UVR | GSWIP_PCE_VCTRL_VIMR |
				  GSWIP_PCE_VCTRL_VEMR,
				  GSWIP_PCE_VCTRL_VSR,
				  GSWIP_PCE_VCTRL(port));
		gswip_switch_mask(priv, 0, GSWIP_PCE_PCTRL_0_TVM,
				  GSWIP_PCE_PCTRL_0p(port));
	}

	return 0;
}

static int gswip_setup(struct dsa_switch *ds)
{
	struct gswip_priv *priv = ds->priv;
	struct device_node *np = priv->dev->of_node;
	unsigned int cpu_port = priv->hw_info->cpu_port;
	int i;
	int err;

	RCC_GSW_PRINTK();
	RCC_GSW_BREAKPOINT(priv);

	gswip_switch_w(priv, GSWIP_SWRES_R0, GSWIP_SWRES);
	usleep_range(5000, 10000);
	gswip_switch_w(priv, 0, GSWIP_SWRES);

	/* disable port fetch/store dma on all ports */
	for (i = 0; i < priv->hw_info->max_ports; i++) {
		gswip_port_disable(ds, i);
		gswip_port_vlan_filtering(ds, i, false, NULL);
	}

	/* enable Switch */
	gswip_slave_mdio_mask(priv, 0, GSWIP_MDIO_GLOB_ENABLE, GSWIP_MDIO_GLOB);

	err = gswip_pce_load_microcode(priv);
	if (err) {
		dev_err(priv->dev, "writing PCE microcode failed, %i", err);
		return err;
	}

	RCC_GSW_PRINTK("Microcode loaded...");

	/* RCC behaviour edit:
	 * Configure switch to flood all ports for unknown unicast & multicast.
	 * Do this instead of directing unknown frames to the CPU port and
	 * needing to forward them in SW.
	 */
	gswip_switch_w(priv, GSWIP_PCE_PMAP_ALL_PORTS, GSWIP_PCE_PMAP2);
	gswip_switch_w(priv, GSWIP_PCE_PMAP_ALL_PORTS, GSWIP_PCE_PMAP3);

	/* Deactivate MDIO PHY auto polling. Some PHYs as the AR8030 have an
	 * interoperability problem with this auto polling mechanism because
	 * their status registers think that the link is in a different state
	 * than it actually is. For the AR8030 it has the BMSR_ESTATEN bit set
	 * as well as ESTATUS_1000_TFULL and ESTATUS_1000_XFULL. This makes the
	 * auto polling state machine consider the link being negotiated with
	 * 1Gbit/s. Since the PHY itself is a Fast Ethernet RMII PHY this leads
	 * to the switch port being completely dead (RX and TX are both not
	 * working).
	 * Also with various other PHY / port combinations (PHY11G GPHY, PHY22F
	 * GPHY, external RGMII PEF7071/7072) any traffic would stop. Sometimes
	 * it would work fine for a few minutes to hours and then stop, on
	 * other device it would no traffic could be sent or received at all.
	 * Testing shows that when PHY auto polling is disabled these problems
	 * go away.
	 */
	gswip_slave_mdio_w(priv, 0x0, GSWIP_MDIO_MDC_CFG0);

	/* Configure the MDIO Clock */
	gswip_slave_mdio_mask(priv, GSWIP_MDIO_MDC_CFG1_FREQ_MASK, \
			GSWIP_MDIO_MDC_CFG1_FREQ_S9, GSWIP_MDIO_MDC_CFG1);

	/* Disable the xMII interface and clear it's isolation bit */
	for (i = 0; i < priv->hw_info->max_ports; i++)
		gswip_mii_mask_cfg(priv,
				   GSWIP_MII_CFG_EN | GSWIP_MII_CFG_ISOLATE,
				   0, i);

	/* enable special tag insertion on cpu port */
	gswip_switch_mask(priv, 0, GSWIP_FDMA_PCTRL_STEN,
			  GSWIP_FDMA_PCTRLp(cpu_port));

	/* accept special tag in ingress direction */
	gswip_switch_mask(priv, 0, GSWIP_PCE_PCTRL_0_INGRESS,
			  GSWIP_PCE_PCTRL_0p(cpu_port));

	gswip_switch_mask(priv, 0, GSWIP_MAC_CTRL_2_MLEN,
			  GSWIP_MAC_CTRL_2p(cpu_port));
	gswip_switch_w(priv, VLAN_ETH_FRAME_LEN + 8 + ETH_FCS_LEN,
		       GSWIP_MAC_FLEN);
	gswip_switch_mask(priv, 0, GSWIP_BM_QUEUE_GCTRL_GL_MOD,
			  GSWIP_BM_QUEUE_GCTRL);

	/* Enable "GSWIP2.2 VLAN Mode" on MaxLinear devices */
	if (of_device_is_compatible(np, "maxlinear,gsw12x") ||
	    of_device_is_compatible(np, "maxlinear,gsw140") ||
	    of_device_is_compatible(np, "maxlinear,gsw140-easy"))
		gswip_switch_mask(priv, 0, GSWIP_PCE_GCTRL_1_VLANMD, \
						GSWIP_PCE_GCTRL_1);

	/* VLAN aware Switching */
	gswip_switch_mask(priv, 0, GSWIP_PCE_GCTRL_0_VLAN, GSWIP_PCE_GCTRL_0);

	/* Flush MAC Table */
	gswip_switch_mask(priv, 0, GSWIP_PCE_GCTRL_0_MTFL, GSWIP_PCE_GCTRL_0);

	err = gswip_switch_r_timeout(priv, GSWIP_PCE_GCTRL_0,
				     GSWIP_PCE_GCTRL_0_MTFL);
	if (err) {
		dev_err(priv->dev, "MAC flushing didn't finish\n");
		return err;
	}

	gswip_port_enable(ds, cpu_port, NULL);

	ds->configure_vlan_while_not_filtering = false;

	return 0;
}

static enum dsa_tag_protocol gswip_get_tag_protocol(struct dsa_switch *ds,
						    int port,
						    enum dsa_tag_protocol mp)
{
	struct gswip_priv *priv = ds->priv;

	return priv->hw_info->dsa_tag_proto;
}

static int gswip_vlan_active_create(struct gswip_priv *priv,
				    struct net_device *bridge,
				    int fid, u16 vid)
{
	struct gswip_pce_table_entry vlan_active = {0,};
	unsigned int max_ports = priv->hw_info->max_ports;
	int idx = -1;
	int err;
	int i;

	RCC_GSW_PRINTK("fid:%d vid:%d", fid, vid);

	/* Look for a free slot */
	for (i = max_ports; i < ARRAY_SIZE(priv->vlans); i++) {
		if (!priv->vlans[i].bridge) {
			idx = i;
			RCC_GSW_PRINTK("found free slot %u", idx);
			break;
		}
	}

	if (idx == -1)
		return -ENOSPC;

	if (fid == -1)
		fid = idx;

	vlan_active.index = idx;
	vlan_active.table = GSWIP_TABLE_ACTIVE_VLAN;
	vlan_active.key[0] = vid;
	vlan_active.val[0] = fid;
	vlan_active.valid = true;

	err = gswip_pce_table_entry_write(priv, &vlan_active);
	if (err) {
		dev_err(priv->dev, "failed to write active VLAN: %d\n",	err);
		return err;
	}

	priv->vlans[idx].bridge = bridge;
	priv->vlans[idx].vid = vid;
	priv->vlans[idx].fid = fid;

	return idx;
}

static int gswip_vlan_active_remove(struct gswip_priv *priv, int idx)
{
	struct gswip_pce_table_entry vlan_active = {0,};
	int err;

	RCC_GSW_PRINTK("idx:%d", idx);

	vlan_active.index = idx;
	vlan_active.table = GSWIP_TABLE_ACTIVE_VLAN;
	vlan_active.valid = false;
	err = gswip_pce_table_entry_write(priv, &vlan_active);
	if (err)
		dev_err(priv->dev, "failed to delete active VLAN: %d\n", err);
	priv->vlans[idx].bridge = NULL;

	return err;
}

static int gswip_vlan_add_unaware(struct gswip_priv *priv,
				  struct net_device *bridge, int port)
{
	struct gswip_pce_table_entry vlan_mapping = {0,};
	unsigned int max_ports = priv->hw_info->max_ports;
	unsigned int cpu_port = priv->hw_info->cpu_port;
	bool active_vlan_created = false;
	int idx = -1;
	int i;
	int err;

	RCC_GSW_PRINTK("port:%d", port);

	/* Check if there is already a page for this bridge */
	for (i = max_ports; i < ARRAY_SIZE(priv->vlans); i++) {
		if (priv->vlans[i].bridge == bridge) {
			idx = i;
			RCC_GSW_PRINTK("found idx %u match", idx);
			break;
		}
	}

	/* If this bridge is not programmed yet, add a Active VLAN table
	 * entry in a free slot and prepare the VLAN mapping table entry.
	 */
	if (idx == -1) {
		idx = gswip_vlan_active_create(priv, bridge, -1, 0);
		if (idx < 0)
			return idx;
		active_vlan_created = true;
		RCC_GSW_PRINTK("active VLAN created, now write VLAN mapping...");
		vlan_mapping.index = idx;
		vlan_mapping.table = GSWIP_TABLE_VLAN_MAPPING;
		/* VLAN ID byte, maps to the VLAN ID of vlan active table */
		vlan_mapping.val[0] = 0;
	} else {
		/* Read the existing VLAN mapping entry from the switch */
		vlan_mapping.index = idx;
		vlan_mapping.table = GSWIP_TABLE_VLAN_MAPPING;
		err = gswip_pce_table_entry_read(priv, &vlan_mapping);
		if (err) {
			dev_err(priv->dev, "failed to read VLAN mapping: %d\n",
				err);
			return err;
		}

		RCC_GSW_PRINTK("READING PCE table entry");
		RCC_GSW_PRINT_TBL_ENTRY_STR(vlan_mapping);
	}

	/* Update the VLAN mapping entry and write it to the switch */
	vlan_mapping.val[1] |= BIT(cpu_port);
	vlan_mapping.val[1] |= BIT(port);
	err = gswip_pce_table_entry_write(priv, &vlan_mapping);
	if (err) {
		dev_err(priv->dev, "failed to write VLAN mapping: %d\n", err);
		/* In case an Active VLAN was creaetd delete it again */
		if (active_vlan_created)
			gswip_vlan_active_remove(priv, idx);
		return err;
	}

	gswip_switch_w(priv, 0, GSWIP_PCE_DEFPVID(port));
	return 0;
}

static int gswip_vlan_add_aware(struct gswip_priv *priv,
				struct net_device *bridge, int port,
				u16 vid, bool untagged,
				bool pvid)
{
	struct gswip_pce_table_entry vlan_mapping = {0,};
	unsigned int max_ports = priv->hw_info->max_ports;
	unsigned int cpu_port = priv->hw_info->cpu_port;
	bool active_vlan_created = false;
	int idx = -1;
	int fid = -1;
	int i;
	int err;

	RCC_GSW_PRINTK("port:%d untagged:%d pvid:%d", \
						port, untagged, pvid);

	/* Check if there is already a page for this bridge */
	for (i = max_ports; i < ARRAY_SIZE(priv->vlans); i++) {
		if (priv->vlans[i].bridge == bridge) {
			if (fid != -1 && fid != priv->vlans[i].fid)
				dev_err(priv->dev, "one bridge with multiple flow ids\n");
			fid = priv->vlans[i].fid;
			if (priv->vlans[i].vid == vid) {
				idx = i;
				break;
			}
		}
	}

	/* If this bridge is not programmed yet, add a Active VLAN table
	 * entry in a free slot and prepare the VLAN mapping table entry.
	 */
	if (idx == -1) {
		idx = gswip_vlan_active_create(priv, bridge, fid, vid);
		if (idx < 0)
			return idx;
		active_vlan_created = true;

		vlan_mapping.index = idx;
		vlan_mapping.table = GSWIP_TABLE_VLAN_MAPPING;
		/* VLAN ID byte, maps to the VLAN ID of vlan active table */
		vlan_mapping.val[0] = vid;
	} else {
		/* Read the existing VLAN mapping entry from the switch */
		vlan_mapping.index = idx;
		vlan_mapping.table = GSWIP_TABLE_VLAN_MAPPING;
		err = gswip_pce_table_entry_read(priv, &vlan_mapping);
		if (err) {
			dev_err(priv->dev, "failed to read VLAN mapping: %d\n",
				err);
			return err;
		}

		RCC_GSW_PRINTK("READING PCE table entry");
		RCC_GSW_PRINT_TBL_ENTRY_STR(vlan_mapping);
	}

	vlan_mapping.val[0] = vid;
	/* Update the VLAN mapping entry and write it to the switch */
	vlan_mapping.val[1] |= BIT(cpu_port);
	vlan_mapping.val[2] |= BIT(cpu_port);
	vlan_mapping.val[1] |= BIT(port);
	if (untagged)
		vlan_mapping.val[2] &= ~BIT(port);
	else
		vlan_mapping.val[2] |= BIT(port);
	err = gswip_pce_table_entry_write(priv, &vlan_mapping);
	if (err) {
		dev_err(priv->dev, "failed to write VLAN mapping: %d\n", err);
		/* In case an Active VLAN was creaetd delete it again */
		if (active_vlan_created)
			gswip_vlan_active_remove(priv, idx);
		return err;
	}

	if (pvid)
		gswip_switch_w(priv, idx, GSWIP_PCE_DEFPVID(port));

	return 0;
}

static int gswip_vlan_remove(struct gswip_priv *priv,
			     struct net_device *bridge, int port,
			     u16 vid, bool pvid, bool vlan_aware)
{
	struct gswip_pce_table_entry vlan_mapping = {0,};
	unsigned int max_ports = priv->hw_info->max_ports;
	unsigned int cpu_port = priv->hw_info->cpu_port;
	int idx = -1;
	int i;
	int err;

	RCC_GSW_PRINTK("port:%d vid:%d pvid:%d aware:%d", \
				port, vid, pvid, vlan_aware);

	/* Check if there is already a page for this bridge */
	for (i = max_ports; i < ARRAY_SIZE(priv->vlans); i++) {
		if (priv->vlans[i].bridge == bridge &&
		    (!vlan_aware || priv->vlans[i].vid == vid)) {
			idx = i;
			break;
		}
	}

	if (idx == -1) {
		dev_err(priv->dev, "bridge to leave does not exists\n");
		return -ENOENT;
	}

	vlan_mapping.index = idx;
	vlan_mapping.table = GSWIP_TABLE_VLAN_MAPPING;
	err = gswip_pce_table_entry_read(priv, &vlan_mapping);
	if (err) {
		dev_err(priv->dev, "failed to read VLAN mapping: %d\n",	err);
		return err;
	}

	RCC_GSW_PRINTK("READING PCE table entry");
	RCC_GSW_PRINT_TBL_ENTRY_STR(vlan_mapping);

	vlan_mapping.val[1] &= ~BIT(port);
	vlan_mapping.val[2] &= ~BIT(port);
	err = gswip_pce_table_entry_write(priv, &vlan_mapping);
	if (err) {
		dev_err(priv->dev, "failed to write VLAN mapping: %d\n", err);
		return err;
	}

	/* In case all ports are removed from the bridge, remove the VLAN */
	if ((vlan_mapping.val[1] & ~BIT(cpu_port)) == 0) {
		err = gswip_vlan_active_remove(priv, idx);
		if (err) {
			dev_err(priv->dev, "failed to write active VLAN: %d\n",
				err);
			return err;
		}
	}

	/* GSWIP 2.2 (GRX300) and later program here the VID directly. */
	if (pvid)
		gswip_switch_w(priv, 0, GSWIP_PCE_DEFPVID(port));

	return 0;
}

static int gswip_port_bridge_join(struct dsa_switch *ds, int port,
				  struct net_device *bridge)
{
	struct gswip_priv *priv = ds->priv;
	int err;

	RCC_GSW_PRINTK("port:%d", port);

	/* When the bridge uses VLAN filtering we have to configure VLAN
	 * specific bridges. No bridge is configured here.
	 */
	if (!br_vlan_enabled(bridge)) {
		err = gswip_vlan_add_unaware(priv, bridge, port);
		if (err)
			return err;
		priv->port_vlan_filter &= ~BIT(port);
	} else {
		priv->port_vlan_filter |= BIT(port);
	}
	return gswip_add_single_port_br(priv, port, false);
}

static void gswip_port_bridge_leave(struct dsa_switch *ds, int port,
				    struct net_device *bridge)
{
	struct gswip_priv *priv = ds->priv;

	RCC_GSW_PRINTK("port:%d", port);

	gswip_add_single_port_br(priv, port, true);

	/* When the bridge uses VLAN filtering we have to configure VLAN
	 * specific bridges. No bridge is configured here.
	 */
	if (!br_vlan_enabled(bridge))
		gswip_vlan_remove(priv, bridge, port, 0, true, false);
}

static int gswip_port_vlan_prepare(struct dsa_switch *ds, int port,
				   const struct switchdev_obj_port_vlan *vlan,
				   struct netlink_ext_ack *extack)
{
	struct gswip_priv *priv = ds->priv;
	struct net_device *bridge = dsa_to_port(ds, port)->bridge_dev;
	unsigned int max_ports = priv->hw_info->max_ports;
	int pos = max_ports;
	int i, idx = -1;

	RCC_GSW_PRINTK("port:%d", port);

	/* We only support VLAN filtering on bridges */
	if (!dsa_is_cpu_port(ds, port) && !bridge)
		return -EOPNOTSUPP;

	RCC_GSW_PRINTK("... passed bridge/cpu check.");

	/* Check if there is already a page for this VLAN */
	for (i = max_ports; i < ARRAY_SIZE(priv->vlans); i++) {
		if (priv->vlans[i].bridge == bridge &&
		    priv->vlans[i].vid == vlan->vid) {
			idx = i;
			RCC_GSW_PRINTK("matched existing, idx:%u vid:%u", idx, vid);
			break;
		}
	}

	/* If this VLAN is not programmed yet, we have to reserve
	 * one entry in the VLAN table. Make sure we start at the
	 * next position round.
	 */
	if (idx == -1) {
		/* Look for a free slot */
		for (; pos < ARRAY_SIZE(priv->vlans); pos++) {
			if (!priv->vlans[pos].bridge) {
				idx = pos;
				pos++;
				RCC_GSW_PRINTK("found free slot, idx:%u", idx);
				break;
			}
		}

		if (idx == -1) {
			NL_SET_ERR_MSG_MOD(extack, "No slot in VLAN table");
			return -ENOSPC;
		}
	}

	return 0;
}

static int gswip_port_vlan_add(struct dsa_switch *ds, int port,
			       const struct switchdev_obj_port_vlan *vlan,
			       struct netlink_ext_ack *extack)
{
	struct gswip_priv *priv = ds->priv;
	struct net_device *bridge = dsa_to_port(ds, port)->bridge_dev;
	bool untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
	bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
	int err;

	err = gswip_port_vlan_prepare(ds, port, vlan, extack);
	if (err)
		return err;

	RCC_GSW_PRINTK("port:%d", port);

	/* We have to receive all packets on the CPU port and should not
	 * do any VLAN filtering here. This is also called with bridge
	 * NULL and then we do not know for which bridge to configure
	 * this.
	 */
	if (dsa_is_cpu_port(ds, port))
		return 0;

	RCC_GSW_PRINTK("...passed CPU port check");

	return gswip_vlan_add_aware(priv, bridge, port, vlan->vid,
				    untagged, pvid);
}

static int gswip_port_vlan_del(struct dsa_switch *ds, int port,
			       const struct switchdev_obj_port_vlan *vlan)
{
	struct gswip_priv *priv = ds->priv;
	struct net_device *bridge = dsa_to_port(ds, port)->bridge_dev;
	bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;

	RCC_GSW_PRINTK("port:%d", port);

	/* We have to receive all packets on the CPU port and should not
	 * do any VLAN filtering here. This is also called with bridge
	 * NULL and then we do not know for which bridge to configure
	 * this.
	 */
	if (dsa_is_cpu_port(ds, port))
		return 0;

	return gswip_vlan_remove(priv, bridge, port, vlan->vid, pvid, true);
}

static void gswip_port_fast_age(struct dsa_switch *ds, int port)
{
	struct gswip_priv *priv = ds->priv;
	struct gswip_pce_table_entry mac_bridge = {0,};
	int i;
	int err;

	RCC_GSW_PRINTK("port:%d", port);

	for (i = 0; i < 2048; i++) {
		mac_bridge.table = GSWIP_TABLE_MAC_BRIDGE;
		mac_bridge.index = i;

		err = gswip_pce_table_entry_read(priv, &mac_bridge);
		if (err) {
			dev_err(priv->dev, "failed to read mac bridge: %d\n",
				err);
			return;
		}

		if (!mac_bridge.valid)
			continue;

		if (mac_bridge.val[1] & GSWIP_TABLE_MAC_BRIDGE_STATIC)
			continue;

		if (((mac_bridge.val[0] & GENMASK(7, 4)) >> 4) != port)
			continue;

		mac_bridge.valid = false;
		err = gswip_pce_table_entry_write(priv, &mac_bridge);
		if (err) {
			dev_err(priv->dev, "failed to write mac bridge: %d\n",
				err);
			return;
		}
	}
}

static void gswip_port_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
	struct gswip_priv *priv = ds->priv;
	u32 stp_state;

	RCC_GSW_PRINTK("port:%d state:%d", port, state);

	switch (state) {
	case BR_STATE_DISABLED:
		gswip_switch_mask(priv, GSWIP_SDMA_PCTRL_EN, 0,
				  GSWIP_SDMA_PCTRLp(port));
		return;
	case BR_STATE_BLOCKING:
	case BR_STATE_LISTENING:
		stp_state = GSWIP_PCE_PCTRL_0_PSTATE_LISTEN;
		break;
	case BR_STATE_LEARNING:
		stp_state = GSWIP_PCE_PCTRL_0_PSTATE_LEARNING;
		break;
	case BR_STATE_FORWARDING:
		stp_state = GSWIP_PCE_PCTRL_0_PSTATE_FORWARDING;
		break;
	default:
		dev_err(priv->dev, "invalid STP state: %d\n", state);
		return;
	}

	gswip_switch_mask(priv, 0, GSWIP_SDMA_PCTRL_EN,
			  GSWIP_SDMA_PCTRLp(port));
	gswip_switch_mask(priv, GSWIP_PCE_PCTRL_0_PSTATE_MASK, stp_state,
			  GSWIP_PCE_PCTRL_0p(port));
}

static int gswip_port_fdb(struct dsa_switch *ds, int port,
			  const unsigned char *addr, u16 vid, bool add)
{
	struct gswip_priv *priv = ds->priv;
	struct net_device *bridge = dsa_to_port(ds, port)->bridge_dev;
	struct gswip_pce_table_entry mac_bridge = {0,};
	unsigned int max_ports = priv->hw_info->max_ports;
	int fid = -1;
	int i;
	int err;

	RCC_GSW_PRINTK("port:%d addr:%s vid:%d add:%d", \
				port, addr, vid, add);

	if (!bridge)
		return -EINVAL;

	for (i = max_ports; i < ARRAY_SIZE(priv->vlans); i++) {
		if (priv->vlans[i].bridge == bridge) {
			fid = priv->vlans[i].fid;
			break;
		}
	}

	if (fid == -1) {
		dev_err(priv->dev, "Port not part of a bridge\n");
		return -EINVAL;
	}

	mac_bridge.table = GSWIP_TABLE_MAC_BRIDGE;
	mac_bridge.key_mode = true;
	mac_bridge.key[0] = addr[5] | (addr[4] << 8);
	mac_bridge.key[1] = addr[3] | (addr[2] << 8);
	mac_bridge.key[2] = addr[1] | (addr[0] << 8);
	mac_bridge.key[3] = fid;
	mac_bridge.val[0] = add ? BIT(port) : 0; /* port map */
	mac_bridge.val[1] = GSWIP_TABLE_MAC_BRIDGE_STATIC;
	mac_bridge.valid = add;

	err = gswip_pce_table_entry_write(priv, &mac_bridge);
	if (err)
		dev_err(priv->dev, "failed to write mac bridge: %d\n", err);

	return err;
}

static int gswip_port_fdb_add(struct dsa_switch *ds, int port,
			      const unsigned char *addr, u16 vid)
{
	return gswip_port_fdb(ds, port, addr, vid, true);
}

static int gswip_port_fdb_del(struct dsa_switch *ds, int port,
			      const unsigned char *addr, u16 vid)
{
	return gswip_port_fdb(ds, port, addr, vid, false);
}

static int gswip_port_fdb_dump(struct dsa_switch *ds, int port,
			       dsa_fdb_dump_cb_t *cb, void *data)
{
	struct gswip_priv *priv = ds->priv;
	struct gswip_pce_table_entry mac_bridge = {0,};
	unsigned char addr[6];
	int i;
	int err;

	RCC_GSW_PRINTK("port:%d", port);

	for (i = 0; i < 2048; i++) {
		mac_bridge.table = GSWIP_TABLE_MAC_BRIDGE;
		mac_bridge.index = i;

		err = gswip_pce_table_entry_read(priv, &mac_bridge);
		if (err) {
			dev_err(priv->dev, "failed to read mac bridge entry%d: %d\n",
				i, err);
			return err;
		}

		if (!mac_bridge.valid)
			continue;

		RCC_GSW_PRINTK("READING PCE table entry");
		RCC_GSW_PRINT_TBL_ENTRY_STR(mac_bridge);

		addr[5] = mac_bridge.key[0] & 0xff;
		addr[4] = (mac_bridge.key[0] >> 8) & 0xff;
		addr[3] = mac_bridge.key[1] & 0xff;
		addr[2] = (mac_bridge.key[1] >> 8) & 0xff;
		addr[1] = mac_bridge.key[2] & 0xff;
		addr[0] = (mac_bridge.key[2] >> 8) & 0xff;
		if (mac_bridge.val[1] & GSWIP_TABLE_MAC_BRIDGE_STATIC) {
			if (mac_bridge.val[0] & BIT(port)) {
				err = cb(addr, 0, true, data);
				if (err)
					return err;
			}
		} else {
			if (((mac_bridge.val[0] & GENMASK(7, 4)) >> 4) == port) {
				err = cb(addr, 0, false, data);
				if (err)
					return err;
			}
		}
	}
	return 0;
}

static void gswip_phylink_set_capab(unsigned long *supported,
				    struct phylink_link_state *state)
{
	__ETHTOOL_DECLARE_LINK_MODE_MASK(mask) = { 0, };

	RCC_GSW_PRINTK("supported:%d, state:%d", supported, state);

	/* Allow all the expected bits */
	phylink_set(mask, Autoneg);
	phylink_set_port_modes(mask);
	phylink_set(mask, Pause);
	phylink_set(mask, Asym_Pause);

	/* With the exclusion of MII, Reverse MII and Reduced MII, we
	 * support Gigabit, including Half duplex
	 */
	if (state->interface != PHY_INTERFACE_MODE_MII &&
	    state->interface != PHY_INTERFACE_MODE_REVMII &&
	    state->interface != PHY_INTERFACE_MODE_RMII) {
		phylink_set(mask, 1000baseT_Full);
		phylink_set(mask, 1000baseT_Half);
	}

	phylink_set(mask, 10baseT_Half);
	phylink_set(mask, 10baseT_Full);
	phylink_set(mask, 100baseT_Half);
	phylink_set(mask, 100baseT_Full);

	bitmap_and(supported, supported, mask,
		   __ETHTOOL_LINK_MODE_MASK_NBITS);
	bitmap_and(state->advertising, state->advertising, mask,
		   __ETHTOOL_LINK_MODE_MASK_NBITS);
}

static void gswip_phylink_validate(struct dsa_switch *ds, int port,
					  unsigned long *supported,
					  struct phylink_link_state *state)
{
	struct gswip_priv *priv = ds->priv;

	RCC_GSW_PRINTK("port:%d", port);

	if (!priv->hw_info->hw_ops->check_interface_support(port, \
							state->interface))
			goto unsupported;

	gswip_phylink_set_capab(supported, state);

	return;

unsupported:
	bitmap_zero(supported, __ETHTOOL_LINK_MODE_MASK_NBITS);
	dev_err(ds->dev, "Unsupported interface '%s' for port %d\n",
		phy_modes(state->interface), port);
}

static void gswip_port_set_link(struct gswip_priv *priv, int port, bool link)
{
	u32 mdio_phy;

	RCC_GSW_PRINTK("port:%d link:%d", port, link);

	if (link)
		mdio_phy = GSWIP_MDIO_PHY_LINK_UP;
	else
		mdio_phy = GSWIP_MDIO_PHY_LINK_DOWN;

	gswip_slave_mdio_mask(priv, GSWIP_MDIO_PHY_LINK_MASK, mdio_phy,
			GSWIP_MDIO_PHYp(port));
}

static void gswip_port_set_speed(struct gswip_priv *priv, int port, int speed,
				 phy_interface_t interface)
{
	u32 mdio_phy = 0, mii_cfg = 0, mac_ctrl_0 = 0;

	RCC_GSW_PRINTK("port:%d speed:%d", port, speed);

	switch (speed) {
	case SPEED_10:
		mdio_phy = GSWIP_MDIO_PHY_SPEED_M10;

		if (interface == PHY_INTERFACE_MODE_RMII)
			mii_cfg = GSWIP_MII_CFG_RATE_M50;
		else
			mii_cfg = GSWIP_MII_CFG_RATE_M2P5;

		mac_ctrl_0 = GSWIP_MAC_CTRL_0_GMII_MII;
		break;

	case SPEED_100:
		mdio_phy = GSWIP_MDIO_PHY_SPEED_M100;

		if (interface == PHY_INTERFACE_MODE_RMII)
			mii_cfg = GSWIP_MII_CFG_RATE_M50;
		else
			mii_cfg = GSWIP_MII_CFG_RATE_M25;

		mac_ctrl_0 = GSWIP_MAC_CTRL_0_GMII_MII;
		break;

	case SPEED_1000:
		mdio_phy = GSWIP_MDIO_PHY_SPEED_G1;

		mii_cfg = GSWIP_MII_CFG_RATE_M125;

		mac_ctrl_0 = GSWIP_MAC_CTRL_0_GMII_RGMII;
		break;
	}

	gswip_slave_mdio_mask(priv, GSWIP_MDIO_PHY_SPEED_MASK, mdio_phy,
			GSWIP_MDIO_PHYp(port));
	gswip_mii_mask_cfg(priv, GSWIP_MII_CFG_RATE_MASK, mii_cfg, port);
	gswip_switch_mask(priv, GSWIP_MAC_CTRL_0_GMII_MASK, mac_ctrl_0,
			  GSWIP_MAC_CTRL_0p(port));
}

static void gswip_port_set_duplex(struct gswip_priv *priv, int port, int duplex)
{
	u32 mac_ctrl_0, mdio_phy;

	RCC_GSW_PRINTK("port:%d duplex:%d", port, duplex);

	if (duplex == DUPLEX_FULL) {
		mac_ctrl_0 = GSWIP_MAC_CTRL_0_FDUP_EN;
		mdio_phy = GSWIP_MDIO_PHY_FDUP_EN;
	} else {
		mac_ctrl_0 = GSWIP_MAC_CTRL_0_FDUP_DIS;
		mdio_phy = GSWIP_MDIO_PHY_FDUP_DIS;
	}

	gswip_switch_mask(priv, GSWIP_MAC_CTRL_0_FDUP_MASK, mac_ctrl_0,
			  GSWIP_MAC_CTRL_0p(port));
	gswip_slave_mdio_mask(priv, GSWIP_MDIO_PHY_FDUP_MASK, mdio_phy,
			GSWIP_MDIO_PHYp(port));
}

static void gswip_port_set_pause(struct gswip_priv *priv, int port,
				 bool tx_pause, bool rx_pause)
{
	u32 mac_ctrl_0, mdio_phy;

	RCC_GSW_PRINTK("port:%d tx_p:%d, rx_p:%d", port, tx_pause, rx_pause);

	if (tx_pause && rx_pause) {
		mac_ctrl_0 = GSWIP_MAC_CTRL_0_FCON_RXTX;
		mdio_phy = GSWIP_MDIO_PHY_FCONTX_EN |
			   GSWIP_MDIO_PHY_FCONRX_EN;
	} else if (tx_pause) {
		mac_ctrl_0 = GSWIP_MAC_CTRL_0_FCON_TX;
		mdio_phy = GSWIP_MDIO_PHY_FCONTX_EN |
			   GSWIP_MDIO_PHY_FCONRX_DIS;
	} else if (rx_pause) {
		mac_ctrl_0 = GSWIP_MAC_CTRL_0_FCON_RX;
		mdio_phy = GSWIP_MDIO_PHY_FCONTX_DIS |
			   GSWIP_MDIO_PHY_FCONRX_EN;
	} else {
		mac_ctrl_0 = GSWIP_MAC_CTRL_0_FCON_NONE;
		mdio_phy = GSWIP_MDIO_PHY_FCONTX_DIS |
			   GSWIP_MDIO_PHY_FCONRX_DIS;
	}

	gswip_switch_mask(priv, GSWIP_MAC_CTRL_0_FCON_MASK,
			  mac_ctrl_0, GSWIP_MAC_CTRL_0p(port));
	gswip_slave_mdio_mask(priv,
			GSWIP_MDIO_PHY_FCONTX_MASK |
			GSWIP_MDIO_PHY_FCONRX_MASK,
			mdio_phy, GSWIP_MDIO_PHYp(port));
}

static void gswip_phylink_mac_config(struct dsa_switch *ds, int port,
				     unsigned int mode,
				     const struct phylink_link_state *state)
{
	struct gswip_priv *priv = ds->priv;
	u32 miicfg = 0;

	RCC_GSW_PRINTK("port:%d mode:%d", port, mode);

	miicfg |= GSWIP_MII_CFG_LDCLKDIS;

	switch (state->interface) {
	case PHY_INTERFACE_MODE_MII:
	case PHY_INTERFACE_MODE_INTERNAL:
		miicfg |= GSWIP_MII_CFG_MODE_MIIM;
		break;
	case PHY_INTERFACE_MODE_REVMII:
		miicfg |= GSWIP_MII_CFG_MODE_MIIP;
		break;
	case PHY_INTERFACE_MODE_RMII:
		miicfg |= GSWIP_MII_CFG_MODE_RMIIM;
		break;
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		miicfg |= GSWIP_MII_CFG_MODE_RGMII;
		break;
	case PHY_INTERFACE_MODE_GMII:
		miicfg |= GSWIP_MII_CFG_MODE_GMII;
		break;
	default:
		dev_err(ds->dev,
			"Unsupported interface: %d\n", state->interface);
		return;
	}

	gswip_mii_mask_cfg(priv,
			   GSWIP_MII_CFG_MODE_MASK | GSWIP_MII_CFG_RMII_CLK |
			   GSWIP_MII_CFG_RGMII_IBS | GSWIP_MII_CFG_LDCLKDIS,
			   miicfg, port);

	switch (state->interface) {
	case PHY_INTERFACE_MODE_RGMII_ID:
		gswip_mii_mask_pcdu(priv, GSWIP_MII_PCDU_TXDLY_MASK |
					  GSWIP_MII_PCDU_RXDLY_MASK, 0, port);
		break;
	case PHY_INTERFACE_MODE_RGMII_RXID:
		gswip_mii_mask_pcdu(priv, GSWIP_MII_PCDU_RXDLY_MASK, 0, port);
		break;
	case PHY_INTERFACE_MODE_RGMII_TXID:
		gswip_mii_mask_pcdu(priv, GSWIP_MII_PCDU_TXDLY_MASK, 0, port);
		break;
	default:
		break;
	}
}

static void gswip_phylink_mac_link_down(struct dsa_switch *ds, int port,
					unsigned int mode,
					phy_interface_t interface)
{
	struct gswip_priv *priv = ds->priv;

	RCC_GSW_PRINTK("port:%d mode:%d", port, mode);

	gswip_mii_mask_cfg(priv, GSWIP_MII_CFG_EN, 0, port);

	if (!dsa_is_cpu_port(ds, port))
		gswip_port_set_link(priv, port, false);
}

static void gswip_phylink_mac_link_up(struct dsa_switch *ds, int port,
				      unsigned int mode,
				      phy_interface_t interface,
				      struct phy_device *phydev,
				      int speed, int duplex,
				      bool tx_pause, bool rx_pause)
{
	struct gswip_priv *priv = ds->priv;

	RCC_GSW_PRINTK("port:%d mode:%d", port, mode);

	if (!dsa_is_cpu_port(ds, port)) {
		gswip_port_set_link(priv, port, true);
		gswip_port_set_speed(priv, port, speed, interface);
		gswip_port_set_duplex(priv, port, duplex);
		gswip_port_set_pause(priv, port, tx_pause, rx_pause);
	}

	gswip_mii_mask_cfg(priv, 0, GSWIP_MII_CFG_EN, port);
}

static void gswip_get_strings(struct dsa_switch *ds, int port, u32 stringset,
			      uint8_t *data)
{
	int i;

	RCC_GSW_PRINTK("port:%d", port);

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(gswip_rmon_cnt); i++)
		strncpy(data + i * ETH_GSTRING_LEN, gswip_rmon_cnt[i].name,
			ETH_GSTRING_LEN);
}

static u32 gswip_bcm_ram_entry_read(struct gswip_priv *priv, u32 table,
				    u32 index)
{
	u32 result;
	int err;

	RCC_GSW_PRINTK("table:%d index:%d", table, index);

	gswip_switch_w(priv, index, GSWIP_BM_RAM_ADDR);
	gswip_switch_mask(priv, GSWIP_BM_RAM_CTRL_ADDR_MASK |
				GSWIP_BM_RAM_CTRL_OPMOD,
			      table | GSWIP_BM_RAM_CTRL_BAS,
			      GSWIP_BM_RAM_CTRL);

	err = gswip_switch_r_timeout(priv, GSWIP_BM_RAM_CTRL,
				     GSWIP_BM_RAM_CTRL_BAS);
	if (err) {
		dev_err(priv->dev, "timeout while reading table: %u, index: %u",
			table, index);
		return 0;
	}

	result = gswip_switch_r(priv, GSWIP_BM_RAM_VAL(0));
	result |= gswip_switch_r(priv, GSWIP_BM_RAM_VAL(1)) << 16;

	return result;
}

static void gswip_get_ethtool_stats(struct dsa_switch *ds, int port,
				    uint64_t *data)
{
	struct gswip_priv *priv = ds->priv;
	const struct gswip_rmon_cnt_desc *rmon_cnt;
	int i;
	u64 high;

	RCC_GSW_PRINTK("port:%d", port);

	for (i = 0; i < ARRAY_SIZE(gswip_rmon_cnt); i++) {
		rmon_cnt = &gswip_rmon_cnt[i];

		data[i] = gswip_bcm_ram_entry_read(priv, port,
						   rmon_cnt->offset);
		if (rmon_cnt->size == 2) {
			high = gswip_bcm_ram_entry_read(priv, port,
							rmon_cnt->offset + 1);
			data[i] |= high << 32;
		}
	}
}

static int gswip_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	if (sset != ETH_SS_STATS)
		return 0;

	return ARRAY_SIZE(gswip_rmon_cnt);
}

static const struct dsa_switch_ops gswip_switch_ops = {
	.get_tag_protocol	= gswip_get_tag_protocol,
	.setup			= gswip_setup,
	.port_enable		= gswip_port_enable,
	.port_disable		= gswip_port_disable,
	.port_bridge_join	= gswip_port_bridge_join,
	.port_bridge_leave	= gswip_port_bridge_leave,
	.port_fast_age		= gswip_port_fast_age,
	.port_vlan_filtering	= gswip_port_vlan_filtering,
	.port_vlan_add		= gswip_port_vlan_add,
	.port_vlan_del		= gswip_port_vlan_del,
	.port_stp_state_set	= gswip_port_stp_state_set,
	.port_fdb_add		= gswip_port_fdb_add,
	.port_fdb_del		= gswip_port_fdb_del,
	.port_fdb_dump		= gswip_port_fdb_dump,
	.phylink_validate	= gswip_phylink_validate,
	.phylink_mac_config	= gswip_phylink_mac_config,
	.phylink_mac_link_down	= gswip_phylink_mac_link_down,
	.phylink_mac_link_up	= gswip_phylink_mac_link_up,
	.get_strings		= gswip_get_strings,
	.get_ethtool_stats	= gswip_get_ethtool_stats,
	.get_sset_count		= gswip_get_sset_count,
};

static const struct xway_gphy_match_data xrx200a1x_gphy_data = {
	.fe_firmware_name = "lantiq/xrx200_phy22f_a14.bin",
	.ge_firmware_name = "lantiq/xrx200_phy11g_a14.bin",
};

static const struct xway_gphy_match_data xrx200a2x_gphy_data = {
	.fe_firmware_name = "lantiq/xrx200_phy22f_a22.bin",
	.ge_firmware_name = "lantiq/xrx200_phy11g_a22.bin",
};

static const struct xway_gphy_match_data xrx300_gphy_data = {
	.fe_firmware_name = "lantiq/xrx300_phy22f_a21.bin",
	.ge_firmware_name = "lantiq/xrx300_phy11g_a21.bin",
};

static const struct of_device_id xway_gphy_match[] = {
	{ .compatible = "lantiq,xrx200-gphy-fw", .data = NULL },
	{ .compatible = "lantiq,xrx200a1x-gphy-fw", .data = &xrx200a1x_gphy_data },
	{ .compatible = "lantiq,xrx200a2x-gphy-fw", .data = &xrx200a2x_gphy_data },
	{ .compatible = "lantiq,xrx300-gphy-fw", .data = &xrx300_gphy_data },
	{ .compatible = "lantiq,xrx330-gphy-fw", .data = &xrx300_gphy_data },
	{},
};

static int gswip_gphy_fw_load(struct gswip_priv *priv, struct gswip_gphy_fw *gphy_fw)
{
	struct device *dev = priv->dev;
	const struct firmware *fw;
	void *fw_addr;
	dma_addr_t dma_addr;
	dma_addr_t dev_addr;
	size_t size;
	int ret;

	ret = clk_prepare_enable(gphy_fw->clk_gate);
	if (ret)
		return ret;

	reset_control_assert(gphy_fw->reset);

	/* The vendor BSP uses a 200ms delay after asserting the reset line.
	 * Without this some users are observing that the PHY is not coming up
	 * on the MDIO bus.
	 */
	msleep(200);

	ret = request_firmware(&fw, gphy_fw->fw_name, dev);
	if (ret) {
		dev_err(dev, "failed to load firmware: %s, error: %i\n",
			gphy_fw->fw_name, ret);
		return ret;
	}

	/* GPHY cores need the firmware code in a persistent and contiguous
	 * memory area with a 16 kB boundary aligned start address.
	 */
	size = fw->size + XRX200_GPHY_FW_ALIGN;

	fw_addr = dmam_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);
	if (fw_addr) {
		fw_addr = PTR_ALIGN(fw_addr, XRX200_GPHY_FW_ALIGN);
		dev_addr = ALIGN(dma_addr, XRX200_GPHY_FW_ALIGN);
		memcpy(fw_addr, fw->data, fw->size);
	} else {
		dev_err(dev, "failed to alloc firmware memory\n");
		release_firmware(fw);
		return -ENOMEM;
	}

	release_firmware(fw);

	ret = regmap_write(priv->rcu_regmap, gphy_fw->fw_addr_offset, dev_addr);
	if (ret)
		return ret;

	reset_control_deassert(gphy_fw->reset);

	return ret;
}

static int gswip_gphy_fw_probe(struct gswip_priv *priv,
			       struct gswip_gphy_fw *gphy_fw,
			       struct device_node *gphy_fw_np, int i)
{
	struct device *dev = priv->dev;
	u32 gphy_mode;
	int ret;
	char gphyname[10];

	snprintf(gphyname, sizeof(gphyname), "gphy%d", i);

	gphy_fw->clk_gate = devm_clk_get(dev, gphyname);
	if (IS_ERR(gphy_fw->clk_gate)) {
		dev_err(dev, "Failed to lookup gate clock\n");
		return PTR_ERR(gphy_fw->clk_gate);
	}

	ret = of_property_read_u32(gphy_fw_np, "reg", &gphy_fw->fw_addr_offset);
	if (ret)
		return ret;

	ret = of_property_read_u32(gphy_fw_np, "lantiq,gphy-mode", &gphy_mode);
	/* Default to GE mode */
	if (ret)
		gphy_mode = GPHY_MODE_GE;

	switch (gphy_mode) {
	case GPHY_MODE_FE:
		gphy_fw->fw_name = priv->gphy_fw_name_cfg->fe_firmware_name;
		break;
	case GPHY_MODE_GE:
		gphy_fw->fw_name = priv->gphy_fw_name_cfg->ge_firmware_name;
		break;
	default:
		dev_err(dev, "Unknown GPHY mode %d\n", gphy_mode);
		return -EINVAL;
	}

	gphy_fw->reset = of_reset_control_array_get_exclusive(gphy_fw_np);
	if (IS_ERR(gphy_fw->reset)) {
		if (PTR_ERR(gphy_fw->reset) != -EPROBE_DEFER)
			dev_err(dev, "Failed to lookup gphy reset\n");
		return PTR_ERR(gphy_fw->reset);
	}

	return gswip_gphy_fw_load(priv, gphy_fw);
}

static void gswip_gphy_fw_remove(struct gswip_priv *priv,
				 struct gswip_gphy_fw *gphy_fw)
{
	int ret;

	/* check if the device was fully probed */
	if (!gphy_fw->fw_name)
		return;

	ret = regmap_write(priv->rcu_regmap, gphy_fw->fw_addr_offset, 0);
	if (ret)
		dev_err(priv->dev, "can not reset GPHY FW pointer");

	clk_disable_unprepare(gphy_fw->clk_gate);

	reset_control_put(gphy_fw->reset);
}

static int gswip_gphy_fw_list(struct gswip_priv *priv,
			      struct device_node *gphy_fw_list_np, u32 version)
{
	struct device *dev = priv->dev;
	struct device_node *gphy_fw_np;
	const struct of_device_id *match;
	int err;
	int i = 0;

	/* The VRX200 rev 1.1 uses the GSWIP 2.0 and needs the older
	 * GPHY firmware. The VRX200 rev 1.2 uses the GSWIP 2.1 and also
	 * needs a different GPHY firmware.
	 */
	if (of_device_is_compatible(gphy_fw_list_np, "lantiq,xrx200-gphy-fw")) {
		switch (version) {
		case GSWIP_VERSION_2_0:
			priv->gphy_fw_name_cfg = &xrx200a1x_gphy_data;
			break;
		case GSWIP_VERSION_2_1:
			priv->gphy_fw_name_cfg = &xrx200a2x_gphy_data;
			break;
		default:
			dev_err(dev, "unknown GSWIP version: 0x%x", version);
			return -ENOENT;
		}
	}

	match = of_match_node(xway_gphy_match, gphy_fw_list_np);
	if (match && match->data)
		priv->gphy_fw_name_cfg = match->data;

	if (!priv->gphy_fw_name_cfg) {
		dev_err(dev, "GPHY compatible type not supported");
		return -ENOENT;
	}

	priv->num_gphy_fw = of_get_available_child_count(gphy_fw_list_np);
	if (!priv->num_gphy_fw)
		return -ENOENT;

	priv->rcu_regmap = syscon_regmap_lookup_by_phandle(gphy_fw_list_np,
							   "lantiq,rcu");
	if (IS_ERR(priv->rcu_regmap))
		return PTR_ERR(priv->rcu_regmap);

	priv->gphy_fw = devm_kmalloc_array(dev, priv->num_gphy_fw,
					   sizeof(*priv->gphy_fw),
					   GFP_KERNEL | __GFP_ZERO);
	if (!priv->gphy_fw)
		return -ENOMEM;

	for_each_available_child_of_node(gphy_fw_list_np, gphy_fw_np) {
		err = gswip_gphy_fw_probe(priv, &priv->gphy_fw[i],
					  gphy_fw_np, i);
		if (err) {
			of_node_put(gphy_fw_np);
			goto remove_gphy;
		}
		i++;
	}

	/* The standalone PHY11G requires 300ms to be fully
	 * initialized and ready for any MDIO communication after being
	 * taken out of reset. For the SoC-internal GPHY variant there
	 * is no (known) documentation for the minimum time after a
	 * reset. Use the same value as for the standalone variant as
	 * some users have reported internal PHYs not being detected
	 * without any delay.
	 */
	msleep(300);

	return 0;

remove_gphy:
	for (i = 0; i < priv->num_gphy_fw; i++)
		gswip_gphy_fw_remove(priv, &priv->gphy_fw[i]);
	return err;
}

int gsw_core_probe(struct gswip_priv *priv, struct device *dev)
{
	struct device_node *np, *mdio_np, *gphy_fw_np;
	int err;
	int i;
	u32 version;

	priv->hw_info = of_device_get_match_data(dev);
	if (!priv->hw_info)
		return -EINVAL;

	priv->ds = devm_kzalloc(dev, sizeof(*priv->ds), GFP_KERNEL);
	if (!priv->ds)
		return -ENOMEM;

	priv->ds->dev = dev;
	priv->ds->num_ports = priv->hw_info->max_ports;
	priv->ds->priv = priv;
	priv->ds->ops = &gswip_switch_ops;
	priv->dev = dev;
	mutex_init(&priv->pce_table_lock);

	version = gswip_switch_r(priv, GSWIP_VERSION);

	np = dev->of_node;
	switch (version) {
	case GSWIP_VERSION_2_0:
	case GSWIP_VERSION_2_1:
		if (!of_device_is_compatible(np, "lantiq,xrx200-gswip"))
			return -EINVAL;
		break;
	case GSWIP_VERSION_2_2:
	case GSWIP_VERSION_2_2_ETC:
		if (!of_device_is_compatible(np, "lantiq,xrx300-gswip") &&
		    !of_device_is_compatible(np, "lantiq,xrx330-gswip"))
			return -EINVAL;
		break;
	case GSWIP_VERSION_2_3:
		if (!of_device_is_compatible(np, "maxlinear,gsw12x") &&
		    !of_device_is_compatible(np, "maxlinear,gsw140") &&
		    !of_device_is_compatible(np, "maxlinear,gsw140-easy"))
			return -EINVAL;
		break;
	default:
		dev_err(dev, "unknown GSWIP version: 0x%x", version);
		return -ENOENT;
	}

	if (of_device_is_compatible(np, "maxlinear,gsw140-easy")) {
		/* Need to configure GPIO0/1 pins for external master MDIO
		 * bus in order to allow board to communicate with the
		 * external PHY correctly (pins default to GPIO function).
		 */
		gswip_switch_mask(priv, 0, GSWIP_GPIO_ALTSEL_0_1_MASK, \
							GSWIP_GPIO_ALTSEL0);
		gswip_switch_mask(priv, 0, GSWIP_GPIO_ALTSEL_0_1_MASK,
							GSWIP_GPIO_ALTSEL1);
	}

	if (of_device_is_compatible(np, "lantiq,xrx200-gswip") ||
	    of_device_is_compatible(np, "lantiq,xrx300-gswip") ||
	    of_device_is_compatible(np, "lantiq,xrx330-gswip")) {
		gphy_fw_np = of_get_compatible_child(dev->of_node, "lantiq,gphy-fw");
		if (gphy_fw_np) {
			err = gswip_gphy_fw_list(priv, gphy_fw_np, version);
			of_node_put(gphy_fw_np);
			if (err) {
				dev_err(dev, "gphy fw probe failed\n");
				return err;
			}
		}
	}

	mdio_np = of_get_compatible_child(dev->of_node, "lantiq,xrx200-mdio");
	if (mdio_np) {
		err = gswip_slave_mdio(priv, mdio_np);
		if (err) {
			dev_err(dev, "mdio probe failed\n");
			goto put_mdio_node;
		}
	}

	err = dsa_register_switch(priv->ds);
	if (err) {
		dev_err(dev, "dsa switch register failed: %i\n", err);
		goto mdio_bus;
	}
	if (!dsa_is_cpu_port(priv->ds, priv->hw_info->cpu_port)) {
		dev_err(dev, "wrong CPU port defined, HW only supports port: %i",
			priv->hw_info->cpu_port);
		err = -EINVAL;
		goto disable_switch;
	}

	dev_info(dev, "probed GSWIP version %lx mod %lx\n",
		 (version & GSWIP_VERSION_REV_MASK) >> GSWIP_VERSION_REV_SHIFT,
		 (version & GSWIP_VERSION_MOD_MASK) >> GSWIP_VERSION_MOD_SHIFT);
	
	return 0;

disable_switch:
	gswip_slave_mdio_mask(priv, GSWIP_MDIO_GLOB_ENABLE, 0, GSWIP_MDIO_GLOB);
	dsa_unregister_switch(priv->ds);
mdio_bus:
	if (mdio_np) {
		mdiobus_unregister(priv->ds->slave_mii_bus);
		mdiobus_free(priv->ds->slave_mii_bus);
	}
put_mdio_node:
	of_node_put(mdio_np);
	for (i = 0; i < priv->num_gphy_fw; i++)
		gswip_gphy_fw_remove(priv, &priv->gphy_fw[i]);
	return err;
}
EXPORT_SYMBOL(gsw_core_probe);

int gsw_core_remove(struct gswip_priv *priv)
{
	int i;

	/* disable the switch */
	gswip_slave_mdio_mask(priv, GSWIP_MDIO_GLOB_ENABLE, 0, GSWIP_MDIO_GLOB);

	dsa_unregister_switch(priv->ds);

	if (priv->ds->slave_mii_bus) {
		mdiobus_unregister(priv->ds->slave_mii_bus);
		of_node_put(priv->ds->slave_mii_bus->dev.of_node);
		mdiobus_free(priv->ds->slave_mii_bus);
	}

	for (i = 0; i < priv->num_gphy_fw; i++)
		gswip_gphy_fw_remove(priv, &priv->gphy_fw[i]);

	return 0;
}
EXPORT_SYMBOL(gsw_core_remove);

void gsw_core_shutdown(struct gswip_priv *priv)
{
	dsa_switch_shutdown(priv->ds);
}
EXPORT_SYMBOL(gsw_core_shutdown);

MODULE_FIRMWARE("lantiq/xrx300_phy11g_a21.bin");
MODULE_FIRMWARE("lantiq/xrx300_phy22f_a21.bin");
MODULE_FIRMWARE("lantiq/xrx200_phy11g_a14.bin");
MODULE_FIRMWARE("lantiq/xrx200_phy11g_a22.bin");
MODULE_FIRMWARE("lantiq/xrx200_phy22f_a14.bin");
MODULE_FIRMWARE("lantiq/xrx200_phy22f_a22.bin");
MODULE_AUTHOR("Hauke Mehrtens <hauke@hauke-m.de>");
MODULE_DESCRIPTION("Core driver for MaxLinear / Lantiq / Intel GSW switches");
MODULE_LICENSE("GPL v2");
