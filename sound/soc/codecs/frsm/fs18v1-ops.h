/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Digital FS18xx (FS1816/FS1832/FS1838, I2S input, DSP) operations,
 * matching the register sequences of the FourSemi fs18v1 driver.
 */

#ifndef __FS18V1_OPS_H__
#define __FS18V1_OPS_H__

#include "frsm-dev.h"
#include "internal.h"

#define FS18V1_03H_DEVID		0x03
#define FS18V1_04H_I2SCTRL		0x04
#define FS18V1_06H_AUDIOCTRL		0x06
#define FS18V1_08H_SYSCTRL		0x08
#define FS18V1_09H_PWRCTRL		0x09
#define FS18V1_0BH_ACCKEY		0x0B
#define FS18V1_58H_STATUS		0x58
#define FS18V1_90H_CHIPINI		0x90
#define FS18V1_A2H_DACEQWL		0xA2
#define FS18V1_A6H_DACEQA		0xA6
#define FS18V1_BDH_DIGSTAT		0xBD
#define FS18V1_C1H_PLLCTRL1		0xC1
#define FS18V1_C2H_PLLCTRL2		0xC2
#define FS18V1_C3H_PLLCTRL3		0xC3
#define FS18V1_C4H_PLLCTRL4		0xC4

#define FS18V1_08H_UNINIT		0x0010
#define FS18V1_08H_INITED		0x0310
#define FS18V1_09H_I2C_RESET		0x0003
#define FS18V1_09H_POWER_DOWN		0x0001
#define FS18V1_09H_ACCESS		0x0000
#define FS18V1_09H_UNMUTE		0x0008
#define FS18V1_0BH_ACCKEY_ON		0xCA91
#define FS18V1_90H_INIST_MASK		0x0003
#define FS18V1_BDH_DACRUN_MASK		BIT(1)
#define FS18V1_C4H_PLL_ON		0x000B
#define FS18V1_C4H_PLL_OFF		0x0000
#define FS18V1_04H_I2SSR_SHIFT		12
#define FS18V1_04H_I2SSR_MASK		GENMASK(15, 12)
#define FS18V1_VOLUME_MAX		0x3FF

static int fs18v1_set_mute(struct frsm_dev *frsm_dev, int mute);

static int fs18v1_wait_dac_off(struct frsm_dev *frsm_dev)
{
	uint16_t val;
	int i, ret;

	FRSM_DELAY_MS(5);
	for (i = 0; i < 20; i++) {
		ret = frsm_reg_read_status(frsm_dev, FS18V1_BDH_DIGSTAT, &val);
		if (ret)
			return ret;
		if (!(val & FS18V1_BDH_DACRUN_MASK))
			return 0;
		FRSM_DELAY_MS(2);
	}
	dev_err(frsm_dev->dev, "Wait %02Xh stable timeout!\n",
		FS18V1_BDH_DIGSTAT);
	return -ETIMEDOUT;
}

static int fs18v1_access_cram(struct frsm_dev *frsm_dev, bool on)
{
	int ret;

	if (on) {
		ret  = frsm_reg_write(frsm_dev, FS18V1_C4H_PLLCTRL4,
				FS18V1_C4H_PLL_ON);
		ret |= frsm_reg_write(frsm_dev, FS18V1_09H_PWRCTRL,
				FS18V1_09H_ACCESS);
		ret |= frsm_reg_write(frsm_dev, FS18V1_0BH_ACCKEY,
				FS18V1_0BH_ACCKEY_ON);
		return ret;
	}

	ret  = frsm_reg_write(frsm_dev, FS18V1_0BH_ACCKEY, 0);
	ret |= frsm_reg_write(frsm_dev, FS18V1_09H_PWRCTRL,
			FS18V1_09H_POWER_DOWN);
	ret |= frsm_reg_write(frsm_dev, FS18V1_C4H_PLLCTRL4,
			FS18V1_C4H_PLL_OFF);
	ret |= fs18v1_wait_dac_off(frsm_dev);
	return ret;
}

