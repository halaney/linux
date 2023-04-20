// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved. */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>

#include "phy-qcom-sgmii-eth.h"

struct qcom_dwmac_sgmii_phy {
	void __iomem *mmio;
	struct clk *sgmiref_clk;
	struct device *dev;
	int cur_speed;
	int new_speed;
};

static int poll_status(struct device *dev, void __iomem *sgmii_base)
{
	int retry = 500;
	unsigned int val;
	int ret = 0;

	do {
		val = readl_relaxed(sgmii_base + QSERDES_COM_C_READY_STATUS);
		val &= QSERDES_COM_C_READY;
		if (val)
			break;
		usleep_range(1000, 1500);
		retry--;
	} while (retry > 0);
	if (!retry) {
		dev_err(dev,
			"QSERDES_COM_C_READY_STATUS timedout, retry = %d\n",
			retry);
		ret = -1;
		goto err_ret;
	}

	retry = 500;
	do {
		val = readl_relaxed(sgmii_base +
				    QSERDES_PCS_PCS_READY_STATUS);
		val &= QSERDES_PCS_READY;
		if (val)
			break;
		usleep_range(1000, 1500);
		retry--;
	} while (retry > 0);
	if (!retry) {
		dev_err(dev, "PCS_READY timedout, retry = %d\n", retry);
		ret = -1;
		goto err_ret;
	}

	retry = 500;
	do {
		val = readl_relaxed(sgmii_base +
				    QSERDES_PCS_PCS_READY_STATUS);
		val &= QSERDES_PCS_SGMIIPHY_READY;
		if (val)
			break;
		usleep_range(1000, 1500);
		retry--;
	} while (retry > 0);
	if (!retry) {
		dev_err(dev, "SGMIIPHY_READY timedout, retry = %d\n", retry);
		ret = -1;
		goto err_ret;
	}

	retry = 5000;
	do {
		val = readl_relaxed(sgmii_base + QSERDES_COM_CMN_STATUS);
		val &= QSERDES_COM_C_PLL_LOCKED;
		if (val)
			break;
		usleep_range(1000, 1500);
		retry--;
	} while (retry > 0);
	if (!retry) {
		dev_err(dev, "PLL Lock Status timedout, retry = %d\n", retry);
		ret = -1;
		goto err_ret;
	}

err_ret:
	return ret;
}

