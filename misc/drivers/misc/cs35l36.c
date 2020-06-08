/*
 * cs35l36.c  --  CS35L36 Misc driver
 *
 * Copyright 2020 Cirrus Logic, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/ioctl.h>
#include <linux/uaccess.h>
#include <linux/platform_data/cs35l36.h>

#include "cs35l36.h"

/*
 * Some fields take zero as a valid value so use a high bit flag that won't
 * get written to the device to mark those.
 */
#define CS35L36_VALID_PDATA 0x80000000

static const char * const cs35l36_supplies[] = {
	"VA",
	"VP",
};

struct  cs35l36_private {
	struct device *dev;
	struct cs35l36_platform_data pdata;
	struct regmap *regmap;
	struct regulator_bulk_data supplies[2];
	int num_supplies;
	int chip_version;
	int rev_id;
	struct gpio_desc *reset_gpio;
	struct mutex lock;
	struct miscdevice misc_dev;
};

static struct reg_default cs35l36_reg[] = {
	{CS35L36_TESTKEY_CTRL,			0x00000000},
	{CS35L36_USERKEY_CTL,			0x00000000},
	{CS35L36_OTP_CTRL1,			0x00002460},
	{CS35L36_OTP_CTRL2,			0x00000000},
	{CS35L36_OTP_CTRL3,			0x00000000},
	{CS35L36_OTP_CTRL4,			0x00000000},
	{CS35L36_OTP_CTRL5,			0x00000000},
	{CS35L36_PAC_CTL1,			0x00000004},
	{CS35L36_PAC_CTL2,			0x00000000},
	{CS35L36_PAC_CTL3,			0x00000000},
	{CS35L36_PWR_CTRL1,			0x00000000},
	{CS35L36_PWR_CTRL2,			0x00003321},
	{CS35L36_PWR_CTRL3,			0x01000010},
	{CS35L36_CTRL_OVRRIDE,			0x00000002},
	{CS35L36_AMP_OUT_MUTE,			0x00000000},
	{CS35L36_OTP_TRIM_STATUS,		0x00000000},
	{CS35L36_DISCH_FILT,			0x00000000},
	{CS35L36_PROTECT_REL_ERR,		0x00000000},
	{CS35L36_PAD_INTERFACE,			0x00000038},
	{CS35L36_PLL_CLK_CTRL,			0x00000010},
	{CS35L36_GLOBAL_CLK_CTRL,		0x00000003},
	{CS35L36_ADC_CLK_CTRL,			0x00000000},
	{CS35L36_SWIRE_CLK_CTRL,		0x00000000},
	{CS35L36_SP_SCLK_CLK_CTRL,		0x00000000},
	{CS35L36_MDSYNC_EN,			0x00000000},
	{CS35L36_MDSYNC_TX_ID,			0x00000000},
	{CS35L36_MDSYNC_PWR_CTRL,		0x00000000},
	{CS35L36_MDSYNC_DATA_TX,		0x00000000},
	{CS35L36_MDSYNC_TX_STATUS,		0x00000002},
	{CS35L36_MDSYNC_RX_STATUS,		0x00000000},
	{CS35L36_MDSYNC_ERR_STATUS,		0x00000000},
	{CS35L36_BSTCVRT_VCTRL1,		0x00000000},
	{CS35L36_BSTCVRT_VCTRL2,		0x00000001},
	{CS35L36_BSTCVRT_PEAK_CUR,		0x0000004A},
	{CS35L36_BSTCVRT_SFT_RAMP,		0x00000003},
	{CS35L36_BSTCVRT_COEFF,			0x00002424},
	{CS35L36_BSTCVRT_SLOPE_LBST,		0x00005800},
	{CS35L36_BSTCVRT_SW_FREQ,		0x00010000},
	{CS35L36_BSTCVRT_DCM_CTRL,		0x00002001},
	{CS35L36_BSTCVRT_DCM_MODE_FORCE,	0x00000000},
	{CS35L36_BSTCVRT_OVERVOLT_CTRL,		0x00000130},
	{CS35L36_VPI_LIMIT_MODE,		0x00000000},
	{CS35L36_VPI_LIMIT_MINMAX,		0x00003000},
	{CS35L36_VPI_VP_THLD,			0x00101010},
	{CS35L36_VPI_TRACK_CTRL,		0x00000000},
	{CS35L36_VPI_TRIG_MODE_CTRL,		0x00000000},
	{CS35L36_VPI_TRIG_STEPS,		0x00000000},
	{CS35L36_VI_SPKMON_FILT,		0x00000003},
	{CS35L36_VI_SPKMON_GAIN,		0x00000909},
	{CS35L36_VI_SPKMON_IP_SEL,		0x00000000},
	{CS35L36_DTEMP_WARN_THLD,		0x00000002},
	{CS35L36_DTEMP_STATUS,			0x00000000},
	{CS35L36_VPVBST_FS_SEL,			0x00000001},
	{CS35L36_VPVBST_VP_CTRL,		0x000001C0},
	{CS35L36_VPVBST_VBST_CTRL,		0x000001C0},
	{CS35L36_ASP_TX_PIN_CTRL,		0x00000028},
	{CS35L36_ASP_RATE_CTRL,			0x00090000},
	{CS35L36_ASP_FORMAT,			0x00000002},
	{CS35L36_ASP_FRAME_CTRL,		0x00180018},
	{CS35L36_ASP_TX1_TX2_SLOT,		0x00010000},
	{CS35L36_ASP_TX3_TX4_SLOT,		0x00030002},
	{CS35L36_ASP_TX5_TX6_SLOT,		0x00050004},
	{CS35L36_ASP_TX7_TX8_SLOT,		0x00070006},
	{CS35L36_ASP_RX1_SLOT,			0x00000000},
	{CS35L36_ASP_RX_TX_EN,			0x00000000},
	{CS35L36_ASP_RX1_SEL,			0x00000008},
	{CS35L36_ASP_TX1_SEL,			0x00000018},
	{CS35L36_ASP_TX2_SEL,			0x00000019},
	{CS35L36_ASP_TX3_SEL,			0x00000028},
	{CS35L36_ASP_TX4_SEL,			0x00000029},
	{CS35L36_ASP_TX5_SEL,			0x00000020},
	{CS35L36_ASP_TX6_SEL,			0x00000000},
	{CS35L36_SWIRE_P1_TX1_SEL,		0x00000018},
	{CS35L36_SWIRE_P1_TX2_SEL,		0x00000019},
	{CS35L36_SWIRE_P2_TX1_SEL,		0x00000028},
	{CS35L36_SWIRE_P2_TX2_SEL,		0x00000029},
	{CS35L36_SWIRE_P2_TX3_SEL,		0x00000020},
	{CS35L36_SWIRE_DP1_FIFO_CFG,		0x0000001B},
	{CS35L36_SWIRE_DP2_FIFO_CFG,		0x0000001B},
	{CS35L36_SWIRE_DP3_FIFO_CFG,		0x0000001B},
	{CS35L36_SWIRE_PCM_RX_DATA,		0x00000000},
	{CS35L36_SWIRE_FS_SEL,			0x00000001},
	{CS35L36_AMP_DIG_VOL_CTRL,		0x00008000},
	{CS35L36_VPBR_CFG,			0x02AA1905},
	{CS35L36_VBBR_CFG,			0x02AA1905},
	{CS35L36_VPBR_STATUS,			0x00000000},
	{CS35L36_VBBR_STATUS,			0x00000000},
	{CS35L36_OVERTEMP_CFG,			0x00000001},
	{CS35L36_AMP_ERR_VOL,			0x00000000},
	{CS35L36_CLASSH_CFG,			0x000B0405},
	{CS35L36_CLASSH_FET_DRV_CFG,		0x00000111},
	{CS35L36_NG_CFG,			0x00000033},
	{CS35L36_AMP_GAIN_CTRL,			0x00000273},
	{CS35L36_PWM_MOD_IO_CTRL,		0x00000000},
	{CS35L36_PWM_MOD_STATUS,		0x00000000},
	{CS35L36_DAC_MSM_CFG,			0x00000000},
	{CS35L36_AMP_SLOPE_CTRL,		0x00000B00},
	{CS35L36_AMP_PDM_VOLUME,		0x00000000},
	{CS35L36_AMP_PDM_RATE_CTRL,		0x00000000},
	{CS35L36_PDM_CH_SEL,			0x00000000},
	{CS35L36_AMP_NG_CTRL,			0x0000212F},
	{CS35L36_PDM_HIGHFILT_CTRL,		0x00000000},
	{CS35L36_PAC_INT0_CTRL,			0x00000001},
	{CS35L36_PAC_INT1_CTRL,			0x00000001},
	{CS35L36_PAC_INT2_CTRL,			0x00000001},
	{CS35L36_PAC_INT3_CTRL,			0x00000001},
	{CS35L36_PAC_INT4_CTRL,			0x00000001},
	{CS35L36_PAC_INT5_CTRL,			0x00000001},
	{CS35L36_PAC_INT6_CTRL,			0x00000001},
	{CS35L36_PAC_INT7_CTRL,			0x00000001},
};

