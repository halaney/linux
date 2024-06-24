#include <linux/phylink.h>

#include "common.h"
#include "stmmac_pcs.h"

static int dwmac_pcs_validate(struct phylink_pcs *pcs, unsigned long *supported,
			      const struct phylink_link_state *state)
{
	/* Only support in-band */
	if (!test_bit(ETHTOOL_LINK_MODE_Autoneg_BIT, state->advertising))
		return -EINVAL;

	return 0;
}

static int dwmac_pcs_enable(struct phylink_pcs *pcs)
{
	struct mac_device_info *hw = phylink_pcs_to_mac_dev_info(pcs);

	stmmac_pcs_enable_irq(hw->priv, hw);

	return 0;
}

static void dwmac_pcs_disable(struct phylink_pcs *pcs)
{
	struct mac_device_info *hw = phylink_pcs_to_mac_dev_info(pcs);

	stmmac_pcs_disable_irq(hw->priv, hw);
}

static int dwmac_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
			    phy_interface_t interface,
			    const unsigned long *advertising,
			    bool permit_pause_to_mac)
{
	struct mac_device_info *hw = phylink_pcs_to_mac_dev_info(pcs);
	struct stmmac_priv *priv = hw->priv;
	u32 val;

	/* TODO Think about this:
	 * + En/dis SGMII/RGMII IRQs based on the neg_mode value?
	 * + Do we need to set PCS_CONTROL.TC?.. For SGMII MAC2MAC?
	 * + The next is SGMII/RTBI/TBI-specific
	 */

	val = readl(priv->pcsaddr + PCS_AN_CTRL);

	val |= PCS_AN_CTRL_ANE | PCS_AN_CTRL_RAN;

	/* + The SGMRAL flag is SGMII-specific */
	if (hw->ps)
		val |= PCS_AN_CTRL_SGMRAL;

	writel(val, priv->pcsaddr + PCS_AN_CTRL);

	return 0;
}

static void dwmac_pcs_get_state(struct phylink_pcs *pcs,
				struct phylink_link_state *state)
{
	struct mac_device_info *hw = phylink_pcs_to_mac_dev_info(pcs);
	struct stmmac_priv *priv = hw->priv;
	u32 val;

	val = stmmac_pcs_get_config_reg(priv, hw);

	/* TODO The next is SGMII/RGMII/SMII-specific */
	state->link = !!(val & PCS_CFG_LNKSTS);
	if (!state->link)
		return;

	switch (FIELD_GET(PCS_CFG_LNKSPEED, val)) {
	case PCS_CFG_LNKSPEED_2_5:
		state->speed = SPEED_10;
		break;
	case PCS_CFG_LNKSPEED_25:
		state->speed = SPEED_100;
		break;
	case PCS_CFG_LNKSPEED_250:
		state->speed = SPEED_1000;
		break;
	default:
		netdev_err(priv->dev, "Unknown speed detected\n");
		break;
	}

	state->duplex = val & PCS_CFG_LNKMOD ? DUPLEX_FULL : DUPLEX_HALF;

	/* TODO Check the PCS_AN_STATUS.Link status here?.. Note the flag is latched-low */

	/* TODO The next is the TBI/RTBI-specific and seems to be valid if PCS_AN_STATUS.ANC */
	val = readl(priv->pcsaddr + PCS_ANE_LPA);

	linkmode_set_bit(ETHTOOL_LINK_MODE_Autoneg_BIT,
			 state->lp_advertising);

	if (val & PCS_ANE_FD) {
		linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
				 state->lp_advertising);
		linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT,
				 state->lp_advertising);
		linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT,
				 state->lp_advertising);
	}

	if (val & PCS_ANE_HD) {
		linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Half_BIT,
				 state->lp_advertising);
		linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT,
				 state->lp_advertising);
		linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT,
				 state->lp_advertising);
	}

	/* TODO The databook says the encoding is defined in IEEE 802.3z,
	 * Section 37.2.1.4. Do we need the STMMAC_PCS_PAUSE and
	 * STMMAC_PCS_ASYM_PAUSE mask here?
	 */
	linkmode_mod_bit(ETHTOOL_LINK_MODE_Pause_BIT,
			 state->lp_advertising,
			 FIELD_GET(PCS_ANE_PSE, val) & STMMAC_PCS_PAUSE);
	linkmode_mod_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT,
			 state->lp_advertising,
			 FIELD_GET(PCS_ANE_PSE, val) & STMMAC_PCS_ASYM_PAUSE);
}

const struct phylink_pcs_ops dwmac_pcs_ops = {
	.pcs_validate = dwmac_pcs_validate,
	.pcs_enable = dwmac_pcs_enable,
	.pcs_disable = dwmac_pcs_disable,
	.pcs_config = dwmac_pcs_config,
	.pcs_get_state = dwmac_pcs_get_state,

};

void dwmac_pcs_isr(struct mac_device_info *hw, unsigned int intr_status,
		   struct stmmac_extra_stats *x)
{
	struct stmmac_priv *priv = hw->priv;
	bool an_status = false, sr_status = false;

	if (intr_status & PCS_ANE_IRQ) {
		x->irq_pcs_ane_n++;
		an_status = true;
	}

	if (intr_status & PCS_LINK_IRQ) {
		x->irq_pcs_link_n++;
		an_status = true;
	}

	if (intr_status & PCS_RGSMIIIS_IRQ) {
		x->irq_rgmii_n++;
		sr_status = true;
	}

	/* Read the AN and SGMII/RGMII/SMII status regs to clear IRQ */
	if (an_status)
		readl(priv->pcsaddr + PCS_AN_STATUS);

	if (sr_status)
		readl(priv->pcsaddr + PCS_SRGMII_CSR);

	/* Any PCS event shall trigger the PHYLINK PCS state update */
	if (an_status || sr_status)
		phylink_pcs_change(&hw->mac_pcs, false);
}
