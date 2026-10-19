/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Digital FS19xx (device id 0x05xx, I2S input, DSP) operations,
 * matching the register sequences of the FourSemi fs19v2 driver.
 */

#ifndef __FS19V2_OPS_H__
#define __FS19V2_OPS_H__

#include "frsm-dev.h"
#include "internal.h"

#define FS19V2_03H_DEVID		0x03
#define FS19V2_06H_DIGSTAT		0x06
#define FS19V2_0BH_ACCKEY		0x0B
#define FS19V2_0EH_CHIPINI		0x0E
#define FS19V2_10H_PWRCTRL		0x10
#define FS19V2_11H_SYSCTRL		0x11
#define FS19V2_14H_SPKCOEF		0x14
#define FS19V2_16H_AUDIOCTRL		0x16
#define FS19V2_17H_I2SCTRL		0x17
#define FS19V2_4CH_TSCTRL		0x4C
#define FS19V2_62H_DACEQWL		0x62
#define FS19V2_66H_DACEQA		0x66
#define FS19V2_82H_ADCEQWL		0x82
#define FS19V2_86H_ADCEQA		0x86
#define FS19V2_A1H_PLLCTRL1		0xA1
#define FS19V2_A2H_PLLCTRL2		0xA2
#define FS19V2_A3H_PLLCTRL3		0xA3
#define FS19V2_ABH_STATUS		0xAB
#define FS19V2_C3H_OTPID		0xC3

#define FS19V2_DEVID			0x05
#define FS19V2_FWM_DEVID		0x3700
#define FS19V2_0BH_ACCKEY_ON		0xCA91
#define FS19V2_10H_I2C_RESET		0x0002
#define FS19V2_10H_POWER_DOWN		0x0001
#define FS19V2_10H_POWER_UP		0x0000
#define FS19V2_11H_SYSTEM_UP		0x00EF
#define FS19V2_11H_SYSTEM_DOWN		0x0022
#define FS19V2_14H_INITED		0x5A5A
#define FS19V2_0EH_INIST_MASK		0x0003
#define FS19V2_4CH_OFFSTA		BIT(14)
#define FS19V2_06H_DACRUN		BIT(1)
#define FS19V2_06H_RUN_MASK		0x0007
#define FS19V2_17H_I2SSR_SHIFT		12
#define FS19V2_17H_I2SSR_MASK		GENMASK(15, 12)
#define FS19V2_VOLUME_MAX		0x3FF

#define FS19V2_MODEL_DAC_SIZE		0x154
#define FS19V2_MODEL_ADC_SIZE		0x168
#define FS19V2_EFFECT_PART1_SIZE	0xF0
#define FS19V2_EFFECT_PART2_SIZE	0x3CC

static int fs19v2_set_mute(struct frsm_dev *frsm_dev, int mute);

/* -12 dB */
static unsigned int fs19v2_default_volume = 0x37F;

static int fs19v2_poll(struct frsm_dev *frsm_dev, uint8_t reg,
		uint16_t mask, uint16_t val)
{
	uint16_t status;
	int i, ret;

	FRSM_DELAY_MS(5);
	for (i = 0; i < 20; i++) {
		ret = frsm_reg_read_status(frsm_dev, reg, &status);
		if (ret)
			return ret;
		if ((status & mask) == val)
			return 0;
		FRSM_DELAY_MS(2);
	}
	dev_dbg(frsm_dev->dev, "Wait %02Xh stable timeout!\n", reg);
	return -ETIMEDOUT;
}

static int fs19v2_access_cram(struct frsm_dev *frsm_dev, bool on)
{
	int ret;

	if (on) {
		ret  = frsm_reg_write(frsm_dev, FS19V2_11H_SYSCTRL,
				FS19V2_11H_SYSTEM_DOWN);
		ret |= frsm_reg_write(frsm_dev, FS19V2_10H_PWRCTRL,
				FS19V2_10H_POWER_UP);
		ret |= frsm_reg_write(frsm_dev, FS19V2_0BH_ACCKEY,
				FS19V2_0BH_ACCKEY_ON);
		return ret;
	}

	ret  = frsm_reg_write(frsm_dev, FS19V2_0BH_ACCKEY, 0);
	ret |= frsm_reg_write(frsm_dev, FS19V2_10H_PWRCTRL,
			FS19V2_10H_POWER_DOWN);
	ret |= frsm_reg_write(frsm_dev, FS19V2_11H_SYSCTRL,
			FS19V2_11H_SYSTEM_DOWN);
	ret |= fs19v2_poll(frsm_dev, FS19V2_06H_DIGSTAT, FS19V2_06H_RUN_MASK, 0);
	return ret;
}