static bool cs35l36_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS35L36_SW_RESET:
	case CS35L36_SW_REV:
	case CS35L36_HW_REV:
	case CS35L36_TESTKEY_CTRL:
	case CS35L36_USERKEY_CTL:
	case CS35L36_OTP_MEM30:
	case CS35L36_OTP_CTRL1:
	case CS35L36_OTP_CTRL2:
	case CS35L36_OTP_CTRL3:
	case CS35L36_OTP_CTRL4:
	case CS35L36_OTP_CTRL5:
	case CS35L36_PAC_CTL1:
	case CS35L36_PAC_CTL2:
	case CS35L36_PAC_CTL3:
	case CS35L36_DEVICE_ID:
	case CS35L36_FAB_ID:
	case CS35L36_REV_ID:
	case CS35L36_PWR_CTRL1:
	case CS35L36_PWR_CTRL2:
	case CS35L36_PWR_CTRL3:
	case CS35L36_CTRL_OVRRIDE:
	case CS35L36_AMP_OUT_MUTE:
	case CS35L36_OTP_TRIM_STATUS:
	case CS35L36_DISCH_FILT:
	case CS35L36_PROTECT_REL_ERR:
	case CS35L36_PAD_INTERFACE:
	case CS35L36_PLL_CLK_CTRL:
	case CS35L36_GLOBAL_CLK_CTRL:
	case CS35L36_ADC_CLK_CTRL:
	case CS35L36_SWIRE_CLK_CTRL:
	case CS35L36_SP_SCLK_CLK_CTRL:
	case CS35L36_TST_FS_MON0:
	case CS35L36_MDSYNC_EN:
	case CS35L36_MDSYNC_TX_ID:
	case CS35L36_MDSYNC_PWR_CTRL:
	case CS35L36_MDSYNC_DATA_TX:
	case CS35L36_MDSYNC_TX_STATUS:
	case CS35L36_MDSYNC_RX_STATUS:
	case CS35L36_MDSYNC_ERR_STATUS:
	case CS35L36_BSTCVRT_VCTRL1:
	case CS35L36_BSTCVRT_VCTRL2:
	case CS35L36_BSTCVRT_PEAK_CUR:
	case CS35L36_BSTCVRT_SFT_RAMP:
	case CS35L36_BSTCVRT_COEFF:
	case CS35L36_BSTCVRT_SLOPE_LBST:
	case CS35L36_BSTCVRT_SW_FREQ:
	case CS35L36_BSTCVRT_DCM_CTRL:
	case CS35L36_BSTCVRT_DCM_MODE_FORCE:
	case CS35L36_BSTCVRT_OVERVOLT_CTRL:
	case CS35L36_BST_TST_MANUAL:
	case CS35L36_BST_ANA2_TEST:
	case CS35L36_VPI_LIMIT_MODE:
	case CS35L36_VPI_LIMIT_MINMAX:
	case CS35L36_VPI_VP_THLD:
	case CS35L36_VPI_TRACK_CTRL:
	case CS35L36_VPI_TRIG_MODE_CTRL:
	case CS35L36_VPI_TRIG_STEPS:
	case CS35L36_VI_SPKMON_FILT:
	case CS35L36_VI_SPKMON_GAIN:
	case CS35L36_VI_SPKMON_IP_SEL:
	case CS35L36_DTEMP_WARN_THLD:
	case CS35L36_DTEMP_STATUS:
	case CS35L36_VPVBST_FS_SEL:
	case CS35L36_VPVBST_VP_CTRL:
	case CS35L36_VPVBST_VBST_CTRL:
	case CS35L36_ASP_TX_PIN_CTRL:
	case CS35L36_ASP_RATE_CTRL:
	case CS35L36_ASP_FORMAT:
	case CS35L36_ASP_FRAME_CTRL:
	case CS35L36_ASP_TX1_TX2_SLOT:
	case CS35L36_ASP_TX3_TX4_SLOT:
	case CS35L36_ASP_TX5_TX6_SLOT:
	case CS35L36_ASP_TX7_TX8_SLOT:
	case CS35L36_ASP_RX1_SLOT:
	case CS35L36_ASP_RX_TX_EN:
	case CS35L36_ASP_RX1_SEL:
	case CS35L36_ASP_TX1_SEL:
	case CS35L36_ASP_TX2_SEL:
	case CS35L36_ASP_TX3_SEL:
	case CS35L36_ASP_TX4_SEL:
	case CS35L36_ASP_TX5_SEL:
	case CS35L36_ASP_TX6_SEL:
	case CS35L36_SWIRE_P1_TX1_SEL:
	case CS35L36_SWIRE_P1_TX2_SEL:
	case CS35L36_SWIRE_P2_TX1_SEL:
	case CS35L36_SWIRE_P2_TX2_SEL:
	case CS35L36_SWIRE_P2_TX3_SEL:
	case CS35L36_SWIRE_DP1_FIFO_CFG:
	case CS35L36_SWIRE_DP2_FIFO_CFG:
	case CS35L36_SWIRE_DP3_FIFO_CFG:
	case CS35L36_SWIRE_PCM_RX_DATA:
	case CS35L36_SWIRE_FS_SEL:
	case CS35L36_AMP_DIG_VOL_CTRL:
	case CS35L36_VPBR_CFG:
	case CS35L36_VBBR_CFG:
	case CS35L36_VPBR_STATUS:
	case CS35L36_VBBR_STATUS:
	case CS35L36_OVERTEMP_CFG:
	case CS35L36_AMP_ERR_VOL:
	case CS35L36_CLASSH_CFG:
	case CS35L36_CLASSH_FET_DRV_CFG:
	case CS35L36_NG_CFG:
	case CS35L36_AMP_GAIN_CTRL:
	case CS35L36_PWM_MOD_IO_CTRL:
	case CS35L36_PWM_MOD_STATUS:
	case CS35L36_DAC_MSM_CFG:
	case CS35L36_AMP_SLOPE_CTRL:
	case CS35L36_AMP_PDM_VOLUME:
	case CS35L36_AMP_PDM_RATE_CTRL:
	case CS35L36_PDM_CH_SEL:
	case CS35L36_AMP_NG_CTRL:
	case CS35L36_PDM_HIGHFILT_CTRL:
	case CS35L36_INT1_STATUS:
	case CS35L36_INT2_STATUS:
	case CS35L36_INT3_STATUS:
	case CS35L36_INT4_STATUS:
	case CS35L36_INT1_RAW_STATUS:
	case CS35L36_INT2_RAW_STATUS:
	case CS35L36_INT3_RAW_STATUS:
	case CS35L36_INT4_RAW_STATUS:
	case CS35L36_INT1_MASK:
	case CS35L36_INT2_MASK:
	case CS35L36_INT3_MASK:
	case CS35L36_INT4_MASK:
	case CS35L36_INT1_EDGE_LVL_CTRL:
	case CS35L36_INT3_EDGE_LVL_CTRL:
	case CS35L36_PAC_INT_STATUS:
	case CS35L36_PAC_INT_RAW_STATUS:
	case CS35L36_PAC_INT_FLUSH_CTRL:
	case CS35L36_PAC_INT0_CTRL:
	case CS35L36_PAC_INT1_CTRL:
	case CS35L36_PAC_INT2_CTRL:
	case CS35L36_PAC_INT3_CTRL:
	case CS35L36_PAC_INT4_CTRL:
	case CS35L36_PAC_INT5_CTRL:
	case CS35L36_PAC_INT6_CTRL:
	case CS35L36_PAC_INT7_CTRL:
		return true;
	default:
		if (reg >= CS35L36_PAC_PMEM_WORD0 &&
			reg <= CS35L36_PAC_PMEM_WORD1023)
			return true;
		else
			return false;
	}
}

