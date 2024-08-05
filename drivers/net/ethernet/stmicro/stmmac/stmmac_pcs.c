// SPDX-License-Identifier: GPL-2.0-only
#include "stmmac.h"
#include "stmmac_pcs.h"

static void __dwmac_ctrl_ane(struct stmmac_pcs *spcs, bool ane, bool srgmi_ral,
			     bool loopback)

{
	u32 val;

	val = readl(spcs->pcs_base + GMAC_AN_CTRL(0));

	/* Enable and restart the Auto-Negotiation */
	if (ane)
		val |= GMAC_AN_CTRL_ANE | GMAC_AN_CTRL_RAN;
	else
		val &= ~GMAC_AN_CTRL_ANE;

	/* In case of MAC-2-MAC connection, block is configured to operate
	 * according to MAC conf register.
	 */
	if (srgmi_ral)
		val |= GMAC_AN_CTRL_SGMRAL;

	if (loopback)
		val |= GMAC_AN_CTRL_ELE;
	else
		val &= ~GMAC_AN_CTRL_ELE;

	writel(val, spcs->pcs_base + GMAC_AN_CTRL(0));
}

int dwmac_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
		     phy_interface_t interface,
		     const unsigned long *advertising,
		     bool permit_pause_to_mac)
{
	struct stmmac_pcs *spcs = phylink_pcs_to_stmmac_pcs(pcs);

	/* The RGMII interface does not have the GMAC_AN_CTRL register */
	if (phy_interface_mode_is_rgmii(spcs->priv->plat->mac_interface))
		return 0;

	__dwmac_ctrl_ane(spcs, true, spcs->priv->hw->ps, false);

	return 0;
}
