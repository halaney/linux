// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018-19, Linaro Limited

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include "dwmac-qcom-serdes.h"
#include "stmmac.h"
#include "stmmac_platform.h"

#include <linux/regulator/consumer.h>

#define RGMII_IO_MACRO_CONFIG		0x0
#define SDCC_HC_REG_DLL_CONFIG		0x4
#define SDCC_TEST_CTL			0x8
#define SDCC_HC_REG_DDR_CONFIG		0xC
#define SDCC_HC_REG_DLL_CONFIG2		0x10
#define SDC4_STATUS			0x14
#define SDCC_USR_CTL			0x18
#define RGMII_IO_MACRO_CONFIG2		0x1C
#define RGMII_IO_MACRO_DEBUG1		0x20
#define EMAC_SYSTEM_LOW_POWER_DEBUG	0x28

/* RGMII_IO_MACRO_CONFIG fields */
#define RGMII_CONFIG_FUNC_CLK_EN		BIT(30)
#define RGMII_CONFIG_POS_NEG_DATA_SEL		BIT(23)
#define RGMII_CONFIG_GPIO_CFG_RX_INT		GENMASK(21, 20)
#define RGMII_CONFIG_GPIO_CFG_TX_INT		GENMASK(19, 17)
#define RGMII_CONFIG_MAX_SPD_PRG_9		GENMASK(16, 8)
#define RGMII_CONFIG_MAX_SPD_PRG_2		GENMASK(7, 6)
#define RGMII_CONFIG_INTF_SEL			GENMASK(5, 4)
#define RGMII_CONFIG_BYPASS_TX_ID_EN		BIT(3)
#define RGMII_CONFIG_LOOPBACK_EN		BIT(2)
#define RGMII_CONFIG_PROG_SWAP			BIT(1)
#define RGMII_CONFIG_DDR_MODE			BIT(0)

/* SDCC_HC_REG_DLL_CONFIG fields */
#define SDCC_DLL_CONFIG_DLL_RST			BIT(30)
#define SDCC_DLL_CONFIG_PDN			BIT(29)
#define SDCC_DLL_CONFIG_MCLK_FREQ		GENMASK(26, 24)
#define SDCC_DLL_CONFIG_CDR_SELEXT		GENMASK(23, 20)
#define SDCC_DLL_CONFIG_CDR_EXT_EN		BIT(19)
#define SDCC_DLL_CONFIG_CK_OUT_EN		BIT(18)
#define SDCC_DLL_CONFIG_CDR_EN			BIT(17)
#define SDCC_DLL_CONFIG_DLL_EN			BIT(16)
#define SDCC_DLL_MCLK_GATING_EN			BIT(5)
#define SDCC_DLL_CDR_FINE_PHASE			GENMASK(3, 2)

/* SDCC_HC_REG_DDR_CONFIG fields */
#define SDCC_DDR_CONFIG_PRG_DLY_EN		BIT(31)
#define SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY	GENMASK(26, 21)
#define SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_CODE	GENMASK(29, 27)
#define SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_EN	BIT(30)
#define SDCC_DDR_CONFIG_TCXO_CYCLES_CNT		GENMASK(11, 9)
#define SDCC_DDR_CONFIG_PRG_RCLK_DLY		GENMASK(8, 0)

/* SDCC_HC_REG_DLL_CONFIG2 fields */
#define SDCC_DLL_CONFIG2_DLL_CLOCK_DIS		BIT(21)
#define SDCC_DLL_CONFIG2_MCLK_FREQ_CALC		GENMASK(17, 10)
#define SDCC_DLL_CONFIG2_DDR_TRAFFIC_INIT_SEL	GENMASK(3, 2)
#define SDCC_DLL_CONFIG2_DDR_TRAFFIC_INIT_SW	BIT(1)
#define SDCC_DLL_CONFIG2_DDR_CAL_EN		BIT(0)

/* SDC4_STATUS bits */
#define SDC4_STATUS_DLL_LOCK			BIT(7)

/* RGMII_IO_MACRO_CONFIG2 fields */
#define RGMII_CONFIG2_RSVD_CONFIG15		GENMASK(31, 17)
#define RGMII_CONFIG2_RGMII_CLK_SEL_CFG		BIT(16)
#define RGMII_CONFIG2_TX_TO_RX_LOOPBACK_EN	BIT(13)
#define RGMII_CONFIG2_CLK_DIVIDE_SEL		BIT(12)
#define RGMII_CONFIG2_RX_PROG_SWAP		BIT(7)
#define RGMII_CONFIG2_DATA_DIVIDE_CLK_SEL	BIT(6)
#define RGMII_CONFIG2_TX_CLK_PHASE_SHIFT_EN	BIT(5)

struct ethqos_emac_por {
	unsigned int offset;
	unsigned int value;
};

struct ethqos_emac_driver_data {
	const struct ethqos_emac_por *por;
	unsigned int num_por;
	bool rgmii_config_loopback_en;
	bool has_emac3;
	struct dwmac4_addrs dwmac4_addrs;
};

struct qcom_ethqos {
	struct platform_device *pdev;
	void __iomem *ioaddr;
	void __iomem *rgmii_base;
	void __iomem *sgmii_base;

	unsigned int rgmii_clk_rate;
	struct clk *rgmii_clk;
	struct clk *phyaux_clk;
	struct clk *sgmiref_clk;
	unsigned int speed;

	const struct ethqos_emac_por *por;
	unsigned int num_por;
	bool rgmii_config_loopback_en;
	bool has_emac3;
};

static int rgmii_readl(struct qcom_ethqos *ethqos, unsigned int offset)
{
	return readl(ethqos->rgmii_base + offset);
}

static void rgmii_writel(struct qcom_ethqos *ethqos,
			 int value, unsigned int offset)
{
	writel(value, ethqos->rgmii_base + offset);
}

static void rgmii_updatel(struct qcom_ethqos *ethqos,
			  int mask, int val, unsigned int offset)
{
	unsigned int temp;

	temp =  rgmii_readl(ethqos, offset);
	temp = (temp & ~(mask)) | val;
	rgmii_writel(ethqos, temp, offset);
}

#include "dwmac-qcom-serdes.h"