static bool cs35l36_precious_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS35L36_TESTKEY_CTRL:
	case CS35L36_USERKEY_CTL:
	case CS35L36_TST_FS_MON0:
		return true;
	default:
		return false;
	}
}

static bool cs35l36_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS35L36_SW_RESET:
	case CS35L36_SW_REV:
	case CS35L36_HW_REV:
	case CS35L36_TESTKEY_CTRL:
	case CS35L36_USERKEY_CTL:
	case CS35L36_DEVICE_ID:
	case CS35L36_FAB_ID:
	case CS35L36_REV_ID:
	case CS35L36_INT1_STATUS:
	case CS35L36_INT2_STATUS:
	case CS35L36_INT3_STATUS:
	case CS35L36_INT4_STATUS:
	case CS35L36_INT1_RAW_STATUS:
	case CS35L36_INT2_RAW_STATUS:
	case CS35L36_INT3_RAW_STATUS:
	case CS35L36_INT4_RAW_STATUS:
	case CS35L36_INT1_MASK:
	case CS35L36_INT2_MASK:
	case CS35L36_INT3_MASK:
	case CS35L36_INT4_MASK:
	case CS35L36_INT1_EDGE_LVL_CTRL:
	case CS35L36_INT3_EDGE_LVL_CTRL:
	case CS35L36_PAC_INT_STATUS:
	case CS35L36_PAC_INT_RAW_STATUS:
	case CS35L36_PAC_INT_FLUSH_CTRL:
		return true;
	default:
		if (reg >= CS35L36_PAC_PMEM_WORD0 &&
			reg <= CS35L36_PAC_PMEM_WORD1023)
			return true;
		else
			return false;
	}
}

static const struct reg_sequence cs35l36_pup_patch[] = {
	{CS35L36_TESTKEY_CTRL, 0x00005555},
	{CS35L36_TESTKEY_CTRL, 0x0000AAAA},
	{0x00007850, 0x00002FA9},
	{0x00007854, 0x0003F1D5},
	{0x00007858, 0x0003F5E3},
	{0x0000785C, 0x00001137},
	{0x00007860, 0x0001A7A5},
	{0x00007864, 0x0002F16A},
	{0x00007868, 0x00003E21},
	{0x00007848, 0x00000001},
	{CS35L36_TESTKEY_CTRL, 0x0000CCCC},
	{CS35L36_TESTKEY_CTRL, 0x00003333},
};

static const struct reg_sequence cs35l36_spk_power_on_patch[] = {
	{CS35L36_AMP_GAIN_CTRL, 0x00000233},
	{CS35L36_ASP_TX1_TX2_SLOT, 0x00000002},
	{CS35L36_ASP_RX1_SLOT, 0x00000000},
	{CS35L36_ASP_RX_TX_EN, 0x00010003},
	{CS35L36_PWR_CTRL2, 0x00003721},
};

static int cs35l36_spk_power_on(struct cs35l36_private *cs35l36)
{
	regmap_multi_reg_write_bypassed(cs35l36->regmap, cs35l36_pup_patch,
					ARRAY_SIZE(cs35l36_pup_patch));

	regmap_multi_reg_write(cs35l36->regmap, cs35l36_spk_power_on_patch,
			       ARRAY_SIZE(cs35l36_spk_power_on_patch));

	regmap_update_bits(cs35l36->regmap, CS35L36_PWR_CTRL1,
			   CS35L36_GLOBAL_EN_MASK,
			   1 << CS35L36_GLOBAL_EN_SHIFT);

	usleep_range(1000, 1100);

	return 0;
}

static int cs35l36_spk_power_off(struct cs35l36_private *cs35l36)
{
	regmap_update_bits(cs35l36->regmap, CS35L36_PWR_CTRL1,
			   CS35L36_GLOBAL_EN_MASK,
			   0 << CS35L36_GLOBAL_EN_SHIFT);

	usleep_range(1000, 1100);

	regmap_update_bits(cs35l36->regmap, CS35L36_PWR_CTRL2, 0x01, 0);

	return 0;
}

static int cs35l36_boost_inductor(struct cs35l36_private *cs35l36, int inductor)
{
	regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_COEFF,
			   CS35L36_BSTCVRT_K1_MASK, 0x3C);
	regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_COEFF,
			   CS35L36_BSTCVRT_K2_MASK,
			   0x3C << CS35L36_BSTCVRT_K2_SHIFT);
	regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_SW_FREQ,
			   CS35L36_BSTCVRT_CCMFREQ_MASK, 0x00);

	switch (inductor) {
	case 1000: /* 1 uH */
		regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_SLOPE_LBST,
				   CS35L36_BSTCVRT_SLOPE_MASK,
				   0x75 << CS35L36_BSTCVRT_SLOPE_SHIFT);
		regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_SLOPE_LBST,
				   CS35L36_BSTCVRT_LBSTVAL_MASK, 0x00);
		break;
	case 1200: /* 1.2 uH */
		regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_SLOPE_LBST,
				   CS35L36_BSTCVRT_SLOPE_MASK,
				   0x6B << CS35L36_BSTCVRT_SLOPE_SHIFT);
		regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_SLOPE_LBST,
				   CS35L36_BSTCVRT_LBSTVAL_MASK, 0x01);
		break;
	default:
		dev_err(cs35l36->dev, "%s Invalid Inductor Value %d uH\n",
			__func__, inductor);
		return -EINVAL;
	}

	return 0;
}

