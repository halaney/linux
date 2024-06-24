#include "common.h"
#include "stmmac_pcs.h"

int dwmac_pcs_config(struct mac_device_info *hw, unsigned int neg_mode,
		     phy_interface_t interface,
		     const unsigned long *advertising)
{
	struct stmmac_priv *priv = hw->priv;
	u32 val;

	val = readl(priv->pcsaddr + PCS_AN_CTRL);

	val |= PCS_AN_CTRL_ANE | PCS_AN_CTRL_RAN;

	if (hw->ps)
		val |= PCS_AN_CTRL_SGMRAL;

	writel(val, priv->pcsaddr + PCS_AN_CTRL);

	return 0;
}

void dwmac_pcs_get_state(struct mac_device_info *hw,
			 struct phylink_link_state *state)
{
	struct stmmac_priv *priv = hw->priv;
	u32 val;

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

	/* TODO Make sure that STMMAC_PCS_PAUSE STMMAC_PCS_ASYM_PAUSE usage is legitimate */
	linkmode_mod_bit(ETHTOOL_LINK_MODE_Pause_BIT,
			 state->lp_advertising,
			 FIELD_GET(PCS_ANE_PSE, val) & STMMAC_PCS_PAUSE);
	linkmode_mod_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT,
			 state->lp_advertising,
			 FIELD_GET(PCS_ANE_PSE, val) & STMMAC_PCS_ASYM_PAUSE);
}

const struct phylink_pcs_ops dwmac_pcs_ops = {
};