int configure_serdes_dt(struct qcom_ethqos *ethqos)
{
	struct resource *res = NULL;
	struct platform_device *pdev = ethqos->pdev;
	int ret;

	printk(KERN_ERR "%s: %d\n", __func__, __LINE__);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "serdes");
	ethqos->sgmii_base = devm_ioremap_resource(&pdev->dev, res);
	printk(KERN_ERR "%s: %d\n", __func__, __LINE__);
	if (IS_ERR(ethqos->sgmii_base)) {
		dev_err(&pdev->dev, "Can't get sgmii base\n");
		ret = PTR_ERR(ethqos->sgmii_base);
		goto err_mem;
	}

	ethqos->sgmiref_clk = devm_clk_get(&pdev->dev, "sgmi_ref");
	if (IS_ERR(ethqos->sgmiref_clk)) {
		ret = PTR_ERR(ethqos->sgmiref_clk);
		ethqos->sgmiref_clk = NULL;
		goto err_mem;
	}
	ethqos->phyaux_clk = devm_clk_get(&pdev->dev, "phyaux");
	if (IS_ERR(ethqos->phyaux_clk)) {
		ret = PTR_ERR(ethqos->phyaux_clk);
		goto err_get_phyaux_clk;
	}

	ret = clk_prepare_enable(ethqos->sgmiref_clk);
	if (ret)
		goto err_enable_sgmiref_clk;

	ret = clk_prepare_enable(ethqos->phyaux_clk);
	if (ret)
		goto err_enable_phyaux_clk;

	printk(KERN_ERR "%s: %d\n", __func__, __LINE__);
	return 0;

err_enable_phyaux_clk:
	clk_disable_unprepare(ethqos->sgmiref_clk);
err_enable_sgmiref_clk:
	ethqos->phyaux_clk = NULL;
err_get_phyaux_clk:
	ethqos->sgmiref_clk = NULL;
err_mem:
	return ret;
}