static int cs35l36_probe(struct cs35l36_private *cs35l36)
{
	int ret = 0;

	if ((cs35l36->rev_id == CS35L36_REV_A0) && cs35l36->pdata.dcm_mode) {
		regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_DCM_CTRL,
				   CS35L36_DCM_AUTO_MASK,
				   CS35L36_DCM_AUTO_MASK);

		regmap_write(cs35l36->regmap, CS35L36_TESTKEY_CTRL,
			     CS35L36_TEST_UNLOCK1);
		regmap_write(cs35l36->regmap, CS35L36_TESTKEY_CTRL,
			     CS35L36_TEST_UNLOCK2);

		regmap_update_bits(cs35l36->regmap, CS35L36_BST_TST_MANUAL,
				   CS35L36_BST_MAN_IPKCOMP_MASK,
				   0 << CS35L36_BST_MAN_IPKCOMP_SHIFT);
		regmap_update_bits(cs35l36->regmap, CS35L36_BST_TST_MANUAL,
				   CS35L36_BST_MAN_IPKCOMP_EN_MASK,
				   CS35L36_BST_MAN_IPKCOMP_EN_MASK);

		regmap_write(cs35l36->regmap, CS35L36_TESTKEY_CTRL,
				CS35L36_TEST_LOCK1);
		regmap_write(cs35l36->regmap, CS35L36_TESTKEY_CTRL,
				CS35L36_TEST_LOCK2);
	}

	if (cs35l36->pdata.amp_pcm_inv)
		regmap_update_bits(cs35l36->regmap, CS35L36_AMP_DIG_VOL_CTRL,
				   CS35L36_AMP_PCM_INV_MASK,
				   CS35L36_AMP_PCM_INV_MASK);

	if (cs35l36->pdata.multi_amp_mode)
		regmap_update_bits(cs35l36->regmap, CS35L36_ASP_TX_PIN_CTRL,
				   CS35L36_ASP_TX_HIZ_MASK,
				   CS35L36_ASP_TX_HIZ_MASK);

	if (cs35l36->pdata.imon_pol_inv)
		regmap_update_bits(cs35l36->regmap, CS35L36_VI_SPKMON_FILT,
				   CS35L36_IMON_POL_MASK, 0);

	if (cs35l36->pdata.vmon_pol_inv)
		regmap_update_bits(cs35l36->regmap, CS35L36_VI_SPKMON_FILT,
				   CS35L36_VMON_POL_MASK, 0);

	if (cs35l36->pdata.bst_vctl)
		regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_VCTRL1,
				   CS35L35_BSTCVRT_CTL_MASK,
				   cs35l36->pdata.bst_vctl);

	if (cs35l36->pdata.bst_vctl_sel)
		regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_VCTRL2,
				   CS35L35_BSTCVRT_CTL_SEL_MASK,
				   cs35l36->pdata.bst_vctl_sel);

	if (cs35l36->pdata.bst_ipk)
		regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_PEAK_CUR,
				   CS35L36_BST_IPK_MASK,
				   cs35l36->pdata.bst_ipk);

	if (cs35l36->pdata.boost_ind) {
		ret = cs35l36_boost_inductor(cs35l36, cs35l36->pdata.boost_ind);
		if (ret < 0) {
			dev_err(cs35l36->dev,
				"Boost inductor config failed(%d)\n", ret);
			return ret;
		}
	}

	if (cs35l36->pdata.temp_warn_thld)
		regmap_update_bits(cs35l36->regmap, CS35L36_DTEMP_WARN_THLD,
				   CS35L36_TEMP_THLD_MASK,
				   cs35l36->pdata.temp_warn_thld);

	if (cs35l36->pdata.irq_drv_sel)
		regmap_update_bits(cs35l36->regmap, CS35L36_PAD_INTERFACE,
				   CS35L36_INT_DRV_SEL_MASK,
				   cs35l36->pdata.irq_drv_sel <<
				   CS35L36_INT_DRV_SEL_SHIFT);

	if (cs35l36->pdata.irq_gpio_sel)
		regmap_update_bits(cs35l36->regmap, CS35L36_PAD_INTERFACE,
				   CS35L36_INT_GPIO_SEL_MASK,
				   cs35l36->pdata.irq_gpio_sel <<
				   CS35L36_INT_GPIO_SEL_SHIFT);

	/*
	 * Rev B0 has 2 versions
	 * L36 is 10V
	 * L37 is 12V
	 * If L36 we need to clamp some values for safety
	 * after probe has setup dt values. We want to make
	 * sure we dont miss any values set in probe
	 */
	if (cs35l36->chip_version == CS35L36_10V_L36) {
		regmap_update_bits(cs35l36->regmap,
				   CS35L36_BSTCVRT_OVERVOLT_CTRL,
				   CS35L36_BST_OVP_THLD_MASK,
				   CS35L36_BST_OVP_THLD_11V);

		regmap_write(cs35l36->regmap, CS35L36_TESTKEY_CTRL,
			     CS35L36_TEST_UNLOCK1);
		regmap_write(cs35l36->regmap, CS35L36_TESTKEY_CTRL,
			     CS35L36_TEST_UNLOCK2);

		regmap_update_bits(cs35l36->regmap, CS35L36_BST_ANA2_TEST,
				   CS35L36_BST_OVP_TRIM_MASK,
				   CS35L36_BST_OVP_TRIM_11V <<
				   CS35L36_BST_OVP_TRIM_SHIFT);
		regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_VCTRL2,
				   CS35L36_BST_CTRL_LIM_MASK,
				   1 << CS35L36_BST_CTRL_LIM_SHIFT);
		regmap_update_bits(cs35l36->regmap, CS35L36_BSTCVRT_VCTRL1,
				   CS35L35_BSTCVRT_CTL_MASK,
				   CS35L36_BST_CTRL_10V_CLAMP);
		regmap_write(cs35l36->regmap, CS35L36_TESTKEY_CTRL,
			     CS35L36_TEST_LOCK1);
		regmap_write(cs35l36->regmap, CS35L36_TESTKEY_CTRL,
			     CS35L36_TEST_LOCK2);
	}

	/*
	 * RevA and B require the disabling of
	 * SYNC_GLOBAL_OVR when GLOBAL_EN = 0.
	 * Just turn it off from default
	 */
	regmap_update_bits(cs35l36->regmap, CS35L36_CTRL_OVRRIDE,
			   CS35L36_SYNC_GLOBAL_OVR_MASK,
			   0 << CS35L36_SYNC_GLOBAL_OVR_SHIFT);

	// Apply ASP Config
	if (cs35l36->pdata.asp_config.asp_fmt)
		regmap_update_bits(cs35l36->regmap, CS35L36_ASP_FORMAT,
				   CS35L36_ASP_FMT_MASK,
				   cs35l36->pdata.asp_config.asp_fmt << CS35L36_ASP_FMT_SHIFT);

	if (cs35l36->pdata.asp_config.asp_rx_width)
		regmap_update_bits(cs35l36->regmap, CS35L36_ASP_FRAME_CTRL,
				   CS35L36_ASP_RX_WIDTH_MASK,
				   cs35l36->pdata.asp_config.asp_rx_width <<
				   CS35L36_ASP_RX_WIDTH_SHIFT);

	if (cs35l36->pdata.asp_config.asp_tx_width)
		regmap_update_bits(cs35l36->regmap, CS35L36_ASP_FRAME_CTRL,
				   CS35L36_ASP_TX_WIDTH_MASK,
				   cs35l36->pdata.asp_config.asp_tx_width <<
				   CS35L36_ASP_TX_WIDTH_SHIFT);

	if (cs35l36->pdata.asp_config.asp_sample_rate)
		regmap_update_bits(cs35l36->regmap, CS35L36_GLOBAL_CLK_CTRL,
				   CS35L36_GLOBAL_FS_MASK,
				   cs35l36->pdata.asp_config.asp_sample_rate <<
				   CS35L36_GLOBAL_FS_SHIFT);

	if (cs35l36->pdata.asp_config.asp_sclk_rate)
		regmap_update_bits(cs35l36->regmap, CS35L36_ASP_TX_PIN_CTRL,
				   CS35L36_SCLK_FREQ_MASK,
				   cs35l36->pdata.asp_config.asp_sclk_rate);

	// Apply PLL Config
	if ((cs35l36->pdata.pll_refclk_freq & CS35L36_VALID_PDATA) |
	    (cs35l36->pdata.pll_refclk_sel & CS35L36_VALID_PDATA)) {
		regmap_update_bits(cs35l36->regmap, CS35L36_PLL_CLK_CTRL,
				   CS35L36_PLL_OPENLOOP_MASK,
				   1 << CS35L36_PLL_OPENLOOP_SHIFT);

		if (cs35l36->pdata.pll_refclk_freq & CS35L36_VALID_PDATA)
			regmap_update_bits(cs35l36->regmap, CS35L36_PLL_CLK_CTRL,
				   CS35L36_REFCLK_FREQ_MASK,
				   cs35l36->pdata.pll_refclk_freq << CS35L36_REFCLK_FREQ_SHIFT);

		regmap_update_bits(cs35l36->regmap, CS35L36_PLL_CLK_CTRL,
				   CS35L36_PLL_REFCLK_EN_MASK,
				   0 << CS35L36_PLL_REFCLK_EN_SHIFT);
		if (cs35l36->pdata.pll_refclk_freq & CS35L36_VALID_PDATA)
			regmap_update_bits(cs35l36->regmap, CS35L36_PLL_CLK_CTRL,
				   CS35L36_PLL_CLK_SEL_MASK,
				   cs35l36->pdata.pll_refclk_sel);

		regmap_update_bits(cs35l36->regmap, CS35L36_PLL_CLK_CTRL,
				  CS35L36_PLL_OPENLOOP_MASK,
				   0 << CS35L36_PLL_OPENLOOP_SHIFT);
		regmap_update_bits(cs35l36->regmap, CS35L36_PLL_CLK_CTRL,
				   CS35L36_PLL_REFCLK_EN_MASK,
				   1 << CS35L36_PLL_REFCLK_EN_SHIFT);
	}
	return 0;
}

static struct regmap_config cs35l36_regmap = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = CS35L36_PAC_PMEM_WORD1023,
	.reg_defaults = cs35l36_reg,
	.num_reg_defaults = ARRAY_SIZE(cs35l36_reg),
	.precious_reg = cs35l36_precious_reg,
	.volatile_reg = cs35l36_volatile_reg,
	.readable_reg = cs35l36_readable_reg,
	.cache_type = REGCACHE_RBTREE,
};