static int qcom_dwmac_sgmii_1g_phy_init(struct device *dev,
					void __iomem *sgmii_base)
{
	/****************MODULE: SGMII_PHY_SGMII_PCS**************************/
	writel_relaxed(0x01, sgmii_base + QSERDES_PCS_SW_RESET);
	writel_relaxed(0x01, sgmii_base + QSERDES_PCS_POWER_DOWN_CONTROL);

	/***************** MODULE: QSERDES_COM_SGMII_QMP_PLL*********/
	writel_relaxed(0x0F, sgmii_base + QSERDES_COM_PLL_IVCO);
	writel_relaxed(0x06, sgmii_base + QSERDES_COM_CP_CTRL_MODE0);
	writel_relaxed(0x16, sgmii_base + QSERDES_COM_PLL_RCTRL_MODE0);
	writel_relaxed(0x36, sgmii_base + QSERDES_COM_PLL_CCTRL_MODE0);
	writel_relaxed(0x1A, sgmii_base + QSERDES_COM_SYSCLK_EN_SEL);
	writel_relaxed(0x0A, sgmii_base + QSERDES_COM_LOCK_CMP1_MODE0);
	writel_relaxed(0x1A, sgmii_base + QSERDES_COM_LOCK_CMP2_MODE0);
	writel_relaxed(0x82, sgmii_base + QSERDES_COM_DEC_START_MODE0);
	writel_relaxed(0x55, sgmii_base + QSERDES_COM_DIV_FRAC_START1_MODE0);
	writel_relaxed(0x55, sgmii_base + QSERDES_COM_DIV_FRAC_START2_MODE0);
	writel_relaxed(0x03, sgmii_base + QSERDES_COM_DIV_FRAC_START3_MODE0);
	writel_relaxed(0x24, sgmii_base + QSERDES_COM_VCO_TUNE1_MODE0);

	writel_relaxed(0x02, sgmii_base + QSERDES_COM_VCO_TUNE2_MODE0);
	writel_relaxed(0x00, sgmii_base + QSERDES_COM_VCO_TUNE_INITVAL2);
	writel_relaxed(0x04, sgmii_base + QSERDES_COM_HSCLK_SEL);
	writel_relaxed(0x00, sgmii_base + QSERDES_COM_HSCLK_HS_SWITCH_SEL);
	writel_relaxed(0x0A, sgmii_base + QSERDES_COM_CORECLK_DIV_MODE0);
	writel_relaxed(0x00, sgmii_base + QSERDES_COM_CORE_CLK_EN);
	writel_relaxed(0xB9, sgmii_base +
		       QSERDES_COM_BIN_VCOCAL_CMP_CODE1_MODE0);
	writel_relaxed(0x1E, sgmii_base +
		       QSERDES_COM_BIN_VCOCAL_CMP_CODE2_MODE0);
	writel_relaxed(0x11, sgmii_base + QSERDES_COM_BIN_VCOCAL_HSCLK_SEL);

	/******************MODULE: QSERDES_TX0_SGMII_QMP_TX***********************/
	writel_relaxed(0x05, sgmii_base + QSERDES_TX_TX_BAND);
	writel_relaxed(0x0A, sgmii_base + QSERDES_TX_SLEW_CNTL);
	writel_relaxed(0x09, sgmii_base + QSERDES_TX_RES_CODE_LANE_OFFSET_TX);
	writel_relaxed(0x09, sgmii_base + QSERDES_TX_RES_CODE_LANE_OFFSET_RX);
	writel_relaxed(0x05, sgmii_base + QSERDES_TX_LANE_MODE_1);
	writel_relaxed(0x00, sgmii_base + QSERDES_TX_LANE_MODE_3);
	writel_relaxed(0x12, sgmii_base + QSERDES_TX_RCV_DETECT_LVL_2);
	writel_relaxed(0x0C, sgmii_base + QSERDES_TX_TRAN_DRVR_EMP_EN);

	/*****************MODULE: QSERDES_RX0_SGMII_QMP_RX*******************/
	writel_relaxed(0x0A, sgmii_base + QSERDES_RX_UCDR_FO_GAIN);
	writel_relaxed(0x06, sgmii_base + QSERDES_RX_UCDR_SO_GAIN);
	writel_relaxed(0x0A, sgmii_base + QSERDES_RX_UCDR_FASTLOCK_FO_GAIN);
	writel_relaxed(0x7F, sgmii_base +
		       QSERDES_RX_UCDR_SO_SATURATION_AND_ENABLE);
	writel_relaxed(0x00, sgmii_base + QSERDES_RX_UCDR_FASTLOCK_COUNT_LOW);
	writel_relaxed(0x01, sgmii_base + QSERDES_RX_UCDR_FASTLOCK_COUNT_HIGH);
	writel_relaxed(0x81, sgmii_base + QSERDES_RX_UCDR_PI_CONTROLS);
	writel_relaxed(0x80, sgmii_base + QSERDES_RX_UCDR_PI_CTRL2);
	writel_relaxed(0x04, sgmii_base + QSERDES_RX_RX_TERM_BW);
	writel_relaxed(0x08, sgmii_base + QSERDES_RX_VGA_CAL_CNTRL2);
	writel_relaxed(0x0F, sgmii_base + QSERDES_RX_GM_CAL);
	writel_relaxed(0x04, sgmii_base + QSERDES_RX_RX_EQU_ADAPTOR_CNTRL1);
	writel_relaxed(0x00, sgmii_base + QSERDES_RX_RX_EQU_ADAPTOR_CNTRL2);
	writel_relaxed(0x4A, sgmii_base + QSERDES_RX_RX_EQU_ADAPTOR_CNTRL3);
	writel_relaxed(0x0A, sgmii_base + QSERDES_RX_RX_EQU_ADAPTOR_CNTRL4);
	writel_relaxed(0x80, sgmii_base + QSERDES_RX_RX_IDAC_TSETTLE_LOW);
	writel_relaxed(0x01, sgmii_base + QSERDES_RX_RX_IDAC_TSETTLE_HIGH);
	writel_relaxed(0x20, sgmii_base + QSERDES_RX_RX_IDAC_MEASURE_TIME);
	writel_relaxed(0x17, sgmii_base +
		       QSERDES_RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1);
	writel_relaxed(0x00, sgmii_base + QSERDES_RX_RX_OFFSET_ADAPTOR_CNTRL2);
	writel_relaxed(0x0F, sgmii_base + QSERDES_RX_SIGDET_CNTRL);
	writel_relaxed(0x1E, sgmii_base + QSERDES_RX_SIGDET_DEGLITCH_CNTRL);
	writel_relaxed(0x05, sgmii_base + QSERDES_RX_RX_BAND);
	writel_relaxed(0xE0, sgmii_base + QSERDES_RX_RX_MODE_00_LOW);
	writel_relaxed(0xC8, sgmii_base + QSERDES_RX_RX_MODE_00_HIGH);
	writel_relaxed(0xC8, sgmii_base + QSERDES_RX_RX_MODE_00_HIGH2);
	writel_relaxed(0x09, sgmii_base + QSERDES_RX_RX_MODE_00_HIGH3);
	writel_relaxed(0xB1, sgmii_base + QSERDES_RX_RX_MODE_00_HIGH4);
	writel_relaxed(0xE0, sgmii_base + QSERDES_RX_RX_MODE_01_LOW);
	writel_relaxed(0xC8, sgmii_base + QSERDES_RX_RX_MODE_01_HIGH);
	writel_relaxed(0xC8, sgmii_base + QSERDES_RX_RX_MODE_01_HIGH2);
	writel_relaxed(0x09, sgmii_base + QSERDES_RX_RX_MODE_01_HIGH3);
	writel_relaxed(0xB1, sgmii_base + QSERDES_RX_RX_MODE_01_HIGH4);
	writel_relaxed(0xE0, sgmii_base + QSERDES_RX_RX_MODE_10_LOW);
	writel_relaxed(0xC8, sgmii_base + QSERDES_RX_RX_MODE_10_HIGH);
	writel_relaxed(0xC8, sgmii_base + QSERDES_RX_RX_MODE_10_HIGH2);
	writel_relaxed(0x3B, sgmii_base + QSERDES_RX_RX_MODE_10_HIGH3);
	writel_relaxed(0xB7, sgmii_base + QSERDES_RX_RX_MODE_10_HIGH4);
	writel_relaxed(0x0C, sgmii_base + QSERDES_RX_DCC_CTRL1);

	/****************MODULE: SGMII_PHY_SGMII_PCS**************************/
	writel_relaxed(0x0C, sgmii_base + QSERDES_PCS_LINE_RESET_TIME);
	writel_relaxed(0x1F, sgmii_base + QSERDES_PCS_TX_LARGE_AMP_DRV_LVL);
	writel_relaxed(0x03, sgmii_base + QSERDES_PCS_TX_SMALL_AMP_DRV_LVL);
	writel_relaxed(0x83, sgmii_base + QSERDES_PCS_TX_MID_TERM_CTRL1);
	writel_relaxed(0x08, sgmii_base + QSERDES_PCS_TX_MID_TERM_CTRL2);
	writel_relaxed(0x0C, sgmii_base + QSERDES_PCS_SGMII_MISC_CTRL8);
	writel_relaxed(0x00, sgmii_base + QSERDES_PCS_SW_RESET);

	writel_relaxed(0x01, sgmii_base + QSERDES_PCS_PHY_START);

	return poll_status(dev, sgmii_base);
}

