// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW8697 haptic driver, rumble only
 *
 * Plays the closed-loop continuous (CONT) mode, which needs no waveform
 * firmware: the chip drives the LRA at its resonance frequency until stopped.
 * RAM and RTP waveform playback, f0 calibration, the trigger inputs and the
 * interrupt line are not supported.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define AW8697_REG_ID			0x00
#define AW8697_CHIPID			0x97
#define AW8697_SOFT_RESET		0xaa

#define AW8697_REG_SYSINT		0x02
#define AW8697_REG_SYSINTM		0x03
#define AW8697_SYSINTM_UVLO		BIT(5)

#define AW8697_REG_SYSCTRL		0x04
#define AW8697_SYSCTRL_PLAY_MODE	GENMASK(3, 2)
#define AW8697_PLAY_MODE_CONT		2
#define AW8697_SYSCTRL_BST_MODE		BIT(1)
#define AW8697_SYSCTRL_STANDBY		BIT(0)

#define AW8697_REG_GO			0x05
#define AW8697_GO			BIT(0)

#define AW8697_REG_PWMPRC		0x2d
#define AW8697_PWMPRC_EN		BIT(7)
#define AW8697_REG_PWMDBG		0x2e
#define AW8697_PWMDBG_PWM_MODE		GENMASK(6, 5)
#define AW8697_PWM_24K			2
#define AW8697_REG_BSTDBG1		0x31
#define AW8697_REG_BSTDBG2		0x32
#define AW8697_REG_BSTDBG3		0x33
#define AW8697_REG_BSTCFG		0x34
#define AW8697_BSTCFG_PEAKCUR		GENMASK(2, 0)
#define AW8697_BSTCFG_PEAKCUR_3P5A	5
#define AW8697_REG_ANADBG		0x35
#define AW8697_ANADBG_IOC		GENMASK(3, 2)
#define AW8697_ANADBG_IOC_4P65A		3
#define AW8697_REG_PRLVL		0x3e
#define AW8697_PRLVL_PR_EN		BIT(7)

#define AW8697_REG_BST_AUTO		0x47
#define AW8697_BST_AUTO_AUTOSW		BIT(2)
#define AW8697_REG_CONT_CTRL		0x48
#define AW8697_CONT_CTRL_ZC_DETEC	BIT(7)
#define AW8697_CONT_CTRL_WAIT_PERIOD	GENMASK(6, 5)
#define AW8697_CONT_CTRL_BY_DRV_TIME	BIT(4)
#define AW8697_CONT_CTRL_CLOSE_PLAYBACK	BIT(3)
#define AW8697_CONT_CTRL_F0_DETECT	BIT(2)
#define AW8697_CONT_CTRL_O2C		BIT(1)
#define AW8697_CONT_CTRL_AUTO_BRK	BIT(0)
#define AW8697_REG_F_PRE_H		0x49
#define AW8697_REG_F_PRE_L		0x4a
#define AW8697_REG_TD_H			0x4b
#define AW8697_REG_TD_L			0x4c
#define AW8697_REG_TSET			0x4d

#define AW8697_REG_GLB_STATE		0x46
#define AW8697_GLB_STATE_MASK		GENMASK(3, 0)

#define AW8697_REG_DATCTRL		0x2b
#define AW8697_DATCTRL_FC		GENMASK(7, 6)
#define AW8697_DATCTRL_FC_1000HZ	3
#define AW8697_DATCTRL_LPF_EN		BIT(5)

#define AW8697_REG_R_SPARE		0x5d
#define AW8697_REG_DETCTRL		0x5f
#define AW8697_DETCTRL_PROTECT_NO_ACT	BIT(5)
#define AW8697_REG_ADCTEST		0x66
#define AW8697_ADCTEST_VBAT_HW_COMP	BIT(6)

#define AW8697_REG_ZC_THRSH_H		0x72
#define AW8697_REG_ZC_THRSH_L		0x73
#define AW8697_REG_BEMF_VTHH_H		0x74
#define AW8697_REG_BEMF_VTHH_L		0x75
#define AW8697_REG_BEMF_VTHL_H		0x76
#define AW8697_REG_BEMF_VTHL_L		0x77
#define AW8697_REG_BEMF_NUM		0x78
#define AW8697_BEMF_NUM_BRK		GENMASK(3, 0)
#define AW8697_REG_TIME_NZC		0x7a
#define AW8697_REG_DRV_LVL		0x7b
#define AW8697_REG_DRV_LVL_OV		0x7c

/*
 * Motor tuning of the OnePlus device_id 832 LRA (235 Hz) as used by the
 * vendor kernel.  The f0 register is 10^9 / (f0 in 0.1 Hz * 260).
 */
#define AW8697_F0_PRE_REG		(1000000000 / (2350 * 260))
#define AW8697_CONT_TD			0x006c
#define AW8697_CONT_ZC_THR		0x0ff1
#define AW8697_CONT_NUM_BRK		3
#define AW8697_CONT_DRV_LVL		105
#define AW8697_CONT_DRV_LVL_OV		125