void qcom_ethqos_serdes_init(struct qcom_ethqos *ethqos, int speed)
{
	int retry = 500;
	unsigned int val;

	/****************MODULE: SGMII_PHY_SGMII_PCS**********************************/
	writel_relaxed(0x01, ethqos->sgmii_base + QSERDES_PCS_SW_RESET);
	writel_relaxed(0x01, ethqos->sgmii_base + QSERDES_PCS_POWER_DOWN_CONTROL);

	/***************** MODULE: QSERDES_COM_SGMII_QMP_PLL*********/
	writel_relaxed(0x0F, ethqos->sgmii_base + QSERDES_COM_PLL_IVCO);
	writel_relaxed(0x06, ethqos->sgmii_base + QSERDES_COM_CP_CTRL_MODE0);
	writel_relaxed(0x16, ethqos->sgmii_base + QSERDES_COM_PLL_RCTRL_MODE0);
	writel_relaxed(0x36, ethqos->sgmii_base + QSERDES_COM_PLL_CCTRL_MODE0);
	writel_relaxed(0x1A, ethqos->sgmii_base + QSERDES_COM_SYSCLK_EN_SEL);
	writel_relaxed(0x0A, ethqos->sgmii_base + QSERDES_COM_LOCK_CMP1_MODE0);
	writel_relaxed(0x1A, ethqos->sgmii_base + QSERDES_COM_LOCK_CMP2_MODE0);
	writel_relaxed(0x82, ethqos->sgmii_base + QSERDES_COM_DEC_START_MODE0);
	writel_relaxed(0x55, ethqos->sgmii_base + QSERDES_COM_DIV_FRAC_START1_MODE0);
	writel_relaxed(0x55, ethqos->sgmii_base + QSERDES_COM_DIV_FRAC_START2_MODE0);
	writel_relaxed(0x03, ethqos->sgmii_base + QSERDES_COM_DIV_FRAC_START3_MODE0);
	writel_relaxed(0x24, ethqos->sgmii_base + QSERDES_COM_VCO_TUNE1_MODE0);

	writel_relaxed(0x02, ethqos->sgmii_base + QSERDES_COM_VCO_TUNE2_MODE0);
	writel_relaxed(0x00, ethqos->sgmii_base + QSERDES_COM_VCO_TUNE_INITVAL2);
	writel_relaxed(0x04, ethqos->sgmii_base + QSERDES_COM_HSCLK_SEL);
	writel_relaxed(0x00, ethqos->sgmii_base + QSERDES_COM_HSCLK_HS_SWITCH_SEL);
	writel_relaxed(0x0A, ethqos->sgmii_base + QSERDES_COM_CORECLK_DIV_MODE0);
	writel_relaxed(0x00, ethqos->sgmii_base + QSERDES_COM_CORE_CLK_EN);
	writel_relaxed(0xB9, ethqos->sgmii_base + QSERDES_COM_BIN_VCOCAL_CMP_CODE1_MODE0);
	writel_relaxed(0x1E, ethqos->sgmii_base + QSERDES_COM_BIN_VCOCAL_CMP_CODE2_MODE0);
	writel_relaxed(0x11, ethqos->sgmii_base + QSERDES_COM_BIN_VCOCAL_HSCLK_SEL);

	/******************MODULE: QSERDES_TX0_SGMII_QMP_TX***********************/
	writel_relaxed(0x05, ethqos->sgmii_base + QSERDES_TX_TX_BAND);
	writel_relaxed(0x0A, ethqos->sgmii_base + QSERDES_TX_SLEW_CNTL);
	writel_relaxed(0x09, ethqos->sgmii_base + QSERDES_TX_RES_CODE_LANE_OFFSET_TX);
	writel_relaxed(0x09, ethqos->sgmii_base + QSERDES_TX_RES_CODE_LANE_OFFSET_RX);
	writel_relaxed(0x05, ethqos->sgmii_base + QSERDES_TX_LANE_MODE_1);
	writel_relaxed(0x00, ethqos->sgmii_base + QSERDES_TX_LANE_MODE_3);
	writel_relaxed(0x12, ethqos->sgmii_base + QSERDES_TX_RCV_DETECT_LVL_2);
	writel_relaxed(0x0C, ethqos->sgmii_base + QSERDES_TX_TRAN_DRVR_EMP_EN);

	/*****************MODULE: QSERDES_RX0_SGMII_QMP_RX*******************/
	writel_relaxed(0x0A, ethqos->sgmii_base + QSERDES_RX_UCDR_FO_GAIN);
	writel_relaxed(0x06, ethqos->sgmii_base + QSERDES_RX_UCDR_SO_GAIN);
	writel_relaxed(0x0A, ethqos->sgmii_base + QSERDES_RX_UCDR_FASTLOCK_FO_GAIN);
	writel_relaxed(0x7F, ethqos->sgmii_base + QSERDES_RX_UCDR_SO_SATURATION_AND_ENABLE);
	writel_relaxed(0x00, ethqos->sgmii_base + QSERDES_RX_UCDR_FASTLOCK_COUNT_LOW);
	writel_relaxed(0x01, ethqos->sgmii_base + QSERDES_RX_UCDR_FASTLOCK_COUNT_HIGH);
	writel_relaxed(0x81, ethqos->sgmii_base + QSERDES_RX_UCDR_PI_CONTROLS);
	writel_relaxed(0x80, ethqos->sgmii_base + QSERDES_RX_UCDR_PI_CTRL2);
	writel_relaxed(0x04, ethqos->sgmii_base + QSERDES_RX_RX_TERM_BW);
	writel_relaxed(0x08, ethqos->sgmii_base + QSERDES_RX_VGA_CAL_CNTRL2);
	writel_relaxed(0x0F, ethqos->sgmii_base + QSERDES_RX_GM_CAL);
	writel_relaxed(0x04, ethqos->sgmii_base + QSERDES_RX_RX_EQU_ADAPTOR_CNTRL1);
	writel_relaxed(0x00, ethqos->sgmii_base + QSERDES_RX_RX_EQU_ADAPTOR_CNTRL2);
	writel_relaxed(0x4A, ethqos->sgmii_base + QSERDES_RX_RX_EQU_ADAPTOR_CNTRL3);
	writel_relaxed(0x0A, ethqos->sgmii_base + QSERDES_RX_RX_EQU_ADAPTOR_CNTRL4);
	writel_relaxed(0x80, ethqos->sgmii_base + QSERDES_RX_RX_IDAC_TSETTLE_LOW);
	writel_relaxed(0x01, ethqos->sgmii_base + QSERDES_RX_RX_IDAC_TSETTLE_HIGH);
	writel_relaxed(0x20, ethqos->sgmii_base + QSERDES_RX_RX_IDAC_MEASURE_TIME);
	writel_relaxed(0x17, ethqos->sgmii_base + QSERDES_RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1);
	writel_relaxed(0x00, ethqos->sgmii_base + QSERDES_RX_RX_OFFSET_ADAPTOR_CNTRL2);
	writel_relaxed(0x0F, ethqos->sgmii_base + QSERDES_RX_SIGDET_CNTRL);
	writel_relaxed(0x1E, ethqos->sgmii_base + QSERDES_RX_SIGDET_DEGLITCH_CNTRL);
	writel_relaxed(0x05, ethqos->sgmii_base + QSERDES_RX_RX_BAND);
	writel_relaxed(0xE0, ethqos->sgmii_base + QSERDES_RX_RX_MODE_00_LOW);
	writel_relaxed(0xC8, ethqos->sgmii_base + QSERDES_RX_RX_MODE_00_HIGH);
	writel_relaxed(0xC8, ethqos->sgmii_base + QSERDES_RX_RX_MODE_00_HIGH2);
	writel_relaxed(0x09, ethqos->sgmii_base + QSERDES_RX_RX_MODE_00_HIGH3);
	writel_relaxed(0xB1, ethqos->sgmii_base + QSERDES_RX_RX_MODE_00_HIGH4);
	writel_relaxed(0xE0, ethqos->sgmii_base + QSERDES_RX_RX_MODE_01_LOW);
	writel_relaxed(0xC8, ethqos->sgmii_base + QSERDES_RX_RX_MODE_01_HIGH);
	writel_relaxed(0xC8, ethqos->sgmii_base + QSERDES_RX_RX_MODE_01_HIGH2);
	writel_relaxed(0x09, ethqos->sgmii_base + QSERDES_RX_RX_MODE_01_HIGH3);
	writel_relaxed(0xB1, ethqos->sgmii_base + QSERDES_RX_RX_MODE_01_HIGH4);
	writel_relaxed(0xE0, ethqos->sgmii_base + QSERDES_RX_RX_MODE_10_LOW);
	writel_relaxed(0xC8, ethqos->sgmii_base + QSERDES_RX_RX_MODE_10_HIGH);
	writel_relaxed(0xC8, ethqos->sgmii_base + QSERDES_RX_RX_MODE_10_HIGH2);
	writel_relaxed(0x3B, ethqos->sgmii_base + QSERDES_RX_RX_MODE_10_HIGH3);
	writel_relaxed(0xB7, ethqos->sgmii_base + QSERDES_RX_RX_MODE_10_HIGH4);
	writel_relaxed(0x0C, ethqos->sgmii_base + QSERDES_RX_DCC_CTRL1);

	/****************MODULE: SGMII_PHY_SGMII_PCS**********************************/
	writel_relaxed(0x0C, ethqos->sgmii_base + QSERDES_PCS_LINE_RESET_TIME);
	writel_relaxed(0x1F, ethqos->sgmii_base + QSERDES_PCS_TX_LARGE_AMP_DRV_LVL);
	writel_relaxed(0x03, ethqos->sgmii_base + QSERDES_PCS_TX_SMALL_AMP_DRV_LVL);
	writel_relaxed(0x83, ethqos->sgmii_base + QSERDES_PCS_TX_MID_TERM_CTRL1);
	writel_relaxed(0x08, ethqos->sgmii_base + QSERDES_PCS_TX_MID_TERM_CTRL2);
	writel_relaxed(0x0C, ethqos->sgmii_base + QSERDES_PCS_SGMII_MISC_CTRL8);
	writel_relaxed(0x00, ethqos->sgmii_base + QSERDES_PCS_SW_RESET);

	writel_relaxed(0x01, ethqos->sgmii_base + QSERDES_PCS_PHY_START);

	do {
		val = readl_relaxed(ethqos->sgmii_base + QSERDES_COM_C_READY_STATUS);
		val &= BIT(0);
		if (val)
			break;
		usleep_range(1000, 1500);
		retry--;
	} while (retry > 0);
	if (!retry)
		pr_err("QSERDES_COM_C_READY_STATUS timedout with retry = %d\n", retry);

	retry = 500;
	do {
		val = readl_relaxed(ethqos->sgmii_base + QSERDES_PCS_PCS_READY_STATUS);
		val &= BIT(0);
		if (val)
			break;
		usleep_range(1000, 1500);
		retry--;
	} while (retry > 0);
	if (!retry)
		pr_err("PCS_READY timedout with retry = %d\n", retry);

	retry = 500;
	do {
		val = readl_relaxed(ethqos->sgmii_base + QSERDES_PCS_PCS_READY_STATUS);
		val &= BIT(7);
		if (val)
			break;
		usleep_range(1000, 1500);
		retry--;
	} while (retry > 0);
	if (!retry)
		pr_err("SGMIIPHY_READY timedout with retry = %d\n", retry);

	retry = 5000;
	do {
		val = readl_relaxed(ethqos->sgmii_base + QSERDES_COM_CMN_STATUS);
		val &= BIT(1);
		if (val)
			break;
		usleep_range(1000, 1500);
		retry--;
	} while (retry > 0);
	if (!retry)
		pr_err("PLL Lock Status timedout with retry = %d\n", retry);
}

