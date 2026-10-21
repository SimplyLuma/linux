// SPDX-License-Identifier: GPL-2.0
/*
 * Awinic AW36404 LED driver in torch mode, as a plain LED.
 *
 * The LED current is 6.25 mA times the number of rising edges on EN after
 * a shutdown (EN low for longer than 2.5 ms); FLASH/TORCH low selects the
 * torch mode, which has no timeout. The steps a board may use are bounded
 * by awinic,min-step and awinic,max-step.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/irqflags.h>
#include <linux/leds.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define AW36404_MAX_STEPS	64
#define AW36404_PULSE_US	2	/* high and low time: 0.5 to 10 us */
#define AW36404_SHDN_US		3000	/* EN low time that resets the count */

struct aw36404 {
	struct led_classdev cdev;
	struct gpio_desc *en;
	struct gpio_desc *mode;
	struct mutex lock;
	u32 min_step;
};

static void aw36404_set_step(struct aw36404 *aw, unsigned int step)
{
	unsigned long flags;
	unsigned int i;

	gpiod_set_value_cansleep(aw->en, 0);
	usleep_range(AW36404_SHDN_US, AW36404_SHDN_US + 500);
	if (!step)
		return;

	/* Each rising edge is one step; keep the pulses inside 10 us. */
	local_irq_save(flags);
	for (i = 0; i < step; i++) {
		gpiod_set_value(aw->en, 1);
		udelay(AW36404_PULSE_US);
		if (i + 1 < step) {
			gpiod_set_value(aw->en, 0);
			udelay(AW36404_PULSE_US);
		}
	}
	local_irq_restore(flags);
	/* EN stays high: the current ramps up after 350-450 us. */
}

static int aw36404_brightness_set(struct led_classdev *cdev,
				  enum led_brightness brightness)
{
	struct aw36404 *aw = container_of(cdev, struct aw36404, cdev);

	mutex_lock(&aw->lock);
	aw36404_set_step(aw, brightness ? aw->min_step + brightness - 1 : 0);
	mutex_unlock(&aw->lock);
	return 0;
}

static int aw36404_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct led_init_data init_data = {};
	struct aw36404 *aw;
	u32 max_step;
	int ret;

	aw = devm_kzalloc(dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;

	if (device_property_read_u32(dev, "awinic,min-step", &aw->min_step) ||
	    device_property_read_u32(dev, "awinic,max-step", &max_step) ||
	    !aw->min_step || aw->min_step > max_step ||
	    max_step >= AW36404_MAX_STEPS)
		return dev_err_probe(dev, -EINVAL, "invalid step range\n");

	/* Torch mode first, then EN low: the LED starts off. */
	aw->mode = devm_gpiod_get(dev, "mode", GPIOD_OUT_LOW);
	if (IS_ERR(aw->mode))
		return dev_err_probe(dev, PTR_ERR(aw->mode), "failed to get mode gpio\n");
	aw->en = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(aw->en))
		return dev_err_probe(dev, PTR_ERR(aw->en), "failed to get enable gpio\n");
	if (gpiod_cansleep(aw->en))
		return dev_err_probe(dev, -EINVAL, "enable gpio must not sleep\n");

	ret = devm_mutex_init(dev, &aw->lock);
	if (ret)
		return ret;

	aw->cdev.max_brightness = max_step - aw->min_step + 1;
	aw->cdev.brightness_set_blocking = aw36404_brightness_set;
	init_data.fwnode = dev_fwnode(dev);
	return devm_led_classdev_register_ext(dev, &aw->cdev, &init_data);
}

static const struct of_device_id aw36404_of_match[] = {
	{ .compatible = "awinic,aw36404" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw36404_of_match);

static struct platform_driver aw36404_driver = {
	.driver = {
		.name = "leds-aw36404",
		.of_match_table = aw36404_of_match,
	},
	.probe = aw36404_probe,
};
module_platform_driver(aw36404_driver);

MODULE_DESCRIPTION("Awinic AW36404 LED driver (torch mode)");
MODULE_LICENSE("GPL");
