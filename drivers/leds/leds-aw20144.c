// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW20144 18x8 matrix LED driver.
 * Register definitions and sequencing follow the AW20144 datasheet and
 * Nothing's GPL-licensed drivers/leds/aw20144/leds-aw20144.c.
 */

#include <linux/bitmap.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define AW20144_NUM_CHANNELS	144
#define AW20144_MAX_BRIGHTNESS	255

#define AW20144_REG_GCR		0x00
#define AW20144_GCR_SWSEL_ALL	(7 << 4)
#define AW20144_GCR_CHIPEN	BIT(0)
#define AW20144_REG_GCCR		0x01
#define AW20144_REG_PCCR		0x29
#define AW20144_REG_RSTN		0x2f
#define AW20144_RESET		0xae
#define AW20144_CHIP_ID		0x74
#define AW20144_CHIP_ID_A2	0x71
#define AW20144_REG_PAGE		0xf0
#define AW20144_PAGE_CONTROL	0xc0
#define AW20144_PAGE_PWM		0xc1
#define AW20144_PAGE_SCALING	0xc2

struct aw20144;

struct aw20144_led {
	struct led_classdev cdev;
	struct aw20144 *chip;
	u32 channel;
};

struct aw20144 {
	struct regmap *regmap;
	struct gpio_desc *enable;
	/* Serializes page selection, LED writes and suspend state. */
	struct mutex lock;
	u8 pwm[AW20144_NUM_CHANNELS];
	u8 scaling[AW20144_NUM_CHANNELS];
	u8 global_current;
	bool suspended;
	unsigned int num_leds;
	struct aw20144_led leds[] __counted_by(num_leds);
};

static int aw20144_brightness_set(struct led_classdev *cdev,
				  enum led_brightness brightness)
{
	struct aw20144_led *led = container_of(cdev, struct aw20144_led, cdev);
	struct aw20144 *chip = led->chip;
	int ret = 0;

	mutex_lock(&chip->lock);
	if (!chip->suspended) {
		ret = regmap_write(chip->regmap, AW20144_REG_PAGE,
				   AW20144_PAGE_PWM);
		if (ret)
			goto out;

		ret = regmap_write(chip->regmap, led->channel, brightness);
		if (ret)
			goto out;
	}
	chip->pwm[led->channel] = brightness;
out:
	mutex_unlock(&chip->lock);
	return ret;
}

/* Caller holds the lock, or no LED devices have been registered yet. */
static int aw20144_init(struct aw20144 *chip)
{
	int ret;

	gpiod_set_value_cansleep(chip->enable, 1);
	usleep_range(3000, 3500);

	ret = regmap_write(chip->regmap, AW20144_REG_PAGE,
			   AW20144_PAGE_CONTROL);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, AW20144_REG_RSTN, AW20144_RESET);
	if (ret)
		return ret;
	usleep_range(2000, 2500);

	ret = regmap_write(chip->regmap, AW20144_REG_PAGE,
			   AW20144_PAGE_CONTROL);
	if (ret)
		return ret;

	/* Keep outputs disabled until the PWM and scaling banks are ready. */
	ret = regmap_write(chip->regmap, AW20144_REG_GCR,
			   AW20144_GCR_SWSEL_ALL);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, AW20144_REG_GCCR, chip->global_current);
	if (ret)
		return ret;

	/* 62.5 kHz PWM, independent per-channel brightness. */
	ret = regmap_write(chip->regmap, AW20144_REG_PCCR, 0);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, AW20144_REG_PAGE, AW20144_PAGE_PWM);
	if (ret)
		return ret;

	ret = regmap_bulk_write(chip->regmap, 0, chip->pwm, sizeof(chip->pwm));
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, AW20144_REG_PAGE, AW20144_PAGE_SCALING);
	if (ret)
		return ret;

	ret = regmap_bulk_write(chip->regmap, 0, chip->scaling,
				sizeof(chip->scaling));
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, AW20144_REG_PAGE,
			   AW20144_PAGE_CONTROL);
	if (ret)
		return ret;

	return regmap_write(chip->regmap, AW20144_REG_GCR,
			    AW20144_GCR_SWSEL_ALL | AW20144_GCR_CHIPEN);
}

static void aw20144_disable(void *data)
{
	struct aw20144 *chip = data;

	gpiod_set_value_cansleep(chip->enable, 0);
}

static const struct regmap_config aw20144_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AW20144_REG_PAGE,
};