#define EMAC_VREG_RGMII_NAME "vreg_rgmii"
#define EMAC_VREG_EMAC_PHY_NAME "vreg_emac_phy"
#define EMAC_VREG_RGMII_IO_PADS_NAME "vreg_rgmii_io_pads"
int ethqos_init_regulators(struct qcom_ethqos *ethqos)
{
	int ret = 0;

	struct regulator *reg_rgmii;
	struct regulator *reg_emac_phy;
	struct regulator *reg_rgmii_io_pads;

	if (of_property_read_bool(ethqos->pdev->dev.of_node,
				  "vreg_rgmii-supply")) {
		reg_rgmii =
		devm_regulator_get(&ethqos->pdev->dev, EMAC_VREG_RGMII_NAME);
		if (IS_ERR(reg_rgmii)) {
			printk(KERN_ERR "Can not get <%s>\n", EMAC_VREG_RGMII_NAME);
			return PTR_ERR(reg_rgmii);
		}

		ret = regulator_enable(reg_rgmii);
		if (ret) {
			printk(KERN_ERR "Can not enable <%s>\n",
				  EMAC_VREG_RGMII_NAME);
			goto reg_error;
		}

		printk(KERN_INFO "Enabled <%s>\n", EMAC_VREG_RGMII_NAME);
	}

	if (of_property_read_bool(ethqos->pdev->dev.of_node,
				  "vreg_emac_phy-supply")) {
		reg_emac_phy =
		devm_regulator_get(&ethqos->pdev->dev, EMAC_VREG_EMAC_PHY_NAME);
		if (IS_ERR(reg_emac_phy)) {
			printk(KERN_ERR "Can not get <%s>\n",
				  EMAC_VREG_EMAC_PHY_NAME);
			return PTR_ERR(reg_emac_phy);
		}

		ret = regulator_enable(reg_emac_phy);
		if (ret) {
			printk(KERN_ERR "Can not enable <%s>\n",
				  EMAC_VREG_EMAC_PHY_NAME);
			goto reg_error;
		}

		printk(KERN_INFO "Enabled <%s>\n", EMAC_VREG_EMAC_PHY_NAME);
	}

	if (of_property_read_bool(ethqos->pdev->dev.of_node,
				  "vreg_rgmii_io_pads-supply")) {
		reg_rgmii_io_pads = devm_regulator_get
		(&ethqos->pdev->dev, EMAC_VREG_RGMII_IO_PADS_NAME);
		if (IS_ERR(reg_rgmii_io_pads)) {
			printk(KERN_ERR "Can not get <%s>\n",
				  EMAC_VREG_RGMII_IO_PADS_NAME);
			return PTR_ERR(reg_rgmii_io_pads);
		}

		ret = regulator_enable(reg_rgmii_io_pads);
		if (ret) {
			printk(KERN_ERR "Can not enable <%s>\n",
				  EMAC_VREG_RGMII_IO_PADS_NAME);
			goto reg_error;
		}

		printk(KERN_INFO "Enabled <%s>\n", EMAC_VREG_RGMII_IO_PADS_NAME);
	}

	return ret;

reg_error:
	printk(KERN_ERR "%s failed\n", __func__);
	//ethqos_disable_regulators(ethqos);
	return ret;
}

static void rgmii_dump(void *priv)
{
	struct qcom_ethqos *ethqos = priv;

	dev_dbg(&ethqos->pdev->dev, "Rgmii register dump\n");
	dev_dbg(&ethqos->pdev->dev, "RGMII_IO_MACRO_CONFIG: %x\n",
		rgmii_readl(ethqos, RGMII_IO_MACRO_CONFIG));
	dev_dbg(&ethqos->pdev->dev, "SDCC_HC_REG_DLL_CONFIG: %x\n",
		rgmii_readl(ethqos, SDCC_HC_REG_DLL_CONFIG));
	dev_dbg(&ethqos->pdev->dev, "SDCC_HC_REG_DDR_CONFIG: %x\n",
		rgmii_readl(ethqos, SDCC_HC_REG_DDR_CONFIG));
	dev_dbg(&ethqos->pdev->dev, "SDCC_HC_REG_DLL_CONFIG2: %x\n",
		rgmii_readl(ethqos, SDCC_HC_REG_DLL_CONFIG2));
	dev_dbg(&ethqos->pdev->dev, "SDC4_STATUS: %x\n",
		rgmii_readl(ethqos, SDC4_STATUS));
	dev_dbg(&ethqos->pdev->dev, "SDCC_USR_CTL: %x\n",
		rgmii_readl(ethqos, SDCC_USR_CTL));
	dev_dbg(&ethqos->pdev->dev, "RGMII_IO_MACRO_CONFIG2: %x\n",
		rgmii_readl(ethqos, RGMII_IO_MACRO_CONFIG2));
	dev_dbg(&ethqos->pdev->dev, "RGMII_IO_MACRO_DEBUG1: %x\n",
		rgmii_readl(ethqos, RGMII_IO_MACRO_DEBUG1));
	dev_dbg(&ethqos->pdev->dev, "EMAC_SYSTEM_LOW_POWER_DEBUG: %x\n",
		rgmii_readl(ethqos, EMAC_SYSTEM_LOW_POWER_DEBUG));
}