static int fs18v1_write_effect(struct frsm_dev *frsm_dev, uint16_t effect)
{
	int ret;

	if (effect == 0xFFFF)
		return 0;

	ret  = frsm_reg_write(frsm_dev, FS18V1_A6H_DACEQA, 0);
	ret |= frsm_write_effect_table(frsm_dev, FS18V1_A2H_DACEQWL, effect);
	ret |= frsm_reg_write(frsm_dev, FS18V1_A6H_DACEQA, 0);
	return ret;
}

static int fs18v1_dev_init(struct frsm_dev *frsm_dev)
{
	struct scene_table *scene;
	uint16_t val;
	int i, ret;

	if (frsm_dev == NULL || frsm_dev->dev == NULL)
		return -EINVAL;

	ret = frsm_reg_read(frsm_dev, FS18V1_08H_SYSCTRL, &val);
	if (ret)
		return ret;
	if (!frsm_dev->force_init && val != FS18V1_08H_UNINIT)
		return 0;

	frsm_dev->force_init = false;
	frsm_dev->state &= BIT(EVENT_STAT_MNTR);

	ret = frsm_reg_write(frsm_dev, FS18V1_09H_PWRCTRL,
			FS18V1_09H_I2C_RESET);
	FRSM_DELAY_MS(10);
	for (i = 0; i < 20; i++) {
		ret |= frsm_reg_read_status(frsm_dev, FS18V1_90H_CHIPINI, &val);
		if (ret || (val & FS18V1_90H_INIST_MASK) == FS18V1_90H_INIST_MASK)
			break;
		FRSM_DELAY_MS(2);
	}
	if (ret || (val & FS18V1_90H_INIST_MASK) != FS18V1_90H_INIST_MASK) {
		dev_err(frsm_dev->dev, "Wait %02Xh stable timeout!\n",
			FS18V1_90H_CHIPINI);
		return ret ? ret : -ETIMEDOUT;
	}

	if (frsm_dev->tbl_scene == NULL) {
		dev_err(frsm_dev->dev, "Scene table is null\n");
		return -EINVAL;
	}

	scene = (struct scene_table *)frsm_dev->tbl_scene->buf;
	ret = frsm_write_reg_table(frsm_dev, scene->reg);
	if (ret)
		return ret;

	ret = fs18v1_access_cram(frsm_dev, true);
	if (!ret)
		ret = fs18v1_write_effect(frsm_dev, scene->effect);
	ret |= fs18v1_access_cram(frsm_dev, false);

	ret |= fs18v1_set_mute(frsm_dev, 1);
	ret |= frsm_reg_write(frsm_dev, FS18V1_C4H_PLLCTRL4, FS18V1_C4H_PLL_OFF);
	if (!ret) {
		frsm_dev->cur_scene = scene;
		ret = frsm_reg_write(frsm_dev, FS18V1_08H_SYSCTRL,
				FS18V1_08H_INITED);
	}

	return ret;
}

static int fs18v1_set_scene(struct frsm_dev *frsm_dev,
		struct scene_table *scene)
{
	const struct scene_table *cur_scene;
	int ret;

	if (frsm_dev == NULL || frsm_dev->dev == NULL || scene == NULL)
		return -EINVAL;

	cur_scene = frsm_dev->cur_scene;
	if (cur_scene == scene)
		return 0;
	if (cur_scene && cur_scene->reg == scene->reg)
		return 0;

	ret = frsm_write_reg_table(frsm_dev, scene->reg);
	ret |= fs18v1_access_cram(frsm_dev, true);
	if (!ret)
		ret = fs18v1_write_effect(frsm_dev, scene->effect);
	ret |= fs18v1_access_cram(frsm_dev, false);

	return ret;
}

static const struct frsm_rate fs18v1_rates[] = {
	{  8000, 0 },
	{ 16000, 3 },
	{ 44100, 7 },
	{ 48000, 8 },
};

static const struct frsm_pll fs18v1_plls[] = {
	{   256000, 0x01A0, 0x0180, 0x0001 },
	{   512000, 0x01A0, 0x0180, 0x0002 },
	{  1024000, 0x0260, 0x0120, 0x0003 },
	{  1411200, 0x01A0, 0x0100, 0x0004 },
	{  1536000, 0x0260, 0x0100, 0x0004 },
	{  2048000, 0x0260, 0x0120, 0x0006 },
	{  2822400, 0x01A0, 0x0100, 0x0008 },
	{  3072000, 0x0260, 0x0100, 0x0008 },
	{  6144000, 0x0260, 0x0100, 0x0010 },
	{ 12288000, 0x0260, 0x0100, 0x0020 },
};