/* fetch a model/effect blob from the firmware: {u16 rsvd, u16 size, data} */
static const uint8_t *fs19v2_fwm_blob(struct frsm_dev *frsm_dev, int type,
		uint16_t offset, uint16_t want)
{
	struct fwm_table *table;
	const struct file_table *blob;

	table = frsm_get_fwm_table(frsm_dev, type);
	if (table == NULL) {
		dev_err(frsm_dev->dev, "Failed to find table %d\n", type);
		return NULL;
	}

	blob = (const struct file_table *)((const char *)table + offset);
	if (blob->size < want) {
		dev_err(frsm_dev->dev, "Table %d blob too small: %d < %d\n",
			type, blob->size, want);
		return NULL;
	}
	return blob->buf;
}

static int fs19v2_write_model(struct frsm_dev *frsm_dev, uint16_t model)
{
	const uint8_t *buf;
	int ret;

	if (model == 0xFFFF)
		return 0;
	if (frsm_dev->cur_scene && frsm_dev->cur_scene->model == model)
		return 0;

	buf = fs19v2_fwm_blob(frsm_dev, INDEX_MODEL, model,
			FS19V2_MODEL_DAC_SIZE + FS19V2_MODEL_ADC_SIZE);
	if (buf == NULL)
		return -EINVAL;

	ret  = frsm_reg_write(frsm_dev, FS19V2_66H_DACEQA, 0x803C);
	ret |= frsm_reg_write(frsm_dev, FS19V2_86H_ADCEQA, 0x8000);
	ret |= frsm_reg_bulk_write(frsm_dev, FS19V2_62H_DACEQWL,
			buf, FS19V2_MODEL_DAC_SIZE);
	ret |= frsm_reg_bulk_write(frsm_dev, FS19V2_82H_ADCEQWL,
			buf + FS19V2_MODEL_DAC_SIZE, FS19V2_MODEL_ADC_SIZE);
	ret |= frsm_reg_write(frsm_dev, FS19V2_66H_DACEQA, 0);
	ret |= frsm_reg_write(frsm_dev, FS19V2_86H_ADCEQA, 0);

	return ret;
}

static int fs19v2_write_effect(struct frsm_dev *frsm_dev, uint16_t effect)
{
	const uint8_t *buf;
	int ret;

	if (effect == 0xFFFF)
		return 0;
	if (frsm_dev->cur_scene && frsm_dev->cur_scene->effect == effect)
		return 0;

	buf = fs19v2_fwm_blob(frsm_dev, INDEX_EFFECT, effect,
			FS19V2_EFFECT_PART1_SIZE + FS19V2_EFFECT_PART2_SIZE);
	if (buf == NULL)
		return -EINVAL;

	ret  = frsm_reg_write(frsm_dev, FS19V2_66H_DACEQA, 0x8000);
	ret |= frsm_reg_bulk_write(frsm_dev, FS19V2_62H_DACEQWL,
			buf, FS19V2_EFFECT_PART1_SIZE);
	ret |= frsm_reg_write(frsm_dev, FS19V2_66H_DACEQA, 0x8091);
	ret |= frsm_reg_bulk_write(frsm_dev, FS19V2_62H_DACEQWL,
			buf + FS19V2_EFFECT_PART1_SIZE, FS19V2_EFFECT_PART2_SIZE);
	ret |= frsm_reg_write(frsm_dev, FS19V2_66H_DACEQA, 0);

	return ret;
}