/* Clock rates */
#define RGMII_1000_NOM_CLK_FREQ			(250 * 1000 * 1000UL)
#define RGMII_ID_MODE_100_LOW_SVS_CLK_FREQ	 (50 * 1000 * 1000UL)
#define RGMII_ID_MODE_10_LOW_SVS_CLK_FREQ	  (5 * 1000 * 1000UL)

static void
ethqos_update_rgmii_clk(struct qcom_ethqos *ethqos, unsigned int speed)
{
	switch (speed) {
	case SPEED_1000:
		ethqos->rgmii_clk_rate =  RGMII_1000_NOM_CLK_FREQ;
		break;

	case SPEED_100:
		ethqos->rgmii_clk_rate =  RGMII_ID_MODE_100_LOW_SVS_CLK_FREQ;
		break;

	case SPEED_10:
		ethqos->rgmii_clk_rate =  RGMII_ID_MODE_10_LOW_SVS_CLK_FREQ;
		break;
	}

	clk_set_rate(ethqos->rgmii_clk, ethqos->rgmii_clk_rate);
}

static void ethqos_set_func_clk_en(struct qcom_ethqos *ethqos)
{
	rgmii_updatel(ethqos, RGMII_CONFIG_FUNC_CLK_EN,
		      RGMII_CONFIG_FUNC_CLK_EN, RGMII_IO_MACRO_CONFIG);
}

static const struct ethqos_emac_por emac_v2_3_0_por[] = {
	{ .offset = RGMII_IO_MACRO_CONFIG,	.value = 0x00C01343 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG,	.value = 0x2004642C },
	{ .offset = SDCC_HC_REG_DDR_CONFIG,	.value = 0x00000000 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG2,	.value = 0x00200000 },
	{ .offset = SDCC_USR_CTL,		.value = 0x00010800 },
	{ .offset = RGMII_IO_MACRO_CONFIG2,	.value = 0x00002060 },
};

static const struct ethqos_emac_driver_data emac_v2_3_0_data = {
	.por = emac_v2_3_0_por,
	.num_por = ARRAY_SIZE(emac_v2_3_0_por),
	.rgmii_config_loopback_en = true,
	.has_emac3 = false,
};

static const struct ethqos_emac_por emac_v2_1_0_por[] = {
	{ .offset = RGMII_IO_MACRO_CONFIG,	.value = 0x40C01343 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG,	.value = 0x2004642C },
	{ .offset = SDCC_HC_REG_DDR_CONFIG,	.value = 0x00000000 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG2,	.value = 0x00200000 },
	{ .offset = SDCC_USR_CTL,		.value = 0x00010800 },
	{ .offset = RGMII_IO_MACRO_CONFIG2,	.value = 0x00002060 },
};

static const struct ethqos_emac_driver_data emac_v2_1_0_data = {
	.por = emac_v2_1_0_por,
	.num_por = ARRAY_SIZE(emac_v2_1_0_por),
	.rgmii_config_loopback_en = false,
	.has_emac3 = false,
};

static const struct ethqos_emac_por emac_v3_0_0_por[] = {
	{ .offset = RGMII_IO_MACRO_CONFIG,	.value = 0x40c01343 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG,	.value = 0x2004642c },
	{ .offset = SDCC_HC_REG_DDR_CONFIG,	.value = 0x80040800 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG2,	.value = 0x00200000 },
	{ .offset = SDCC_USR_CTL,		.value = 0x00010800 },
	{ .offset = RGMII_IO_MACRO_CONFIG2,	.value = 0x00002060 },
};

static const struct ethqos_emac_driver_data emac_v3_0_0_data = {
	.por = emac_v3_0_0_por,
	.num_por = ARRAY_SIZE(emac_v3_0_0_por),
	.rgmii_config_loopback_en = false,
	.has_emac3 = true,
	.dwmac4_addrs = {
		.dma_chan = 0x00008100,
		.dma_chan_offset = 0x1000,
		.mtl_chan = 0x00008000,
		.mtl_chan_offset = 0x1000,
		.mtl_ets_ctrl = 0x00008010,
		.mtl_ets_ctrl_offset = 0x1000,
		.mtl_txq_weight = 0x00008018,
		.mtl_txq_weight_offset = 0x1000,
		.mtl_send_slp_cred = 0x0000801c,
		.mtl_send_slp_cred_offset = 0x1000,
		.mtl_high_cred = 0x00008020,
		.mtl_high_cred_offset = 0x1000,
		.mtl_low_cred = 0x00008024,
		.mtl_low_cred_offset = 0x1000,
	},
};