static irqreturn_t cs35l36_irq(int irq, void *data)
{
	struct cs35l36_private *cs35l36 = data;
	unsigned int status[4];
	unsigned int masks[4];
	int ret = IRQ_NONE;

	/* ack the irq by reading all status registers */
	regmap_bulk_read(cs35l36->regmap, CS35L36_INT1_STATUS, status,
			 ARRAY_SIZE(status));

	regmap_bulk_read(cs35l36->regmap, CS35L36_INT1_MASK, masks,
			 ARRAY_SIZE(masks));

	/* Check to see if unmasked bits are active */
	if (!(status[0] & ~masks[0]) && !(status[1] & ~masks[1]) &&
		!(status[2] & ~masks[2]) && !(status[3] & ~masks[3])) {
		return IRQ_NONE;
	}

	/*
	 * The following interrupts require a
	 * protection release cycle to get the
	 * speaker out of Safe-Mode.
	 */
	if (status[2] & CS35L36_AMP_SHORT_ERR) {
		dev_crit(cs35l36->dev, "Amp short error\n");
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_AMP_SHORT_ERR_RLS, 0);
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_AMP_SHORT_ERR_RLS,
				   CS35L36_AMP_SHORT_ERR_RLS);
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_AMP_SHORT_ERR_RLS, 0);
		regmap_update_bits(cs35l36->regmap, CS35L36_INT3_STATUS,
				   CS35L36_AMP_SHORT_ERR,
				   CS35L36_AMP_SHORT_ERR);
		ret = IRQ_HANDLED;
	}

	if (status[0] & CS35L36_TEMP_WARN) {
		dev_crit(cs35l36->dev, "Over temperature warning\n");
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_TEMP_WARN_ERR_RLS, 0);
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_TEMP_WARN_ERR_RLS,
				   CS35L36_TEMP_WARN_ERR_RLS);
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_TEMP_WARN_ERR_RLS, 0);
		regmap_update_bits(cs35l36->regmap, CS35L36_INT1_STATUS,
				   CS35L36_TEMP_WARN, CS35L36_TEMP_WARN);
		ret = IRQ_HANDLED;
	}

	if (status[0] & CS35L36_TEMP_ERR) {
		dev_crit(cs35l36->dev, "Over temperature error\n");
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_TEMP_ERR_RLS, 0);
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_TEMP_ERR_RLS, CS35L36_TEMP_ERR_RLS);
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_TEMP_ERR_RLS, 0);
		regmap_update_bits(cs35l36->regmap, CS35L36_INT1_STATUS,
				   CS35L36_TEMP_ERR, CS35L36_TEMP_ERR);
		ret = IRQ_HANDLED;
	}

	if (status[0] & CS35L36_BST_OVP_ERR) {
		dev_crit(cs35l36->dev, "VBST Over Voltage error\n");
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_TEMP_ERR_RLS, 0);
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_TEMP_ERR_RLS, CS35L36_TEMP_ERR_RLS);
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_TEMP_ERR_RLS, 0);
		regmap_update_bits(cs35l36->regmap, CS35L36_INT1_STATUS,
				   CS35L36_BST_OVP_ERR, CS35L36_BST_OVP_ERR);
		ret = IRQ_HANDLED;
	}

	if (status[0] & CS35L36_BST_DCM_UVP_ERR) {
		dev_crit(cs35l36->dev, "DCM VBST Under Voltage Error\n");
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_BST_UVP_ERR_RLS, 0);
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_BST_UVP_ERR_RLS,
				   CS35L36_BST_UVP_ERR_RLS);
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_BST_UVP_ERR_RLS, 0);
		regmap_update_bits(cs35l36->regmap, CS35L36_INT1_STATUS,
				   CS35L36_BST_DCM_UVP_ERR,
				   CS35L36_BST_DCM_UVP_ERR);
		ret = IRQ_HANDLED;
	}

	if (status[0] & CS35L36_BST_SHORT_ERR) {
		dev_crit(cs35l36->dev, "LBST SHORT error!\n");
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_BST_SHORT_ERR_RLS, 0);
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_BST_SHORT_ERR_RLS,
				   CS35L36_BST_SHORT_ERR_RLS);
		regmap_update_bits(cs35l36->regmap, CS35L36_PROTECT_REL_ERR,
				   CS35L36_BST_SHORT_ERR_RLS, 0);
		regmap_update_bits(cs35l36->regmap, CS35L36_INT1_STATUS,
				   CS35L36_BST_SHORT_ERR,
				   CS35L36_BST_SHORT_ERR);
		ret = IRQ_HANDLED;
	}

	return ret;
}

static int cs35l36_handle_of_data(struct i2c_client *i2c_client,
				struct cs35l36_platform_data *pdata)
{
	struct device_node *np = i2c_client->dev.of_node;
	struct cs35l36_vpbr_cfg *vpbr_config = &pdata->vpbr_config;
	struct asp_cfg *asp_config = &pdata->asp_config;
	struct device_node *vpbr_node, *asp_node;
	unsigned int val;
	int ret;

	if (!np)
		return 0;

	ret = of_property_read_u32(np, "cirrus,boost-ctl-millivolt", &val);
	if (!ret) {
		if (val < 2550 || val > 12000) {
			dev_err(&i2c_client->dev,
				"Invalid Boost Voltage %d mV\n", val);
			return -EINVAL;
		}
		pdata->bst_vctl = (((val - 2550) / 100) + 1) << 1;
	} else {
		dev_err(&i2c_client->dev,
			"Unable to find required parameter 'cirrus,boost-ctl-millivolt'");
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "cirrus,boost-ctl-select", &val);
	if (!ret)
		pdata->bst_vctl_sel = val | CS35L36_VALID_PDATA;

	ret = of_property_read_u32(np, "cirrus,boost-peak-milliamp", &val);
	if (!ret) {
		if (val < 1600 || val > 4500) {
			dev_err(&i2c_client->dev,
				"Invalid Boost Peak Current %u mA\n", val);
			return -EINVAL;
		}

		pdata->bst_ipk = (val - 1600) / 50;
	} else {
		dev_err(&i2c_client->dev,
			"Unable to find required parameter 'cirrus,boost-peak-milliamp'");
		return -EINVAL;
	}

	pdata->multi_amp_mode = of_property_read_bool(np,
					"cirrus,multi-amp-mode");

	pdata->dcm_mode = of_property_read_bool(np,
					"cirrus,dcm-mode-enable");

	pdata->amp_pcm_inv = of_property_read_bool(np,
					"cirrus,amp-pcm-inv");

	pdata->imon_pol_inv = of_property_read_bool(np,
					"cirrus,imon-pol-inv");

	pdata->vmon_pol_inv = of_property_read_bool(np,
					"cirrus,vmon-pol-inv");

	if (of_property_read_u32(np, "cirrus,temp-warn-threshold", &val) >= 0)
		pdata->temp_warn_thld = val | CS35L36_VALID_PDATA;

