/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * stmmac_pcs.h: Physical Coding Sublayer Header File
 *
 * Copyright (C) 2016 STMicroelectronics (R&D) Limited
 * Author: Giuseppe Cavallaro <peppe.cavallaro@st.com>
 */

#ifndef __STMMAC_PCS_H__
#define __STMMAC_PCS_H__

#include <linux/slab.h>
#include <linux/io.h>
#include "common.h"

#define PCS_GMAC4_OFFSET		0x000000e0
#define PCS_GMAC3_X_OFFSET		0x000000c0

/* PCS registers (AN/TBI/SGMII/RGMII) offsets */
#define PCS_AN_CTRL		0x00		/* AN control */
#define PCS_AN_STATUS		0x04		/* AN status */
#define PCS_ANE_ADV		0x08		/* ANE Advertisement */
#define PCS_ANE_LPA		0x0c		/* ANE link partener ability */
#define PCS_ANE_EXP		0x10		/* ANE expansion */
#define PCS_TBI_EXT		0x14		/* TBI extended status */

/* AN Configuration defines */
#define PCS_AN_CTRL_RAN		BIT(9)		/* Restart Auto-Negotiation */
#define PCS_AN_CTRL_ANE		BIT(12)		/* Auto-Negotiation Enable */
#define PCS_AN_CTRL_ELE		BIT(14)		/* External Loopback Enable */
#define PCS_AN_CTRL_ECD		BIT(16)		/* Enable Comma Detect */
#define PCS_AN_CTRL_LR		BIT(17)		/* Lock to Reference */
#define PCS_AN_CTRL_SGMRAL	BIT(18)		/* SGMII RAL Control */

/* AN Status defines */
#define PCS_AN_STATUS_LS	BIT(2)		/* Link Status 0:down 1:up */
#define PCS_AN_STATUS_ANA	BIT(3)		/* Auto-Negotiation Ability */
#define PCS_AN_STATUS_ANC	BIT(5)		/* Auto-Negotiation Complete */
#define PCS_AN_STATUS_ES	BIT(8)		/* Extended Status Ability */

/* ADV and LPA defines */
#define PCS_ANE_FD		BIT(5)		/* AN Full-duplex flag */
#define PCS_ANE_HD		BIT(6)		/* AN Half-duplex flag */
#define PCS_ANE_PSE		GENMASK(8, 7)	/* AN Pause Encoding */
#define PCS_ANE_PSE_SHIFT	7
#define PCS_ANE_RFE		GENMASK(13, 12)	/* AN Remote Fault Encoding */
#define PCS_ANE_RFE_SHIFT	12
#define PCS_ANE_ACK		BIT(14)		/* AN Base-page acknowledge */

/* SGMII/RGMII/SMII link status register */
#define PCS_CFG_LNKMOD		BIT(0)		/* Link Duplex Mode */
#define PCS_CFG_LNKSPEED	GENMASK(2, 1)	/* Link Speed: */
#define PCS_CFG_LNKSPEED_2_5	0x0		/* 2.5 MHz - 10 Mbps */
#define PCS_CFG_LNKSPEED_25	0x1		/* 25 MHz - 100 Mbps */
#define PCS_CFG_LNKSPEED_250	0x2		/* 250 MHz - 1000 Mbps */
#define PCS_CFG_LNKSTS		BIT(3)		/* Link Up/Down Status */
#define PCS_CFG_JABTO		BIT(4)		/* Jabber Timeout (SMII only) */
#define PCS_CFG_FALSCARDET	BIT(5)		/* False Carrier (SMII only) */

/**
 * dwmac_pcs_isr - TBI, RTBI, or SGMII PHY ISR
 * @ioaddr: IO registers pointer
 * @intr_status: GMAC core interrupt status
 * @x: pointer to log these events as stats
 * Description: it is the ISR for PCS events: Auto-Negotiation Completed and
 * Link status.
 */
static inline void dwmac_pcs_isr(void __iomem *pcsaddr,
				 unsigned int intr_status,
				 struct stmmac_extra_stats *x)
{
	u32 val = readl(pcsaddr + PCS_AN_STATUS);

	if (intr_status & PCS_ANE_IRQ) {
		x->irq_pcs_ane_n++;
		if (val & PCS_AN_STATUS_ANC)
			pr_info("stmmac_pcs: ANE process completed\n");
	}

	if (intr_status & PCS_LINK_IRQ) {
		x->irq_pcs_link_n++;
		if (val & PCS_AN_STATUS_LS)
			pr_info("stmmac_pcs: Link Up\n");
		else
			pr_info("stmmac_pcs: Link Down\n");
	}
}

#endif /* __STMMAC_PCS_H__ */