static int ethqos_dll_configure(struct qcom_ethqos *ethqos)
{
	unsigned int val;
	int retry = 1000;

	/* Set CDR_EN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_CDR_EN,
		      SDCC_DLL_CONFIG_CDR_EN, SDCC_HC_REG_DLL_CONFIG);

	/* Set CDR_EXT_EN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_CDR_EXT_EN,
		      SDCC_DLL_CONFIG_CDR_EXT_EN, SDCC_HC_REG_DLL_CONFIG);

	/* Clear CK_OUT_EN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_CK_OUT_EN,
		      0, SDCC_HC_REG_DLL_CONFIG);

	/* Set DLL_EN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_DLL_EN,
		      SDCC_DLL_CONFIG_DLL_EN, SDCC_HC_REG_DLL_CONFIG);

	if (!ethqos->has_emac3) {
		rgmii_updatel(ethqos, SDCC_DLL_MCLK_GATING_EN,
			      0, SDCC_HC_REG_DLL_CONFIG);

		rgmii_updatel(ethqos, SDCC_DLL_CDR_FINE_PHASE,
			      0, SDCC_HC_REG_DLL_CONFIG);
	}

	/* Wait for CK_OUT_EN clear */
	do {
		val = rgmii_readl(ethqos, SDCC_HC_REG_DLL_CONFIG);
		val &= SDCC_DLL_CONFIG_CK_OUT_EN;
		if (!val)
			break;
		mdelay(1);
		retry--;
	} while (retry > 0);
	if (!retry)
		dev_err(&ethqos->pdev->dev, "Clear CK_OUT_EN timedout\n");

	/* Set CK_OUT_EN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_CK_OUT_EN,
		      SDCC_DLL_CONFIG_CK_OUT_EN, SDCC_HC_REG_DLL_CONFIG);

	/* Wait for CK_OUT_EN set */
	retry = 1000;
	do {
		val = rgmii_readl(ethqos, SDCC_HC_REG_DLL_CONFIG);
		val &= SDCC_DLL_CONFIG_CK_OUT_EN;
		if (val)
			break;
		mdelay(1);
		retry--;
	} while (retry > 0);
	if (!retry)
		dev_err(&ethqos->pdev->dev, "Set CK_OUT_EN timedout\n");

	/* Set DDR_CAL_EN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG2_DDR_CAL_EN,
		      SDCC_DLL_CONFIG2_DDR_CAL_EN, SDCC_HC_REG_DLL_CONFIG2);

	if (!ethqos->has_emac3) {
		rgmii_updatel(ethqos, SDCC_DLL_CONFIG2_DLL_CLOCK_DIS,
			      0, SDCC_HC_REG_DLL_CONFIG2);

		rgmii_updatel(ethqos, SDCC_DLL_CONFIG2_MCLK_FREQ_CALC,
			      0x1A << 10, SDCC_HC_REG_DLL_CONFIG2);

		rgmii_updatel(ethqos, SDCC_DLL_CONFIG2_DDR_TRAFFIC_INIT_SEL,
			      BIT(2), SDCC_HC_REG_DLL_CONFIG2);

		rgmii_updatel(ethqos, SDCC_DLL_CONFIG2_DDR_TRAFFIC_INIT_SW,
			      SDCC_DLL_CONFIG2_DDR_TRAFFIC_INIT_SW,
			      SDCC_HC_REG_DLL_CONFIG2);
	}

	return 0;
}

static int ethqos_rgmii_macro_init(struct qcom_ethqos *ethqos)
{
	int phase_shift;
	int phy_mode;
	int loopback;

	/* Determine if the PHY adds a 2 ns TX delay or the MAC handles it */
	phy_mode = device_get_phy_mode(&ethqos->pdev->dev);
	if (phy_mode == PHY_INTERFACE_MODE_RGMII_ID ||
	    phy_mode == PHY_INTERFACE_MODE_RGMII_TXID)
		phase_shift = 0;
	else
		phase_shift = RGMII_CONFIG2_TX_CLK_PHASE_SHIFT_EN;

	/* Disable loopback mode */
	rgmii_updatel(ethqos, RGMII_CONFIG2_TX_TO_RX_LOOPBACK_EN,
		      0, RGMII_IO_MACRO_CONFIG2);

	/* Determine if this platform wants loopback enabled after programming */
	if (ethqos->rgmii_config_loopback_en)
		loopback = RGMII_CONFIG_LOOPBACK_EN;
	else
		loopback = 0;

	/* Select RGMII, write 0 to interface select */
	rgmii_updatel(ethqos, RGMII_CONFIG_INTF_SEL,
		      0, RGMII_IO_MACRO_CONFIG);

	switch (ethqos->speed) {
	case SPEED_1000:
		rgmii_updatel(ethqos, RGMII_CONFIG_DDR_MODE,
			      RGMII_CONFIG_DDR_MODE, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_BYPASS_TX_ID_EN,
			      0, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_POS_NEG_DATA_SEL,
			      RGMII_CONFIG_POS_NEG_DATA_SEL,
			      RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_PROG_SWAP,
			      RGMII_CONFIG_PROG_SWAP, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG2_DATA_DIVIDE_CLK_SEL,
			      0, RGMII_IO_MACRO_CONFIG2);

		rgmii_updatel(ethqos, RGMII_CONFIG2_TX_CLK_PHASE_SHIFT_EN,
			      phase_shift, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG2_RSVD_CONFIG15,
			      0, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP,
			      RGMII_CONFIG2_RX_PROG_SWAP,
			      RGMII_IO_MACRO_CONFIG2);

		/* PRG_RCLK_DLY = TCXO period * TCXO_CYCLES_CNT / 2 * RX delay ns,
		 * in practice this becomes PRG_RCLK_DLY = 52 * 4 / 2 * RX delay ns
		 */
		if (ethqos->has_emac3) {
			/* 0.9 ns */
			rgmii_updatel(ethqos, SDCC_DDR_CONFIG_PRG_RCLK_DLY,
				      115, SDCC_HC_REG_DDR_CONFIG);
		} else {
			/* 1.8 ns */
			rgmii_updatel(ethqos, SDCC_DDR_CONFIG_PRG_RCLK_DLY,
				      57, SDCC_HC_REG_DDR_CONFIG);
		}
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_PRG_DLY_EN,
			      SDCC_DDR_CONFIG_PRG_DLY_EN,
			      SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_LOOPBACK_EN,
			      loopback, RGMII_IO_MACRO_CONFIG);
		break;

	case SPEED_100:
		rgmii_updatel(ethqos, RGMII_CONFIG_DDR_MODE,
			      RGMII_CONFIG_DDR_MODE, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_BYPASS_TX_ID_EN,
			      RGMII_CONFIG_BYPASS_TX_ID_EN,
			      RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_POS_NEG_DATA_SEL,
			      0, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_PROG_SWAP,
			      0, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG2_DATA_DIVIDE_CLK_SEL,
			      0, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG2_TX_CLK_PHASE_SHIFT_EN,
			      phase_shift, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG_MAX_SPD_PRG_2,
			      BIT(6), RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG2_RSVD_CONFIG15,
			      0, RGMII_IO_MACRO_CONFIG2);

		if (ethqos->has_emac3)
			rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP,
				      RGMII_CONFIG2_RX_PROG_SWAP,
				      RGMII_IO_MACRO_CONFIG2);
		else
			rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP,
				      0, RGMII_IO_MACRO_CONFIG2);

		/* Write 0x5 to PRG_RCLK_DLY_CODE */
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_CODE,
			      (BIT(29) | BIT(27)), SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY,
			      SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY,
			      SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_EN,
			      SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_EN,
			      SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_LOOPBACK_EN,
			      loopback, RGMII_IO_MACRO_CONFIG);
		break;

	case SPEED_10:
		rgmii_updatel(ethqos, RGMII_CONFIG_DDR_MODE,
			      RGMII_CONFIG_DDR_MODE, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_BYPASS_TX_ID_EN,
			      RGMII_CONFIG_BYPASS_TX_ID_EN,
			      RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_POS_NEG_DATA_SEL,
			      0, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_PROG_SWAP,
			      0, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG2_DATA_DIVIDE_CLK_SEL,
			      0, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG2_TX_CLK_PHASE_SHIFT_EN,
			      phase_shift, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG_MAX_SPD_PRG_9,
			      BIT(12) | GENMASK(9, 8),
			      RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG2_RSVD_CONFIG15,
			      0, RGMII_IO_MACRO_CONFIG2);
		if (ethqos->has_emac3)
			rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP,
				      RGMII_CONFIG2_RX_PROG_SWAP,
				      RGMII_IO_MACRO_CONFIG2);
		else
			rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP,
				      0, RGMII_IO_MACRO_CONFIG2);
		/* Write 0x5 to PRG_RCLK_DLY_CODE */
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_CODE,
			      (BIT(29) | BIT(27)), SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY,
			      SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY,
			      SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_EN,
			      SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_EN,
			      SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_LOOPBACK_EN,
			      loopback, RGMII_IO_MACRO_CONFIG);
		break;
	default:
		dev_err(&ethqos->pdev->dev,
			"Invalid speed %d\n", ethqos->speed);
		return -EINVAL;
	}

	return 0;
}