	if (of_property_read_u32(np, "cirrus,boost-ind-nanohenry", &val) >= 0) {
		pdata->boost_ind = val;
	} else {
		dev_err(&i2c_client->dev, "Inductor not specified.\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "cirrus,irq-drive-select", &val) >= 0)
		pdata->irq_drv_sel = val | CS35L36_VALID_PDATA;

	if (of_property_read_u32(np, "cirrus,irq-gpio-select", &val) >= 0)
		pdata->irq_gpio_sel = val | CS35L36_VALID_PDATA;

	/* VPBR Config */
	vpbr_node = of_get_child_by_name(np, "cirrus,vpbr-config");
	vpbr_config->is_present = vpbr_node ? true : false;
	if (vpbr_config->is_present) {
		if (of_property_read_u32(vpbr_node, "cirrus,vpbr-en",
					 &val) >= 0)
			vpbr_config->vpbr_en = val;
		if (of_property_read_u32(vpbr_node, "cirrus,vpbr-thld",
					 &val) >= 0)
			vpbr_config->vpbr_thld = val;
		if (of_property_read_u32(vpbr_node, "cirrus,vpbr-atk-rate",
					 &val) >= 0)
			vpbr_config->vpbr_atk_rate = val;
		if (of_property_read_u32(vpbr_node, "cirrus,vpbr-atk-vol",
					 &val) >= 0)
			vpbr_config->vpbr_atk_vol = val;
		if (of_property_read_u32(vpbr_node, "cirrus,vpbr-max-attn",
					 &val) >= 0)
			vpbr_config->vpbr_max_attn = val;
		if (of_property_read_u32(vpbr_node, "cirrus,vpbr-wait",
					 &val) >= 0)
			vpbr_config->vpbr_wait = val;
		if (of_property_read_u32(vpbr_node, "cirrus,vpbr-rel-rate",
					 &val) >= 0)
			vpbr_config->vpbr_rel_rate = val;
		if (of_property_read_u32(vpbr_node, "cirrus,vpbr-mute-en",
					 &val) >= 0)
			vpbr_config->vpbr_mute_en = val;
	}
	of_node_put(vpbr_node);

	// PLL Config
	ret = of_property_read_u32(np, "cirrus,pll-refclk-sel", &val);
	if (ret >= 0) {
		val |= CS35L36_VALID_PDATA;
		pdata->pll_refclk_sel = val;
	}

	ret = of_property_read_u32(np, "cirrus,pll-refclk-freq", &val);
	if (ret >= 0) {
		val |= CS35L36_VALID_PDATA;
		pdata->pll_refclk_freq = val;
	}

	// ASP Config
	asp_node = of_get_child_by_name(np, "cirrus,asp-config");
	if (asp_node) {
		ret = of_property_read_u32(asp_node, "cirrus,asp-rx-width", &val);
		if (ret >= 0) {
			val |= CS35L36_VALID_PDATA;
			asp_config->asp_rx_width = val;
		}

		ret = of_property_read_u32(asp_node, "cirrus,asp-tx-width", &val);
		if (ret >= 0) {
			val |= CS35L36_VALID_PDATA;
			asp_config->asp_tx_width = val;
		}

		ret = of_property_read_u32(asp_node, "cirrus,asp-fmt", &val);
		if (ret >= 0) {
			val |= CS35L36_VALID_PDATA;
			asp_config->asp_fmt = val;
		}

		ret = of_property_read_u32(asp_node, "cirrus,asp-sample-rate", &val);
		if (ret >= 0) {
			val |= CS35L36_VALID_PDATA;
			asp_config->asp_sample_rate = val;
		}

		ret = of_property_read_u32(asp_node, "cirrus,asp-sclk-rate", &val);
		if (ret >= 0) {
			val |= CS35L36_VALID_PDATA;
			asp_config->asp_sclk_rate = val;
		}
	}
	of_node_put(asp_node);

	return 0;
}

static int cs35l36_pac(struct cs35l36_private *cs35l36)
{
	int ret, count;
	unsigned int val;

	if (cs35l36->rev_id != CS35L36_REV_B0)
		return 0;

	/*
	 * Magic code for internal PAC
	 */
	regmap_write(cs35l36->regmap, CS35L36_TESTKEY_CTRL,
		     CS35L36_TEST_UNLOCK1);
	regmap_write(cs35l36->regmap, CS35L36_TESTKEY_CTRL,
		     CS35L36_TEST_UNLOCK2);

	usleep_range(9500, 10500);

	regmap_write(cs35l36->regmap, CS35L36_PAC_CTL1,
		     CS35L36_PAC_RESET);
	regmap_write(cs35l36->regmap, CS35L36_PAC_CTL3,
		     CS35L36_PAC_MEM_ACCESS);
	regmap_write(cs35l36->regmap, CS35L36_PAC_PMEM_WORD0,
		     CS35L36_B0_PAC_PATCH);

	regmap_write(cs35l36->regmap, CS35L36_PAC_CTL3,
		     CS35L36_PAC_MEM_ACCESS_CLR);
	regmap_write(cs35l36->regmap, CS35L36_PAC_CTL1,
		     CS35L36_PAC_ENABLE_MASK);

	usleep_range(9500, 10500);

	ret = regmap_read(cs35l36->regmap, CS35L36_INT4_STATUS, &val);
	if (ret < 0) {
		dev_err(cs35l36->dev, "Failed to read int4_status %d\n", ret);
		return ret;
	}

	count = 0;
	while (!(val & CS35L36_MCU_CONFIG_CLR)) {
		usleep_range(100, 200);
		count++;

		ret = regmap_read(cs35l36->regmap, CS35L36_INT4_STATUS,
				  &val);
		if (ret < 0) {
			dev_err(cs35l36->dev, "Failed to read int4_status %d\n",
				ret);
			return ret;
		}

		if (count >= 100)
			return -EINVAL;
	}

	regmap_write(cs35l36->regmap, CS35L36_INT4_STATUS,
		     CS35L36_MCU_CONFIG_CLR);
	regmap_update_bits(cs35l36->regmap, CS35L36_PAC_CTL1,
			   CS35L36_PAC_ENABLE_MASK, 0);

	regmap_write(cs35l36->regmap, CS35L36_TESTKEY_CTRL,
		     CS35L36_TEST_LOCK1);
	regmap_write(cs35l36->regmap, CS35L36_TESTKEY_CTRL,
		     CS35L36_TEST_LOCK2);

	return 0;
}

static void cs35l36_apply_vpbr_config(struct cs35l36_private *cs35l36)
{
	struct cs35l36_platform_data *pdata = &cs35l36->pdata;
	struct cs35l36_vpbr_cfg *vpbr_config = &pdata->vpbr_config;

	regmap_update_bits(cs35l36->regmap, CS35L36_PWR_CTRL3,
			   CS35L36_VPBR_EN_MASK,
			   vpbr_config->vpbr_en <<
			   CS35L36_VPBR_EN_SHIFT);
	regmap_update_bits(cs35l36->regmap, CS35L36_VPBR_CFG,
			   CS35L36_VPBR_THLD_MASK,
			   vpbr_config->vpbr_thld <<
			   CS35L36_VPBR_THLD_SHIFT);
	regmap_update_bits(cs35l36->regmap, CS35L36_VPBR_CFG,
			   CS35L36_VPBR_MAX_ATTN_MASK,
			   vpbr_config->vpbr_max_attn <<
			   CS35L36_VPBR_MAX_ATTN_SHIFT);
	regmap_update_bits(cs35l36->regmap, CS35L36_VPBR_CFG,
			   CS35L36_VPBR_ATK_VOL_MASK,
			   vpbr_config->vpbr_atk_vol <<
			   CS35L36_VPBR_ATK_VOL_SHIFT);
	regmap_update_bits(cs35l36->regmap, CS35L36_VPBR_CFG,
			   CS35L36_VPBR_ATK_RATE_MASK,
			   vpbr_config->vpbr_atk_rate <<
			   CS35L36_VPBR_ATK_RATE_SHIFT);
	regmap_update_bits(cs35l36->regmap, CS35L36_VPBR_CFG,
			   CS35L36_VPBR_WAIT_MASK,
			   vpbr_config->vpbr_wait <<
			   CS35L36_VPBR_WAIT_SHIFT);
	regmap_update_bits(cs35l36->regmap, CS35L36_VPBR_CFG,
			   CS35L36_VPBR_REL_RATE_MASK,
			   vpbr_config->vpbr_rel_rate <<
			   CS35L36_VPBR_REL_RATE_SHIFT);
	regmap_update_bits(cs35l36->regmap, CS35L36_VPBR_CFG,
			   CS35L36_VPBR_MUTE_EN_MASK,
			   vpbr_config->vpbr_mute_en <<
			   CS35L36_VPBR_MUTE_EN_SHIFT);
}

static long cs35l36_ioctl(struct file *f, unsigned int cmd, void __user *arg)
{
	struct miscdevice *dev = f->private_data;
	struct cs35l36_private *cs35l36;
	int ret = 0, val;

	cs35l36 = container_of(dev, struct cs35l36_private, misc_dev);

	mutex_lock(&cs35l36->lock);

	if (copy_from_user(&val, arg, sizeof(val))) {
		dev_err(cs35l36->dev, "copy from user failed\n");
		ret = -EFAULT;
		goto exit;
	}

	switch (cmd) {
	case CS35L36_SPK_DAC_VOLUME:
		break;
	case CS35L36_SPK_POWER_ON:
		ret = cs35l36_spk_power_on(cs35l36);
		break;
	case CS35L36_SPK_POWER_OFF:
		ret = cs35l36_spk_power_off(cs35l36);
		break;
	case CS35L36_SPK_DSP_BYPASS:
		break;
	case CS35L36_SPK_SWITCH_CALIBRATION:
		break;
	case CS35L36_SPK_SWITCH_CONFIGURATION:
		break;
	case CS35L36_SPK_SWITCH_FIRMWARE:
		break;
	case CS35L36_SPK_GET_R0:
		break;
	case CS35L36_SPK_GET_F0:
		break;
	case CS35L36_SPK_GET_CAL_STRUCT:
		break;
	case CS35L36_SPK_SET_CAL_STRUCT:
		break;
	case CS35L36_SPK_SET_AMBIENT:
		break;
	default:
		dev_err(cs35l36->dev, "Invalid IOCTL, command = %d\n", cmd);
		return -EINVAL;
	}

exit:
	mutex_unlock(&cs35l36->lock);

	return ret;
}

static long cs35l36_unlocked_ioctl(struct file *f, unsigned int cmd,
								   unsigned long arg)
{
	return cs35l36_ioctl(f, cmd, (void __user *)arg);
}

#ifdef CONFIG_COMPAT
static long cs35l36_compat_ioctl(struct file *f, unsigned int cmd,
								 unsigned long arg)
{
	struct miscdevice *dev = f->private_data;
	struct cs35l36_private *cs35l36;
	unsigned int cmd64;

	cs35l36 = container_of(dev, struct cs35l36_private, misc_dev);

	switch (cmd) {
	case CS35L36_SPK_DAC_VOLUME_COMPAT:
		cmd64 = CS35L36_SPK_DAC_VOLUME;
		break;
	case CS35L36_SPK_POWER_ON_COMPAT:
		cmd64 = CS35L36_SPK_POWER_ON;
		break;
	case CS35L36_SPK_POWER_OFF_COMPAT:
		cmd64 = CS35L36_SPK_POWER_OFF;
		break;
	case CS35L36_SPK_DSP_BYPASS_COMPAT:
		cmd64 = CS35L36_SPK_DSP_BYPASS;
		break;
	case CS35L36_SPK_SWITCH_CONFIGURATION_COMPAT:
		cmd64 = CS35L36_SPK_SWITCH_CONFIGURATION;
		break;
	case CS35L36_SPK_SWITCH_CALIBRATION_COMPAT:
		cmd64 = CS35L36_SPK_SWITCH_CALIBRATION;
		break;
	case CS35L36_SPK_GET_R0_COMPAT:
		cmd64 = CS35L36_SPK_GET_R0;
		break;
	case CS35L36_SPK_GET_F0_COMPAT:
		cmd64 = CS35L36_SPK_GET_F0;
		break;
	case CS35L36_SPK_GET_CAL_STRUCT_COMPAT:
		cmd64 = CS35L36_SPK_GET_CAL_STRUCT;
		break;
	case CS35L36_SPK_SET_CAL_STRUCT_COMPAT:
		cmd64 = CS35L36_SPK_SET_CAL_STRUCT;
		break;
	case CS35L36_SPK_SET_AMBIENT_COMPAT:
		cmd64 = CS35L36_SPK_SET_AMBIENT;
		break;
	case CS35L36_SPK_SWITCH_FIRMWARE_COMPAT:
		cmd64 = CS35L36_SPK_SWITCH_FIRMWARE;
		break;
	default:
		dev_err(cs35l36->dev, "Invalid IOCTL, command = %d\n", cmd);
		return -EINVAL;
	}

	return cs35l36_ioctl(f, cmd64, compat_ptr(arg));
}
#endif

static const struct reg_sequence cs35l36_reva0_errata_patch[] = {
	{ CS35L36_TESTKEY_CTRL,		CS35L36_TEST_UNLOCK1 },
	{ CS35L36_TESTKEY_CTRL,		CS35L36_TEST_UNLOCK2 },
	/* Errata Writes */
	{ CS35L36_OTP_CTRL1,		0x00002060 },
	{ CS35L36_OTP_CTRL2,		0x00000001 },
	{ CS35L36_OTP_CTRL1,		0x00002460 },
	{ CS35L36_OTP_CTRL2,		0x00000001 },
	{ 0x00002088,			0x012A1838 },
	{ 0x00003014,			0x0100EE0E },
	{ 0x00003008,			0x0008184A },
	{ 0x00007418,			0x509001C8 },
	{ 0x00007064,			0x0929A800 },
	{ 0x00002D10,			0x0002C01C },
	{ 0x0000410C,			0x00000A11 },
	{ 0x00006E08,			0x8B19140C },
	{ 0x00006454,			0x0300000A },
	{ CS35L36_AMP_NG_CTRL,		0x000020EF },
	{ 0x00007E34,			0x0000000E },
	{ 0x0000410C,			0x00000A11 },
	{ 0x00007410,			0x20514B00 },
	/* PAC Config */
	{ CS35L36_CTRL_OVRRIDE,		0x00000000 },
	{ CS35L36_PAC_INT0_CTRL,	0x00860001 },
	{ CS35L36_PAC_INT1_CTRL,	0x00860001 },
	{ CS35L36_PAC_INT2_CTRL,	0x00860001 },
	{ CS35L36_PAC_INT3_CTRL,	0x00860001 },
	{ CS35L36_PAC_INT4_CTRL,	0x00860001 },
	{ CS35L36_PAC_INT5_CTRL,	0x00860001 },
	{ CS35L36_PAC_INT6_CTRL,	0x00860001 },
	{ CS35L36_PAC_INT7_CTRL,	0x00860001 },
	{ CS35L36_PAC_INT_FLUSH_CTRL,	0x000000FF },
	{ CS35L36_TESTKEY_CTRL,		CS35L36_TEST_LOCK1 },
	{ CS35L36_TESTKEY_CTRL,		CS35L36_TEST_LOCK2 },
};

static const struct reg_sequence cs35l36_revb0_errata_patch[] = {
	{ CS35L36_TESTKEY_CTRL,	CS35L36_TEST_UNLOCK1 },
	{ CS35L36_TESTKEY_CTRL, CS35L36_TEST_UNLOCK2 },
	{ 0x00007064,		0x0929A800 },
	{ 0x00007850,		0x00002FA9 },
	{ 0x00007854,		0x0003F1D5 },
	{ 0x00007858,		0x0003F5E3 },
	{ 0x0000785C,		0x00001137 },
	{ 0x00007860,		0x0001A7A5 },
	{ 0x00007864,		0x0002F16A },
	{ 0x00007868,		0x00003E21 },
	{ 0x00007848,		0x00000001 },
	{ 0x00003854,		0x05180240 },
	{ 0x00007418,		0x509001C8 },
	{ 0x0000394C,		0x028764BD },
	{ CS35L36_TESTKEY_CTRL,	CS35L36_TEST_LOCK1 },
	{ CS35L36_TESTKEY_CTRL, CS35L36_TEST_LOCK2 },
};

static const struct file_operations cs35l36_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = cs35l36_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = cs35l36_compat_ioctl,
#endif
};

static int cs35l36_i2c_probe(struct i2c_client *i2c_client,
			      const struct i2c_device_id *id)
{
	struct cs35l36_private *cs35l36;
	struct device *dev = &i2c_client->dev;
	struct cs35l36_platform_data *pdata = dev_get_platdata(dev);
	struct irq_data *irq_d;
	int ret, irq_pol, chip_irq_pol, i;
	u32 reg_id, reg_revid, l37_id_reg;

	cs35l36 = devm_kzalloc(dev, sizeof(struct cs35l36_private), GFP_KERNEL);
	if (!cs35l36)
		return -ENOMEM;

	cs35l36->dev = dev;

	i2c_set_clientdata(i2c_client, cs35l36);
	cs35l36->regmap = devm_regmap_init_i2c(i2c_client, &cs35l36_regmap);
	if (IS_ERR(cs35l36->regmap)) {
		ret = PTR_ERR(cs35l36->regmap);
		dev_err(dev, "regmap_init() failed: %d\n", ret);
		goto err;
	}

	cs35l36->num_supplies = ARRAY_SIZE(cs35l36_supplies);
	for (i = 0; i < ARRAY_SIZE(cs35l36_supplies); i++)
		cs35l36->supplies[i].supply = cs35l36_supplies[i];

	ret = devm_regulator_bulk_get(dev, cs35l36->num_supplies,
				      cs35l36->supplies);
	if (ret != 0) {
		dev_err(dev, "Failed to request core supplies: %d\n", ret);
		return ret;
	}

	if (pdata) {
		cs35l36->pdata = *pdata;
	} else {
		pdata = devm_kzalloc(dev, sizeof(struct cs35l36_platform_data),
				     GFP_KERNEL);
		if (!pdata)
			return -ENOMEM;

		if (i2c_client->dev.of_node) {
			ret = cs35l36_handle_of_data(i2c_client, pdata);
			if (ret != 0)
				return ret;

		}

		cs35l36->pdata = *pdata;
	}

	ret = regulator_bulk_enable(cs35l36->num_supplies, cs35l36->supplies);
	if (ret != 0) {
		dev_err(dev, "Failed to enable core supplies: %d\n", ret);
		return ret;
	}

	/* returning NULL can be an option if in stereo mode */
	cs35l36->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						      GPIOD_OUT_LOW);
	if (IS_ERR(cs35l36->reset_gpio)) {
		ret = PTR_ERR(cs35l36->reset_gpio);
		cs35l36->reset_gpio = NULL;
		if (ret == -EBUSY) {
			dev_info(dev, "Reset line busy, assuming shared reset\n");
		} else {
			dev_err(dev, "Failed to get reset GPIO: %d\n", ret);
			goto err_disable_regs;
		}
	}