static int fs18v1_hw_params(struct frsm_dev *frsm_dev)
{
	struct frsm_hw_params *hw = &frsm_dev->hw_params;
	const struct frsm_pll *pll = NULL;
	int i, sr = -1, ret;

	/* default: 48 kHz, stereo 32-bit slots (64 fs) from the AFE */
	if (hw->rate == 0)
		hw->rate = 48000;
	if (hw->bclk == 0)
		hw->bclk = hw->rate * 64;

	for (i = 0; i < ARRAY_SIZE(fs18v1_rates); i++)
		if (fs18v1_rates[i].rate == hw->rate)
			sr = fs18v1_rates[i].i2ssr;
	for (i = 0; i < ARRAY_SIZE(fs18v1_plls); i++)
		if (fs18v1_plls[i].bclk == hw->bclk)
			pll = &fs18v1_plls[i];
	if (sr < 0 || pll == NULL) {
		dev_err(frsm_dev->dev, "Unsupported rate %d bclk %d\n",
			hw->rate, hw->bclk);
		return -EINVAL;
	}

	ret  = frsm_reg_update_bits(frsm_dev, FS18V1_04H_I2SCTRL,
			FS18V1_04H_I2SSR_MASK, sr << FS18V1_04H_I2SSR_SHIFT);
	ret |= frsm_reg_write(frsm_dev, FS18V1_C1H_PLLCTRL1, pll->pll1);
	ret |= frsm_reg_write(frsm_dev, FS18V1_C2H_PLLCTRL2, pll->pll2);
	ret |= frsm_reg_write(frsm_dev, FS18V1_C3H_PLLCTRL3, pll->pll3);

	return ret;
}

static int fs18v1_set_volume(struct frsm_dev *frsm_dev, uint16_t volume)
{
	volume = min_t(uint16_t, volume, FS18V1_VOLUME_MAX);
	return frsm_reg_write(frsm_dev, FS18V1_06H_AUDIOCTRL, volume << 6);
}

static int fs18v1_start_up(struct frsm_dev *frsm_dev)
{
	int ret;

	ret  = frsm_reg_write(frsm_dev, FS18V1_C4H_PLLCTRL4, FS18V1_C4H_PLL_ON);
	ret |= fs18v1_set_volume(frsm_dev, frsm_dev->volume);
	return ret;
}

static int fs18v1_set_mute(struct frsm_dev *frsm_dev, int mute)
{
	uint16_t val;
	int ret;

	if (mute) {
		ret  = frsm_reg_write(frsm_dev, FS18V1_09H_PWRCTRL,
				FS18V1_09H_POWER_DOWN);
		ret |= fs18v1_wait_dac_off(frsm_dev);
		return ret;
	}

	ret  = frsm_reg_write(frsm_dev, FS18V1_09H_PWRCTRL,
			FS18V1_09H_UNMUTE);
	ret |= frsm_reg_read_status(frsm_dev, FS18V1_58H_STATUS, &val);
	return ret;
}

static int fs18v1_shut_down(struct frsm_dev *frsm_dev)
{
	return frsm_reg_write(frsm_dev, FS18V1_C4H_PLLCTRL4, FS18V1_C4H_PLL_OFF);
}

static int fs18v1_dev_ops(struct frsm_dev *frsm_dev)
{
	if (frsm_dev == NULL || frsm_dev->dev == NULL)
		return -EINVAL;

	frsm_dev->bst_wcam_len = 4;
	if (frsm_dev->volume == 0)
		frsm_dev->volume = FS18V1_VOLUME_MAX;

	frsm_dev->ops.dev_init  = fs18v1_dev_init;
	frsm_dev->ops.set_scene = fs18v1_set_scene;
	frsm_dev->ops.hw_params = fs18v1_hw_params;
	frsm_dev->ops.start_up  = fs18v1_start_up;
	frsm_dev->ops.set_mute  = fs18v1_set_mute;
	frsm_dev->ops.shut_down = fs18v1_shut_down;
	frsm_dev->ops.set_volume = fs18v1_set_volume;

	return 0;
}

#endif /* __FS18V1_OPS_H__ */