static int ethqos_configure(struct qcom_ethqos *ethqos)
{
	volatile unsigned int dll_lock;
	unsigned int i, retry = 1000;

	/* Reset to POR values and enable clk */
	for (i = 0; i < ethqos->num_por; i++)
		rgmii_writel(ethqos, ethqos->por[i].value,
			     ethqos->por[i].offset);
	ethqos_set_func_clk_en(ethqos);

	/* Initialize the DLL first */

	/* Set DLL_RST */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_DLL_RST,
		      SDCC_DLL_CONFIG_DLL_RST, SDCC_HC_REG_DLL_CONFIG);

	/* Set PDN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_PDN,
		      SDCC_DLL_CONFIG_PDN, SDCC_HC_REG_DLL_CONFIG);

	if (ethqos->has_emac3) {
		if (ethqos->speed == SPEED_1000) {
			rgmii_writel(ethqos, 0x1800000, SDCC_TEST_CTL);
			rgmii_writel(ethqos, 0x2C010800, SDCC_USR_CTL);
			rgmii_writel(ethqos, 0xA001, SDCC_HC_REG_DLL_CONFIG2);
		} else {
			rgmii_writel(ethqos, 0x40010800, SDCC_USR_CTL);
			rgmii_writel(ethqos, 0xA001, SDCC_HC_REG_DLL_CONFIG2);
		}
	}

	/* Clear DLL_RST */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_DLL_RST, 0,
		      SDCC_HC_REG_DLL_CONFIG);

	/* Clear PDN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_PDN, 0,
		      SDCC_HC_REG_DLL_CONFIG);

	if (ethqos->speed != SPEED_100 && ethqos->speed != SPEED_10) {
		/* Set DLL_EN */
		rgmii_updatel(ethqos, SDCC_DLL_CONFIG_DLL_EN,
			      SDCC_DLL_CONFIG_DLL_EN, SDCC_HC_REG_DLL_CONFIG);

		/* Set CK_OUT_EN */
		rgmii_updatel(ethqos, SDCC_DLL_CONFIG_CK_OUT_EN,
			      SDCC_DLL_CONFIG_CK_OUT_EN,
			      SDCC_HC_REG_DLL_CONFIG);

		/* Set USR_CTL bit 26 with mask of 3 bits */
		if (!ethqos->has_emac3)
			rgmii_updatel(ethqos, GENMASK(26, 24), BIT(26),
				      SDCC_USR_CTL);

		/* wait for DLL LOCK */
		do {
			mdelay(1);
			dll_lock = rgmii_readl(ethqos, SDC4_STATUS);
			if (dll_lock & SDC4_STATUS_DLL_LOCK)
				break;
			retry--;
		} while (retry > 0);
		if (!retry)
			dev_err(&ethqos->pdev->dev,
				"Timeout while waiting for DLL lock\n");
	}

	if (ethqos->speed == SPEED_1000)
		ethqos_dll_configure(ethqos);

	ethqos_rgmii_macro_init(ethqos);

	return 0;
}

int ethqos_configureSGMII(struct qcom_ethqos *ethqos)
{
	u32 value = 0;

	value = readl(ethqos->ioaddr + MAC_CTRL_REG);
	/* This looks to be updating
#define GMAC_CONFIG_PS			BIT(15)
#define GMAC_CONFIG_FES			BIT(14)
	* described as:
	* BIT 14 FES:
	*	SPEED
	*	This bit selects the speed mode.
	*	The mac_speed_o[0] signal reflects the value of this bit.
	*	0: M_10_1000M
	*	1: M_100_2500M
	*	BIT 15 PS:
	*	Port Select
	*	This bit selects the Ethernet line speed.
	*	This bit, along with Bit 14, selects the exact line speed. In the 10/100 Mbps-only
	*	(always 1) or 1000 Mbps-only (always 0) configurations, this bit is read-only (RO) with
	*	appropriate value. In default 10/100/1000 Mbps configurations, this bit is read-write
	*	(R/W). The mac_speed_o[1] signal reflects the value of this bit.
	*	0: M_1000_2500M
	*	1: M_10_100M
	*/
	switch (ethqos->speed) {
		case SPEED_1000:
			   value &= ~BIT(15);
				   writel(value, ethqos->ioaddr + MAC_CTRL_REG);
				   rgmii_updatel(ethqos, BIT(16), BIT(16), RGMII_IO_MACRO_CONFIG2);
		break;

		case SPEED_100:
				   value |= BIT(15) | BIT(14);
				   writel(value, ethqos->ioaddr + MAC_CTRL_REG);
		break;

		case SPEED_10:
				   value |= BIT(15);
				   value &= ~BIT(14);
				   writel(value, ethqos->ioaddr + MAC_CTRL_REG);
		break;
	}

	return value;
}