struct aw8697 {
	struct regmap *regmap;
	struct input_dev *input;
	struct work_struct work;
	struct mutex lock;
	u16 magnitude;
};

static const struct regmap_config aw8697_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x7f,
	.cache_type = REGCACHE_NONE,
};

static int aw8697_stop(struct aw8697 *haptic)
{
	unsigned int val;
	int ret;

	regmap_clear_bits(haptic->regmap, AW8697_REG_GO, AW8697_GO);

	/* The chip finishes the braking cycle before it reports standby */
	ret = regmap_read_poll_timeout(haptic->regmap, AW8697_REG_GLB_STATE, val,
				       !(val & AW8697_GLB_STATE_MASK), 2000, 400000);
	if (ret)
		return ret;

	regmap_set_bits(haptic->regmap, AW8697_REG_SYSINTM, AW8697_SYSINTM_UVLO);
	return regmap_set_bits(haptic->regmap, AW8697_REG_SYSCTRL,
			       AW8697_SYSCTRL_STANDBY);
}

static int aw8697_start(struct aw8697 *haptic, u16 magnitude)
{
	struct regmap *map = haptic->regmap;
	unsigned int dummy;

	/* Weaker magnitudes lower the drive level, 0 stops */
	regmap_write(map, AW8697_REG_DRV_LVL,
		     max(1, AW8697_CONT_DRV_LVL * magnitude / 0xffff));
	regmap_write(map, AW8697_REG_DRV_LVL_OV, AW8697_CONT_DRV_LVL_OV);

	regmap_update_bits(map, AW8697_REG_SYSCTRL,
			   AW8697_SYSCTRL_PLAY_MODE | AW8697_SYSCTRL_BST_MODE |
			   AW8697_SYSCTRL_STANDBY,
			   FIELD_PREP(AW8697_SYSCTRL_PLAY_MODE, AW8697_PLAY_MODE_CONT));
	regmap_read(map, AW8697_REG_SYSINT, &dummy);	/* clear */
	regmap_clear_bits(map, AW8697_REG_SYSINTM, AW8697_SYSINTM_UVLO);

	regmap_write(map, AW8697_REG_F_PRE_H, AW8697_F0_PRE_REG >> 8);
	regmap_write(map, AW8697_REG_F_PRE_L, AW8697_F0_PRE_REG & 0xff);

	regmap_update_bits(map, AW8697_REG_DATCTRL,
			   AW8697_DATCTRL_FC | AW8697_DATCTRL_LPF_EN,
			   FIELD_PREP(AW8697_DATCTRL_FC, AW8697_DATCTRL_FC_1000HZ) |
			   AW8697_DATCTRL_LPF_EN);

	/* Zero-cross detection, stop on GO clear, closed loop, auto brake */
	regmap_update_bits(map, AW8697_REG_CONT_CTRL,
			   AW8697_CONT_CTRL_ZC_DETEC | AW8697_CONT_CTRL_WAIT_PERIOD |
			   AW8697_CONT_CTRL_BY_DRV_TIME | AW8697_CONT_CTRL_CLOSE_PLAYBACK |
			   AW8697_CONT_CTRL_F0_DETECT | AW8697_CONT_CTRL_O2C |
			   AW8697_CONT_CTRL_AUTO_BRK,
			   AW8697_CONT_CTRL_ZC_DETEC | AW8697_CONT_CTRL_CLOSE_PLAYBACK |
			   AW8697_CONT_CTRL_AUTO_BRK);

	regmap_write(map, AW8697_REG_TD_H, AW8697_CONT_TD >> 8);
	regmap_write(map, AW8697_REG_TD_L, AW8697_CONT_TD & 0xff);
	regmap_write(map, AW8697_REG_TSET, 0x12);
	regmap_write(map, AW8697_REG_ZC_THRSH_H, AW8697_CONT_ZC_THR >> 8);
	regmap_write(map, AW8697_REG_ZC_THRSH_L, AW8697_CONT_ZC_THR & 0xff);
	regmap_write(map, AW8697_REG_BEMF_VTHH_H, 0x10);
	regmap_write(map, AW8697_REG_BEMF_VTHH_L, 0x08);
	regmap_write(map, AW8697_REG_BEMF_VTHL_H, 0x03);
	regmap_write(map, AW8697_REG_BEMF_VTHL_L, 0xf8);
	regmap_update_bits(map, AW8697_REG_BEMF_NUM, AW8697_BEMF_NUM_BRK,
			   AW8697_CONT_NUM_BRK);
	regmap_write(map, AW8697_REG_TIME_NZC, 0x23);

	return regmap_set_bits(map, AW8697_REG_GO, AW8697_GO);
}

static void aw8697_work(struct work_struct *work)
{
	struct aw8697 *haptic = container_of(work, struct aw8697, work);
	struct device *dev = &haptic->input->dev;
	u16 magnitude = READ_ONCE(haptic->magnitude);
	int ret;

	guard(mutex)(&haptic->lock);

	ret = aw8697_stop(haptic);
	if (!ret && magnitude)
		ret = aw8697_start(haptic, magnitude);
	if (ret)
		dev_err(dev, "failed to update vibration: %d\n", ret);
}