	if (cs35l36->reset_gpio)
		gpiod_set_value_cansleep(cs35l36->reset_gpio, 1);

	usleep_range(2000, 2100);

	/* initialize amplifier */
	ret = regmap_read(cs35l36->regmap, CS35L36_SW_RESET, &reg_id);
	if (ret < 0) {
		dev_err(dev, "Get Device ID failed %d\n", ret);
		goto err;
	}

	if (reg_id != CS35L36_CHIP_ID) {
		dev_err(dev, "Device ID (%X). Expected ID %X\n", reg_id,
			CS35L36_CHIP_ID);
		ret = -ENODEV;
		goto err;
	}

	ret = regmap_read(cs35l36->regmap, CS35L36_REV_ID, &reg_revid);
	if (ret < 0) {
		dev_err(&i2c_client->dev, "Get Revision ID failed %d\n", ret);
		goto err;
	}

	cs35l36->rev_id = reg_revid >> 8;

	ret = regmap_read(cs35l36->regmap, CS35L36_OTP_MEM30, &l37_id_reg);
	if (ret < 0) {
		dev_err(&i2c_client->dev, "Failed to read otp_id Register %d\n",
			ret);
		return ret;
	}

	if ((l37_id_reg & CS35L36_OTP_REV_MASK) == CS35L36_OTP_REV_L37)
		cs35l36->chip_version = CS35L36_12V_L37;
	else
		cs35l36->chip_version = CS35L36_10V_L36;