static int fs19v2_dev_init(struct frsm_dev *frsm_dev)
{
	struct scene_table *scene;
	uint16_t val;
	int ret;

	if (frsm_dev == NULL || frsm_dev->dev == NULL)
		return -EINVAL;

	ret = frsm_reg_read(frsm_dev, FS19V2_14H_SPKCOEF, &val);
	if (ret)
		return ret;
	if (!frsm_dev->force_init && val != 0)
		return 0;

	frsm_dev->force_init = false;
	frsm_dev->state &= BIT(EVENT_STAT_MNTR);

	ret = frsm_reg_write(frsm_dev, FS19V2_10H_PWRCTRL,
			FS19V2_10H_I2C_RESET);
	FRSM_DELAY_MS(5);
	ret |= fs19v2_poll(frsm_dev, FS19V2_0EH_CHIPINI,
			FS19V2_0EH_INIST_MASK, FS19V2_0EH_INIST_MASK);
	if (ret)
		return ret;

	if (frsm_dev->tbl_scene == NULL) {
		dev_err(frsm_dev->dev, "Scene table is null\n");
		return -EINVAL;
	}

	scene = (struct scene_table *)frsm_dev->tbl_scene->buf;
	ret = frsm_write_reg_table(frsm_dev, scene->reg);
	if (ret)
		return ret;

	frsm_dev->cur_scene = NULL;
	ret = fs19v2_access_cram(frsm_dev, true);
	if (!ret) {
		ret  = fs19v2_write_model(frsm_dev, scene->model);
		ret |= fs19v2_write_effect(frsm_dev, scene->effect);
	}
	frsm_reg_read_status(frsm_dev, FS19V2_ABH_STATUS, &val);
	ret |= fs19v2_access_cram(frsm_dev, false);

	ret |= fs19v2_set_mute(frsm_dev, 1);
	ret |= frsm_reg_write(frsm_dev, FS19V2_11H_SYSCTRL,
			FS19V2_11H_SYSTEM_DOWN);
	if (!ret) {
		frsm_dev->cur_scene = scene;
		ret = frsm_reg_write(frsm_dev, FS19V2_14H_SPKCOEF,
				FS19V2_14H_INITED);
	}

	return ret;
}

static int fs19v2_set_scene(struct frsm_dev *frsm_dev,
		struct scene_table *scene)
{
	const struct scene_table *cur_scene;
	int ret;

	if (frsm_dev == NULL || frsm_dev->dev == NULL || scene == NULL)
		return -EINVAL;

	cur_scene = frsm_dev->cur_scene;
	if (cur_scene == scene)
		return 0;

	ret = 0;
	if (cur_scene == NULL || cur_scene->reg != scene->reg)
		ret = frsm_write_reg_table(frsm_dev, scene->reg);
	ret |= fs19v2_access_cram(frsm_dev, true);
	if (!ret) {
		ret  = fs19v2_write_model(frsm_dev, scene->model);
		ret |= fs19v2_write_effect(frsm_dev, scene->effect);
	}
	ret |= fs19v2_access_cram(frsm_dev, false);

	return ret;
}

static const struct frsm_rate fs19v2_rates[] = {
	{  8000, 1 },
	{ 16000, 3 },
	{ 32000, 7 },
	{ 44100, 8 },
	{ 48000, 9 },
	{ 88200, 10 },
	{ 96000, 11 },
};

static const struct frsm_pll fs19v2_plls[] = {
	{  1024000, 0x0264, 0x0B40, 0x000C },
	{  1411200, 0x0260, 0x0540, 0x0006 },
	{  1536000, 0x0260, 0x0540, 0x0006 },
	{  2048000, 0x0264, 0x05A0, 0x000C },
	{  2822400, 0x0260, 0x0540, 0x000C },
	{  3072000, 0x0264, 0x03C0, 0x000C },
	{  5644800, 0x0260, 0x0540, 0x0018 },
	{  6144000, 0x0264, 0x01E0, 0x000C },
	{ 12288000, 0x0260, 0x0540, 0x0030 },
};

static int fs19v2_hw_params(struct frsm_dev *frsm_dev)
{
	struct frsm_hw_params *hw = &frsm_dev->hw_params;
	const struct frsm_pll *pll = NULL;
	int i, sr = -1, ret;

	/* default: 48 kHz, stereo 32-bit slots (64 fs) from the AFE */
	if (hw->rate == 0)
		hw->rate = 48000;
	if (hw->bclk == 0)
		hw->bclk = hw->rate * 64;

	for (i = 0; i < ARRAY_SIZE(fs19v2_rates); i++)
		if (fs19v2_rates[i].rate == hw->rate)
			sr = fs19v2_rates[i].i2ssr;
	for (i = 0; i < ARRAY_SIZE(fs19v2_plls); i++)
		if (fs19v2_plls[i].bclk == hw->bclk)
			pll = &fs19v2_plls[i];
	if (sr < 0 || pll == NULL) {
		dev_err(frsm_dev->dev, "Unsupported rate %d bclk %d\n",
			hw->rate, hw->bclk);
		return -EINVAL;
	}

	ret  = frsm_reg_update_bits(frsm_dev, FS19V2_17H_I2SCTRL,
			FS19V2_17H_I2SSR_MASK, sr << FS19V2_17H_I2SSR_SHIFT);
	ret |= frsm_reg_write(frsm_dev, FS19V2_A1H_PLLCTRL1, pll->pll1);
	ret |= frsm_reg_write(frsm_dev, FS19V2_A2H_PLLCTRL2, pll->pll2);
	ret |= frsm_reg_write(frsm_dev, FS19V2_A3H_PLLCTRL3, pll->pll3);

	return ret;
}