static int qcom_dwmac_sgmii_2p5g_phy_init(struct device *dev,
				   void __iomem *sgmii_base)
{
	/****************MODULE: SGMII_PHY_SGMII_PCS**************************/
	writel_relaxed(0x01, sgmii_base + QSERDES_PCS_SW_RESET);
	writel_relaxed(0x01, sgmii_base + QSERDES_PCS_POWER_DOWN_CONTROL);

	/***************** MODULE: QSERDES_COM_SGMII_QMP_PLL*********/
	writel_relaxed(0x0F, sgmii_base + QSERDES_COM_PLL_IVCO);
	writel_relaxed(0x06, sgmii_base + QSERDES_COM_CP_CTRL_MODE0);
	writel_relaxed(0x16, sgmii_base + QSERDES_COM_PLL_RCTRL_MODE0);
	writel_relaxed(0x36, sgmii_base + QSERDES_COM_PLL_CCTRL_MODE0);
	writel_relaxed(0x1A, sgmii_base + QSERDES_COM_SYSCLK_EN_SEL);
	writel_relaxed(0x1A, sgmii_base + QSERDES_COM_LOCK_CMP1_MODE0);
	writel_relaxed(0x41, sgmii_base + QSERDES_COM_LOCK_CMP2_MODE0);
	writel_relaxed(0x7A, sgmii_base + QSERDES_COM_DEC_START_MODE0);
	writel_relaxed(0x00, sgmii_base + QSERDES_COM_DIV_FRAC_START1_MODE0);
	writel_relaxed(0x20, sgmii_base + QSERDES_COM_DIV_FRAC_START2_MODE0);
	writel_relaxed(0x01, sgmii_base + QSERDES_COM_DIV_FRAC_START3_MODE0);
	writel_relaxed(0xA1, sgmii_base + QSERDES_COM_VCO_TUNE1_MODE0);

	writel_relaxed(0x02, sgmii_base + QSERDES_COM_VCO_TUNE2_MODE0);
	writel_relaxed(0x00, sgmii_base + QSERDES_COM_VCO_TUNE_INITVAL2);
	writel_relaxed(0x03, sgmii_base + QSERDES_COM_HSCLK_SEL);
	writel_relaxed(0x00, sgmii_base + QSERDES_COM_HSCLK_HS_SWITCH_SEL);
	writel_relaxed(0x05, sgmii_base + QSERDES_COM_CORECLK_DIV_MODE0);
	writel_relaxed(0x00, sgmii_base + QSERDES_COM_CORE_CLK_EN);
	writel_relaxed(0xCD, sgmii_base +
		       QSERDES_COM_BIN_VCOCAL_CMP_CODE1_MODE0);
	writel_relaxed(0x1C, sgmii_base +
		       QSERDES_COM_BIN_VCOCAL_CMP_CODE2_MODE0);
	writel_relaxed(0x11, sgmii_base + QSERDES_COM_BIN_VCOCAL_HSCLK_SEL);

	/******************MODULE: QSERDES_TX0_SGMII_QMP_TX*******************/
	writel_relaxed(0x04, sgmii_base + QSERDES_TX_TX_BAND);
	writel_relaxed(0x0A, sgmii_base + QSERDES_TX_SLEW_CNTL);
	writel_relaxed(0x09, sgmii_base + QSERDES_TX_RES_CODE_LANE_OFFSET_TX);
	writel_relaxed(0x02, sgmii_base + QSERDES_TX_RES_CODE_LANE_OFFSET_RX);
	writel_relaxed(0x05, sgmii_base + QSERDES_TX_LANE_MODE_1);
	writel_relaxed(0x00, sgmii_base + QSERDES_TX_LANE_MODE_3);
	writel_relaxed(0x12, sgmii_base + QSERDES_TX_RCV_DETECT_LVL_2);
	writel_relaxed(0x0C, sgmii_base + QSERDES_TX_TRAN_DRVR_EMP_EN);

	/*****************MODULE: QSERDES_RX0_SGMII_QMP_RX********************/
	writel_relaxed(0x0A, sgmii_base + QSERDES_RX_UCDR_FO_GAIN);
	writel_relaxed(0x06, sgmii_base + QSERDES_RX_UCDR_SO_GAIN);
	writel_relaxed(0x0A, sgmii_base + QSERDES_RX_UCDR_FASTLOCK_FO_GAIN);
	writel_relaxed(0x7F, sgmii_base +
		       QSERDES_RX_UCDR_SO_SATURATION_AND_ENABLE);
	writel_relaxed(0x00, sgmii_base + QSERDES_RX_UCDR_FASTLOCK_COUNT_LOW);
	writel_relaxed(0x01, sgmii_base + QSERDES_RX_UCDR_FASTLOCK_COUNT_HIGH);
	writel_relaxed(0x81, sgmii_base + QSERDES_RX_UCDR_PI_CONTROLS);
	writel_relaxed(0x80, sgmii_base + QSERDES_RX_UCDR_PI_CTRL2);
	writel_relaxed(0x00, sgmii_base + QSERDES_RX_RX_TERM_BW);
	writel_relaxed(0x08, sgmii_base + QSERDES_RX_VGA_CAL_CNTRL2);
	writel_relaxed(0x0F, sgmii_base + QSERDES_RX_GM_CAL);
	writel_relaxed(0x04, sgmii_base + QSERDES_RX_RX_EQU_ADAPTOR_CNTRL1);
	writel_relaxed(0x00, sgmii_base + QSERDES_RX_RX_EQU_ADAPTOR_CNTRL2);
	writel_relaxed(0x4A, sgmii_base + QSERDES_RX_RX_EQU_ADAPTOR_CNTRL3);
	writel_relaxed(0x0A, sgmii_base + QSERDES_RX_RX_EQU_ADAPTOR_CNTRL4);
	writel_relaxed(0x80, sgmii_base + QSERDES_RX_RX_IDAC_TSETTLE_LOW);
	writel_relaxed(0x01, sgmii_base + QSERDES_RX_RX_IDAC_TSETTLE_HIGH);
	writel_relaxed(0x20, sgmii_base + QSERDES_RX_RX_IDAC_MEASURE_TIME);
	writel_relaxed(0x17, sgmii_base +
		       QSERDES_RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1);
	writel_relaxed(0x00, sgmii_base + QSERDES_RX_RX_OFFSET_ADAPTOR_CNTRL2);
	writel_relaxed(0x0F, sgmii_base + QSERDES_RX_SIGDET_CNTRL);
	writel_relaxed(0x1E, sgmii_base + QSERDES_RX_SIGDET_DEGLITCH_CNTRL);
	writel_relaxed(0x18, sgmii_base + QSERDES_RX_RX_BAND);
	writel_relaxed(0x18, sgmii_base + QSERDES_RX_RX_MODE_00_LOW);
	writel_relaxed(0xC8, sgmii_base + QSERDES_RX_RX_MODE_00_HIGH);
	writel_relaxed(0xC8, sgmii_base + QSERDES_RX_RX_MODE_00_HIGH2);
	writel_relaxed(0x0C, sgmii_base + QSERDES_RX_RX_MODE_00_HIGH3);
	writel_relaxed(0xB8, sgmii_base + QSERDES_RX_RX_MODE_00_HIGH4);
	writel_relaxed(0xE0, sgmii_base + QSERDES_RX_RX_MODE_01_LOW);
	writel_relaxed(0xC8, sgmii_base + QSERDES_RX_RX_MODE_01_HIGH);
	writel_relaxed(0xC8, sgmii_base + QSERDES_RX_RX_MODE_01_HIGH2);
	writel_relaxed(0x09, sgmii_base + QSERDES_RX_RX_MODE_01_HIGH3);
	writel_relaxed(0xB1, sgmii_base + QSERDES_RX_RX_MODE_01_HIGH4);
	writel_relaxed(0xE0, sgmii_base + QSERDES_RX_RX_MODE_10_LOW);
	writel_relaxed(0xC8, sgmii_base + QSERDES_RX_RX_MODE_10_HIGH);
	writel_relaxed(0xC8, sgmii_base + QSERDES_RX_RX_MODE_10_HIGH2);
	writel_relaxed(0x3B, sgmii_base + QSERDES_RX_RX_MODE_10_HIGH3);
	writel_relaxed(0xB7, sgmii_base + QSERDES_RX_RX_MODE_10_HIGH4);
	writel_relaxed(0x0C, sgmii_base + QSERDES_RX_DCC_CTRL1);

	/****************MODULE: SGMII_PHY_SGMII_PCS**************************/
	writel_relaxed(0x0C, sgmii_base + QSERDES_PCS_LINE_RESET_TIME);
	writel_relaxed(0x1F, sgmii_base + QSERDES_PCS_TX_LARGE_AMP_DRV_LVL);
	writel_relaxed(0x03, sgmii_base + QSERDES_PCS_TX_SMALL_AMP_DRV_LVL);
	writel_relaxed(0x83, sgmii_base + QSERDES_PCS_TX_MID_TERM_CTRL1);
	writel_relaxed(0x08, sgmii_base + QSERDES_PCS_TX_MID_TERM_CTRL2);
	writel_relaxed(0x8C, sgmii_base + QSERDES_PCS_SGMII_MISC_CTRL8);
	writel_relaxed(0x00, sgmii_base + QSERDES_PCS_SW_RESET);

	writel_relaxed(0x01, sgmii_base + QSERDES_PCS_PHY_START);

	return poll_status(dev, sgmii_base);
}