	switch (cs35l36->rev_id) {
	case CS35L36_REV_A0:
		ret = regmap_register_patch(cs35l36->regmap,
				cs35l36_reva0_errata_patch,
				ARRAY_SIZE(cs35l36_reva0_errata_patch));
		if (ret < 0) {
			dev_err(dev, "Failed to apply A0 errata patch %d\n",
				ret);
			goto err;
		}
		break;
	case CS35L36_REV_B0:
		ret = cs35l36_pac(cs35l36);
		if (ret < 0) {
			dev_err(dev, "Failed to Trim OTP %d\n", ret);
			goto err;
		}

		ret = regmap_register_patch(cs35l36->regmap,
				cs35l36_revb0_errata_patch,
				ARRAY_SIZE(cs35l36_revb0_errata_patch));
		if (ret < 0) {
			dev_err(dev, "Failed to apply B0 errata patch %d\n",
				ret);
			goto err;
		}
		break;
	}

	if (pdata->vpbr_config.is_present)
		cs35l36_apply_vpbr_config(cs35l36);

	irq_d = irq_get_irq_data(i2c_client->irq);
	if (!irq_d) {
		dev_err(&i2c_client->dev, "Invalid IRQ: %d\n", i2c_client->irq);
		ret = -ENODEV;
		goto err;
	}

	irq_pol = irqd_get_trigger_type(irq_d);

	switch (irq_pol) {
	case IRQF_TRIGGER_FALLING:
	case IRQF_TRIGGER_LOW:
		chip_irq_pol = 0;
		break;
	case IRQF_TRIGGER_RISING:
	case IRQF_TRIGGER_HIGH:
		chip_irq_pol = 1;
		break;
	default:
		dev_err(cs35l36->dev, "Invalid IRQ polarity: %d\n", irq_pol);
		ret = -EINVAL;
		goto err;
	}

	regmap_update_bits(cs35l36->regmap, CS35L36_PAD_INTERFACE,
			   CS35L36_INT_POL_SEL_MASK,
			   chip_irq_pol << CS35L36_INT_POL_SEL_SHIFT);

	ret = devm_request_threaded_irq(dev, i2c_client->irq, NULL, cs35l36_irq,
					IRQF_ONESHOT | irq_pol, "cs35l36",
					cs35l36);
	if (ret != 0) {
		dev_err(dev, "Failed to request IRQ: %d\n", ret);
		goto err;
	}

	regmap_update_bits(cs35l36->regmap, CS35L36_PAD_INTERFACE,
			   CS35L36_INT_OUTPUT_EN_MASK, 1);

	/* Set interrupt masks for critical errors */
	regmap_write(cs35l36->regmap, CS35L36_INT1_MASK,
		     CS35L36_INT1_MASK_DEFAULT);
	regmap_write(cs35l36->regmap, CS35L36_INT3_MASK,
		     CS35L36_INT3_MASK_DEFAULT);

	ret = cs35l36_probe(cs35l36);
	if (ret < 0) {
		dev_err(cs35l36->dev, "cs35l36 probe failed\n");
		goto err;
	}

	dev_info(&i2c_client->dev, "Cirrus Logic CS35L%d, Revision: %02X\n",
		 cs35l36->chip_version, reg_revid >> 8);

	cs35l36->misc_dev.minor = MISC_DYNAMIC_MINOR;
	cs35l36->misc_dev.name = "cs35l36";
	cs35l36->misc_dev.fops = &cs35l36_fops;

	ret = misc_register(&cs35l36->misc_dev);
	if (ret < 0) {
		dev_err(dev, "Register misc driver failed %d\n", ret);
		goto err;
	}

	dev_info(dev, "Register misc driver successful\n");

	return 0;

err:
	gpiod_set_value_cansleep(cs35l36->reset_gpio, 0);

err_disable_regs:
	regulator_bulk_disable(cs35l36->num_supplies, cs35l36->supplies);
	return ret;
}

static int cs35l36_i2c_remove(struct i2c_client *client)
{
	struct cs35l36_private *cs35l36 = i2c_get_clientdata(client);

	/* Reset interrupt masks for device removal */
	regmap_write(cs35l36->regmap, CS35L36_INT1_MASK,
		     CS35L36_INT1_MASK_RESET);
	regmap_write(cs35l36->regmap, CS35L36_INT3_MASK,
		     CS35L36_INT3_MASK_RESET);

	if (cs35l36->reset_gpio)
		gpiod_set_value_cansleep(cs35l36->reset_gpio, 0);

	regulator_bulk_disable(cs35l36->num_supplies, cs35l36->supplies);

	misc_deregister(&cs35l36->misc_dev);

	return 0;
}
static const struct of_device_id cs35l36_of_match[] = {
	{.compatible = "cirrus,cs35l36"},
	{},
};
MODULE_DEVICE_TABLE(of, cs35l36_of_match);

static const struct i2c_device_id cs35l36_id[] = {
	{"cs35l36", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, cs35l36_id);

static struct i2c_driver cs35l36_i2c_driver = {
	.driver = {
		.name = "cs35l36",
		.of_match_table = cs35l36_of_match,
	},
	.id_table = cs35l36_id,
	.probe = cs35l36_i2c_probe,
	.remove = cs35l36_i2c_remove,
};
module_i2c_driver(cs35l36_i2c_driver);

MODULE_DESCRIPTION("Misc CS35L36 driver");
MODULE_AUTHOR("Qi Zhou <qizhou@opensource.cirrus.com>");
MODULE_LICENSE("GPL v2");