static int fs19v2_set_volume(struct frsm_dev *frsm_dev, uint16_t volume)
{
	volume = min_t(uint16_t, volume, FS19V2_VOLUME_MAX);
	return frsm_reg_write(frsm_dev, FS19V2_16H_AUDIOCTRL, volume << 6);
}

static int fs19v2_start_up(struct frsm_dev *frsm_dev)
{
	int ret;

	ret  = frsm_reg_write(frsm_dev, FS19V2_11H_SYSCTRL,
			FS19V2_11H_SYSTEM_UP);
	ret |= fs19v2_set_volume(frsm_dev, frsm_dev->volume);
	return ret;
}

static int fs19v2_set_mute(struct frsm_dev *frsm_dev, int mute)
{
	uint16_t val;
	int ret;

	if (mute) {
		ret = frsm_reg_write(frsm_dev, FS19V2_10H_PWRCTRL,
				FS19V2_10H_POWER_DOWN);
		/*
		 * The ramp-down needs BCK, which the AFE has usually stopped
		 * by the time DAPM powers the speaker down: don't fail on it.
		 */
		if (fs19v2_poll(frsm_dev, FS19V2_4CH_TSCTRL,
				FS19V2_4CH_OFFSTA, FS19V2_4CH_OFFSTA) ||
		    fs19v2_poll(frsm_dev, FS19V2_06H_DIGSTAT,
				FS19V2_06H_DACRUN, 0))
			dev_dbg(frsm_dev->dev, "mute without ramp-down\n");
	} else {
		/* let the PLL settle on the running BCK before unmuting */
		FRSM_DELAY_MS(10);
		ret = frsm_reg_write(frsm_dev, FS19V2_10H_PWRCTRL,
				FS19V2_10H_POWER_UP);
	}
	frsm_reg_read_status(frsm_dev, FS19V2_ABH_STATUS, &val);

	return ret;
}

static int fs19v2_shut_down(struct frsm_dev *frsm_dev)
{
	return frsm_reg_write(frsm_dev, FS19V2_11H_SYSCTRL,
			FS19V2_11H_SYSTEM_DOWN);
}

static int fs19v2_dev_ops(struct frsm_dev *frsm_dev)
{
	uint16_t otp;
	int ret;

	if (frsm_dev == NULL || frsm_dev->dev == NULL)
		return -EINVAL;

	ret  = frsm_reg_write(frsm_dev, FS19V2_0BH_ACCKEY, FS19V2_0BH_ACCKEY_ON);
	ret |= frsm_reg_read(frsm_dev, FS19V2_C3H_OTPID, &otp);
	ret |= frsm_reg_write(frsm_dev, FS19V2_0BH_ACCKEY, 0);
	if (ret || (otp & 0xE0) != 0x80) {
		dev_err(frsm_dev->dev, "Unexpected OTP id %04X\n", otp);
		return -ENODEV;
	}

	frsm_dev->dev_id = FS19V2_FWM_DEVID;
	frsm_dev->bst_wcam_len = 0;
	/* 0x3FF is 0 dB (0.09375 dB/step); start well below it */
	if (frsm_dev->volume == 0)
		frsm_dev->volume = fs19v2_default_volume;

	frsm_dev->ops.dev_init  = fs19v2_dev_init;
	frsm_dev->ops.set_scene = fs19v2_set_scene;
	frsm_dev->ops.hw_params = fs19v2_hw_params;
	frsm_dev->ops.start_up  = fs19v2_start_up;
	frsm_dev->ops.set_mute  = fs19v2_set_mute;
	frsm_dev->ops.shut_down = fs19v2_shut_down;
	frsm_dev->ops.set_volume = fs19v2_set_volume;

	return 0;
}

#endif /* __FS19V2_OPS_H__ */