static int qcom_dwmac_sgmii_phy_init(struct phy *generic_phy)
{
	struct qcom_dwmac_sgmii_phy *phy = phy_get_drvdata(generic_phy);
	void __iomem *sgmii = phy->mmio;
	struct device *dev = phy->dev;
	int ret = 0;

	switch (phy->new_speed) {
	case SPEED_1000:
	case SPEED_100:
	case SPEED_10:
		if (phy->cur_speed != SPEED_1000) {
			dev_info(dev, "%s : Serdes Speed set to 1GB speed\n",
				 __func__);
			ret = qcom_dwmac_sgmii_1g_phy_init(dev, sgmii);
			phy->cur_speed = SPEED_1000;
		}

		break;
	case SPEED_2500:
		if (phy->cur_speed != SPEED_2500) {
			dev_info(dev, "%s : Serdes Speed set to 2.5GB speed\n",
				 __func__);
			ret = qcom_dwmac_sgmii_2p5g_phy_init(dev, sgmii);
			phy->cur_speed = SPEED_2500;
		}
		break;
	default:
		dev_err(dev, "%s : Sgmii serdes phy not init for speed = %d\n",
			__func__, phy->new_speed);
		phy->cur_speed = 0;
		break;
	}

	return ret;
}

static int qcom_dwmac_sgmii_phy_exit(struct phy *generic_phy)
{
	struct qcom_dwmac_sgmii_phy *phy = phy_get_drvdata(generic_phy);
	void __iomem *sgmii_base = phy->mmio;

	/* Power down sequence */
	writel_relaxed(0x08, sgmii_base + QSERDES_PCS_TX_MID_TERM_CTRL2);
	writel_relaxed(0x01, sgmii_base + QSERDES_PCS_SW_RESET);

	udelay(100);
	writel_relaxed(0x00, sgmii_base + QSERDES_PCS_SW_RESET);
	writel_relaxed(0x01, sgmii_base + QSERDES_PCS_PHY_START);

	return 0;
}