static void ethqos_fix_mac_speed(void *priv, unsigned int speed)
{
	struct qcom_ethqos *ethqos = priv;
	int phy_mode = device_get_phy_mode(&ethqos->pdev->dev);
	ethqos->speed = speed;

	if (phy_mode == PHY_INTERFACE_MODE_SGMII)
		ethqos_configureSGMII(ethqos);
	else {
		ethqos_update_rgmii_clk(ethqos, speed);
		ethqos_configure(ethqos);
	}
}

static int ethqos_clks_config(void *priv, bool enabled)
{
	struct qcom_ethqos *ethqos = priv;
	int ret = 0;

	if (enabled) {
		ret = clk_prepare_enable(ethqos->rgmii_clk);
		if (ret) {
			dev_err(&ethqos->pdev->dev, "rgmii_clk enable failed\n");
			return ret;
		}

		/* Enable functional clock to prevent DMA reset to timeout due
		 * to lacking PHY clock after the hardware block has been power
		 * cycled. The actual configuration will be adjusted once
		 * ethqos_fix_mac_speed() is invoked.
		 */
		ethqos_set_func_clk_en(ethqos);
	} else {
		clk_disable_unprepare(ethqos->rgmii_clk);
	}

	return ret;
}

static int qcom_ethqos_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	const struct ethqos_emac_driver_data *data;
	struct qcom_ethqos *ethqos;
	int ret;

	printk(KERN_ERR "%s: %d\n", __func__, __LINE__);
	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	printk(KERN_ERR "%s: %d\n", __func__, __LINE__);
	plat_dat = stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat)) {
		dev_err(&pdev->dev, "dt configuration failed\n");
		return PTR_ERR(plat_dat);
	}

	plat_dat->clks_config = ethqos_clks_config;

	ethqos = devm_kzalloc(&pdev->dev, sizeof(*ethqos), GFP_KERNEL);
	if (!ethqos) {
		ret = -ENOMEM;
		goto err_mem;
	}

	printk(KERN_ERR "%s: %d\n", __func__, __LINE__);
	ethqos->pdev = pdev;
	/* TODO: is this really necessary, and the function that uses it? Isn't
	 * that a core stmmac thing? */
	ethqos->ioaddr = (&stmmac_res)->addr;
	printk(KERN_ERR "%s: %d\n", __func__, __LINE__);
	ethqos->rgmii_base = devm_platform_ioremap_resource_byname(pdev, "rgmii");
	if (IS_ERR(ethqos->rgmii_base)) {
		ret = PTR_ERR(ethqos->rgmii_base);
		goto err_mem;
	}

	printk(KERN_ERR "%s: %d\n", __func__, __LINE__);
	data = of_device_get_match_data(&pdev->dev);
	ethqos->por = data->por;
	ethqos->num_por = data->num_por;
	ethqos->rgmii_config_loopback_en = data->rgmii_config_loopback_en;
	ethqos->has_emac3 = data->has_emac3;

	/* TODO: remove */
	ethqos_init_regulators(ethqos);

	printk(KERN_ERR "%s: %d\n", __func__, __LINE__);
	if (plat_dat->interface == PHY_INTERFACE_MODE_SGMII) {
		printk(KERN_ERR "%s: %d\n", __func__, __LINE__);
		ret = configure_serdes_dt(ethqos);
		if (ret)
			goto err_mem;
		qcom_ethqos_serdes_init(ethqos, ethqos->speed);
	} else {
		printk(KERN_ERR "%s: %d\n", __func__, __LINE__);
		ethqos->rgmii_clk = devm_clk_get(&pdev->dev, "rgmii");
		if (IS_ERR(ethqos->rgmii_clk)) {
			ret = PTR_ERR(ethqos->rgmii_clk);
			goto err_mem;
		}

		ret = ethqos_clks_config(ethqos, true);
		if (ret)
			goto err_mem;

		ethqos->speed = SPEED_1000;
		ethqos_update_rgmii_clk(ethqos, SPEED_1000);
	}

	ethqos_set_func_clk_en(ethqos);

	plat_dat->bsp_priv = ethqos;
	plat_dat->fix_mac_speed = ethqos_fix_mac_speed;
	plat_dat->dump_debug_regs = rgmii_dump;
	plat_dat->has_gmac4 = 1;
	plat_dat->dwmac4_addrs = &data->dwmac4_addrs;
	plat_dat->pmt = 1;
	plat_dat->tso_en = of_property_read_bool(np, "snps,tso");
	if (of_device_is_compatible(np, "qcom,qcs404-ethqos"))
		plat_dat->rx_clk_runs_in_lpi = 1;

	ret = stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
	if (ret)
		goto err_clk;

	return ret;

err_clk:
	ethqos_clks_config(ethqos, false);

err_mem:
	stmmac_remove_config_dt(pdev, plat_dat);

	return ret;
}

static int qcom_ethqos_remove(struct platform_device *pdev)
{
	struct qcom_ethqos *ethqos;
	int ret;

	ethqos = get_stmmac_bsp_priv(&pdev->dev);
	if (!ethqos)
		return -ENODEV;

	ret = stmmac_pltfr_remove(pdev);
	ethqos_clks_config(ethqos, false);

	return ret;
}

static const struct of_device_id qcom_ethqos_match[] = {
	{ .compatible = "qcom,qcs404-ethqos", .data = &emac_v2_3_0_data},
	{ .compatible = "qcom,sa8775p-ethqos", .data = &emac_v3_0_0_data},
	{ .compatible = "qcom,sc8280xp-ethqos", .data = &emac_v3_0_0_data},
	{ .compatible = "qcom,sm8150-ethqos", .data = &emac_v2_1_0_data},
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_ethqos_match);

static struct platform_driver qcom_ethqos_driver = {
	.probe  = qcom_ethqos_probe,
	.remove = qcom_ethqos_remove,
	.driver = {
		.name           = "qcom-ethqos",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = qcom_ethqos_match,
	},
};
module_platform_driver(qcom_ethqos_driver);

MODULE_DESCRIPTION("Qualcomm ETHQOS driver");
MODULE_LICENSE("GPL v2");
