/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */
#ifndef	_DWMAC_QCOM_ETHQOS_H
#define	_DWMAC_QCOM_ETHQOS_H

#define DRV_NAME "qcom-ethqos"
#define ETHQOSDBG(fmt, args...) \
	pr_debug(DRV_NAME " %s:%d " fmt, __func__, __LINE__, ## args)
#define ETHQOSERR(fmt, args...) \
	pr_err(DRV_NAME " %s:%d " fmt, __func__, __LINE__, ## args)
#define ETHQOSINFO(fmt, args...) \
	pr_info(DRV_NAME " %s:%d " fmt, __func__, __LINE__, ## args)

#define RGMII_IO_MACRO_CONFIG		0x0
#define SDCC_HC_REG_DLL_CONFIG		0x4
#define SDCC_TEST_CTL			0x8
#define SDCC_HC_REG_DDR_CONFIG		0xC
#define SDCC_HC_REG_DLL_CONFIG2		0x10
#define SDC4_STATUS			0x14
#define SDCC_USR_CTL			0x18
#define RGMII_IO_MACRO_CONFIG2		0x1C
#define EMAC_HW_NONE 0

struct ethqos_emac_por {
	unsigned int offset;
	unsigned int value;
};

struct ethqos_emac_driver_data {
	struct ethqos_emac_por *por;
	unsigned int num_por;
	/* regulators to be requested */
	const char * const *vreg_list;
	int num_vregs;
};

struct qcom_ethqos {
	struct platform_device *pdev;
	void __iomem *rgmii_base;

	unsigned int rgmii_clk_rate;
	struct clk *rgmii_clk;
	unsigned int speed;

	struct ethqos_emac_por *por;
	unsigned int num_por;
	unsigned int emac_ver;

	struct regulator_bulk_data *vregs;
};

void *qcom_ethqos_get_priv(struct qcom_ethqos *ethqos);
#endif