static int qcom_dwmac_set_speed(struct phy *generic_phy, int speed)
{
	struct qcom_dwmac_sgmii_phy *phy = phy_get_drvdata(generic_phy);

	dev_info(phy->dev, "%s : new speed = %d\n", __func__, speed);
	phy->new_speed = speed;
	return 0;
}

static int qcom_dwmac_sgmii_phy_calibrate(struct phy *generic_phy)
{
	return qcom_dwmac_sgmii_phy_init(generic_phy);
}

static const struct phy_ops qcom_dwmac_sgmii_phy_ops = {
	.init		= qcom_dwmac_sgmii_phy_init,
	.exit		= qcom_dwmac_sgmii_phy_exit,
	.set_speed	= qcom_dwmac_set_speed,
	.calibrate	= qcom_dwmac_sgmii_phy_calibrate,
	.owner		= THIS_MODULE,
};

static int qcom_dwmac_sgmii_phy_probe(struct platform_device *pdev)
{
	struct qcom_dwmac_sgmii_phy *phy;
	struct device *dev = &pdev->dev;
	struct phy_provider *phy_provider;
	struct phy *generic_phy;
	int ret;

	dev_info(dev, "%s Enter\n", __func__);
	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	phy->mmio = devm_platform_ioremap_resource_byname(pdev, "serdes");
	if (IS_ERR(phy->mmio))
		return PTR_ERR(phy->mmio);

	generic_phy = devm_phy_create(dev, NULL, &qcom_dwmac_sgmii_phy_ops);
	if (IS_ERR(generic_phy)) {
		dev_err(dev, "%s: failed to create phy\n", __func__);
		return PTR_ERR(generic_phy);
	}

	phy->dev = dev;
	phy->cur_speed = 0;
	phy->new_speed = 0;
	phy_set_drvdata(generic_phy, phy);
	platform_set_drvdata(pdev, phy);

	phy->sgmiref_clk = devm_clk_get(dev, "sgmi_ref");
	if (IS_ERR(phy->sgmiref_clk)) {
		dev_err(dev, "Failed get sgmi_ref clk\n");
		return PTR_ERR(phy->sgmiref_clk);
	}

	ret = clk_prepare_enable(phy->sgmiref_clk);
	if (ret) {
		dev_err(dev, "Failed enable sgmi_ref clk\n");
		return ret;
	}

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(phy_provider)) {

		dev_err(dev, "failed to register phy\n");
		ret = PTR_ERR(phy_provider);
		goto err_phy_register;
	}

	return 0;

err_phy_register:
	clk_disable_unprepare(phy->sgmiref_clk);
	return ret;
}

static int qcom_dwmac_sgmii_phy_remove(struct platform_device *pdev)
{
	struct qcom_dwmac_sgmii_phy *phy = platform_get_drvdata(pdev);

	clk_disable_unprepare(phy->sgmiref_clk);

	return 0;
}

static const struct of_device_id qcom_dwmac_sgmii_phy_of_match[] = {
	{ .compatible = "qcom,sa8775p-dwmac-sgmii-phy" },
	{ },
};
MODULE_DEVICE_TABLE(of, qcom_dwmac_sgmii_phy_of_match);

static struct platform_driver qcom_dwmac_sgmii_phy_driver = {
	.probe	= qcom_dwmac_sgmii_phy_probe,
	.remove	= qcom_dwmac_sgmii_phy_remove,
	.driver = {
		.name	= "qcom-dwmac-sgmii-phy",
		.of_match_table	= qcom_dwmac_sgmii_phy_of_match,
	}
};

module_platform_driver(qcom_dwmac_sgmii_phy_driver);

MODULE_DESCRIPTION("QCOM DWMAC sgmii PHY driver");
MODULE_LICENSE("GPL v2");