static int aw20144_probe(struct i2c_client *client)
{
	DECLARE_BITMAP(channels, AW20144_NUM_CHANNELS) = {};
	struct device *dev = &client->dev;
	unsigned int num_leds, id, i = 0;
	struct aw20144 *chip;
	u32 global_current;
	int ret;

	num_leds = device_get_child_node_count(dev);
	if (!num_leds || num_leds > AW20144_NUM_CHANNELS)
		return dev_err_probe(dev, -EINVAL, "Invalid LED count\n");

	ret = device_property_read_u32(dev, "awinic,global-current", &global_current);
	if (ret || global_current > 255)
		return dev_err_probe(dev, -EINVAL, "Invalid global current\n");

	chip = devm_kzalloc(dev, struct_size(chip, leds, num_leds), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->num_leds = num_leds;
	chip->global_current = global_current;
	mutex_init(&chip->lock);
	i2c_set_clientdata(client, chip);

	chip->regmap = devm_regmap_init_i2c(client, &aw20144_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(dev, PTR_ERR(chip->regmap),
				     "Failed to create regmap\n");

	/* Validate the complete channel map before touching the controller. */
	device_for_each_child_node_scoped(dev, child) {
		struct aw20144_led *led = &chip->leds[i++];

		ret = fwnode_property_read_u32(child, "reg", &led->channel);
		if (ret || led->channel >= AW20144_NUM_CHANNELS)
			return dev_err_probe(dev, -EINVAL, "Invalid LED channel\n");
		if (test_and_set_bit(led->channel, channels))
			return dev_err_probe(dev, -EINVAL, "Duplicate LED channel\n");

		led->chip = chip;
		led->cdev.max_brightness = AW20144_MAX_BRIGHTNESS;
		led->cdev.brightness_set_blocking = aw20144_brightness_set;
		chip->scaling[led->channel] = 255;
	}

	chip->enable = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(chip->enable))
		return dev_err_probe(dev, PTR_ERR(chip->enable),
				     "Failed to get enable GPIO\n");

	ret = devm_add_action_or_reset(dev, aw20144_disable, chip);
	if (ret)
		return ret;

	usleep_range(2000, 2500);
	gpiod_set_value_cansleep(chip->enable, 1);
	usleep_range(3000, 3500);

	ret = regmap_write(chip->regmap, AW20144_REG_PAGE,
			   AW20144_PAGE_CONTROL);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to select control page\n");

	ret = regmap_read(chip->regmap, AW20144_REG_RSTN, &id);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read chip ID\n");
	if (id != AW20144_CHIP_ID && id != AW20144_CHIP_ID_A2)
		return dev_err_probe(dev, -ENODEV, "Unexpected chip ID %#x\n", id);

	ret = aw20144_init(chip);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize controller\n");

	i = 0;
	device_for_each_child_node_scoped(dev, child) {
		struct led_init_data init_data = { .fwnode = child };

		ret = devm_led_classdev_register_ext(dev, &chip->leds[i++].cdev,
						     &init_data);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to register LED\n");
	}

	dev_info(dev, "AW20144 ID %#x, %u LEDs\n", id, num_leds);
	return 0;
}

static int aw20144_suspend(struct device *dev)
{
	struct aw20144 *chip = dev_get_drvdata(dev);

	mutex_lock(&chip->lock);
	chip->suspended = true;
	aw20144_disable(chip);
	mutex_unlock(&chip->lock);
	return 0;
}

static int aw20144_resume(struct device *dev)
{
	struct aw20144 *chip = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&chip->lock);
	ret = aw20144_init(chip);
	if (ret)
		aw20144_disable(chip);
	else
		chip->suspended = false;
	mutex_unlock(&chip->lock);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(aw20144_pm_ops, aw20144_suspend, aw20144_resume);

static void aw20144_shutdown(struct i2c_client *client)
{
	aw20144_suspend(&client->dev);
}

static const struct of_device_id aw20144_of_match[] = {
	{ .compatible = "awinic,aw20144" },
	{}
};
MODULE_DEVICE_TABLE(of, aw20144_of_match);

static struct i2c_driver aw20144_driver = {
	.driver = {
		.name = "aw20144",
		.of_match_table = aw20144_of_match,
		.pm = pm_sleep_ptr(&aw20144_pm_ops),
	},
	.probe = aw20144_probe,
	.shutdown = aw20144_shutdown,
};
module_i2c_driver(aw20144_driver);

MODULE_DESCRIPTION("Awinic AW20144 matrix LED driver");
MODULE_LICENSE("GPL");