static int aw8697_play_effect(struct input_dev *input, void *data,
			      struct ff_effect *effect)
{
	struct aw8697 *haptic = input_get_drvdata(input);

	WRITE_ONCE(haptic->magnitude, effect->u.rumble.strong_magnitude ?:
		   effect->u.rumble.weak_magnitude);
	schedule_work(&haptic->work);

	return 0;
}

static void aw8697_close(struct input_dev *input)
{
	struct aw8697 *haptic = input_get_drvdata(input);

	cancel_work_sync(&haptic->work);
	guard(mutex)(&haptic->lock);
	aw8697_stop(haptic);
}

static int aw8697_init(struct aw8697 *haptic)
{
	struct regmap *map = haptic->regmap;

	/* Register values taken from the vendor driver's initialisation */
	regmap_update_bits(map, AW8697_REG_PWMDBG, AW8697_PWMDBG_PWM_MODE,
			   FIELD_PREP(AW8697_PWMDBG_PWM_MODE, AW8697_PWM_24K));
	regmap_write(map, AW8697_REG_BSTDBG1, 0x30);
	regmap_write(map, AW8697_REG_BSTDBG2, 0xeb);
	regmap_write(map, AW8697_REG_BSTDBG3, 0xd4);
	regmap_write(map, AW8697_REG_TSET, 0x12);
	regmap_write(map, AW8697_REG_R_SPARE, 0x68);
	regmap_update_bits(map, AW8697_REG_ANADBG, AW8697_ANADBG_IOC,
			   FIELD_PREP(AW8697_ANADBG_IOC, AW8697_ANADBG_IOC_4P65A));
	regmap_update_bits(map, AW8697_REG_BSTCFG, AW8697_BSTCFG_PEAKCUR,
			   AW8697_BSTCFG_PEAKCUR_3P5A);

	/* Motor protection off, manual boost, hardware VBAT compensation */
	regmap_set_bits(map, AW8697_REG_DETCTRL, AW8697_DETCTRL_PROTECT_NO_ACT);
	regmap_clear_bits(map, AW8697_REG_PWMPRC, AW8697_PWMPRC_EN);
	regmap_clear_bits(map, AW8697_REG_PRLVL, AW8697_PRLVL_PR_EN);
	regmap_clear_bits(map, AW8697_REG_BST_AUTO, AW8697_BST_AUTO_AUTOSW);
	regmap_set_bits(map, AW8697_REG_ADCTEST, AW8697_ADCTEST_VBAT_HW_COMP);

	return aw8697_stop(haptic);
}

static int aw8697_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gpio_desc *reset;
	struct aw8697 *haptic;
	unsigned int id;
	int ret;

	haptic = devm_kzalloc(dev, sizeof(*haptic), GFP_KERNEL);
	if (!haptic)
		return -ENOMEM;

	haptic->regmap = devm_regmap_init_i2c(client, &aw8697_regmap_config);
	if (IS_ERR(haptic->regmap))
		return PTR_ERR(haptic->regmap);

	reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "failed to get reset GPIO\n");
	if (reset) {
		usleep_range(1000, 2000);
		gpiod_set_value_cansleep(reset, 0);
		usleep_range(3500, 4000);
	}

	ret = regmap_read(haptic->regmap, AW8697_REG_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip ID\n");
	if (id != AW8697_CHIPID)
		return dev_err_probe(dev, -ENODEV, "unexpected chip ID 0x%02x\n", id);

	regmap_write(haptic->regmap, AW8697_REG_ID, AW8697_SOFT_RESET);
	usleep_range(3000, 3500);

	mutex_init(&haptic->lock);
	INIT_WORK(&haptic->work, aw8697_work);

	haptic->input = devm_input_allocate_device(dev);
	if (!haptic->input)
		return -ENOMEM;

	haptic->input->name = "aw8697-haptics";
	haptic->input->close = aw8697_close;
	input_set_drvdata(haptic->input, haptic);
	input_set_capability(haptic->input, EV_FF, FF_RUMBLE);

	ret = aw8697_init(haptic);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialise the chip\n");

	ret = input_ff_create_memless(haptic->input, NULL, aw8697_play_effect);
	if (ret)
		return ret;

	return input_register_device(haptic->input);
}

static const struct of_device_id aw8697_of_match[] = {
	{ .compatible = "awinic,aw8697" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw8697_of_match);

static const struct i2c_device_id aw8697_id[] = {
	{ "aw8697" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw8697_id);

static struct i2c_driver aw8697_driver = {
	.driver = {
		.name = "aw8697-haptics",
		.of_match_table = aw8697_of_match,
	},
	.probe = aw8697_probe,
	.id_table = aw8697_id,
};
module_i2c_driver(aw8697_driver);

MODULE_DESCRIPTION("Awinic AW8697 haptic driver (rumble)");
MODULE_LICENSE("GPL");
