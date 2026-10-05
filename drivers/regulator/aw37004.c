// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW37004 camera PMIC
 * Copyright (c) 2026 Oleksii Onchul <oleksiionchul@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>

#define AW37004_ENABLE_REG	0x0e
#define AW37004_ID_REG		0x19
#define AW37004_ID		0x04

static const struct regulator_ops aw37004_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

#define AW37004_LDO(_num, _supply, _min, _step) { \
	.name = "ldo" #_num, \
	.of_match = "ldo" #_num, \
	.regulators_node = "regulators", \
	.id = (_num) - 1, \
	.supply_name = _supply, \
	.type = REGULATOR_VOLTAGE, \
	.owner = THIS_MODULE, \
	.ops = &aw37004_ops, \
	.min_uV = _min, \
	.uV_step = _step, \
	.n_voltages = 256, \
	.vsel_reg = 0x03 + (_num) - 1, \
	.vsel_mask = 0xff, \
	.enable_reg = AW37004_ENABLE_REG, \
	.enable_mask = BIT((_num) - 1), \
	.enable_time = 1000, \
}

static const struct regulator_desc aw37004_regulators[] = {
	AW37004_LDO(1, "vin1", 600000, 6000),
	AW37004_LDO(2, "vin1", 600000, 6000),
	AW37004_LDO(3, "vin2", 1200000, 12500),
	AW37004_LDO(4, "vin2", 1200000, 12500),
};

static const struct regmap_config aw37004_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x1a,
};

static int aw37004_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { .dev = dev };
	struct regulator_dev *rdev;
	struct gpio_desc *enable;
	struct regmap *regmap;
	unsigned int id;
	int ret, i;

	/* VIN2 supplies bias and the I2C interface, including for digital LDOs. */
	ret = devm_regulator_get_enable(dev, "vin2");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable bias supply\n");

	/* EN stays low: individual channels are controlled by I2C. */
	enable = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(enable))
		return dev_err_probe(dev, PTR_ERR(enable), "Failed to get enable GPIO\n");
	usleep_range(1000, 2000);
	regmap = devm_regmap_init_i2c(client, &aw37004_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "Failed to create regmap\n");
	ret = regmap_read(regmap, AW37004_ID_REG, &id);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read chip ID\n");
	if (id != AW37004_ID)
		return dev_err_probe(dev, -ENODEV, "Unexpected chip ID: %#x\n", id);

	config.regmap = regmap;
	for (i = 0; i < ARRAY_SIZE(aw37004_regulators); i++) {
		rdev = devm_regulator_register(dev, &aw37004_regulators[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev),
					     "Failed to register LDO%d\n", i + 1);
	}
	return 0;
}

static const struct of_device_id aw37004_of_match[] = {
	{ .compatible = "awinic,aw37004" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw37004_of_match);

static struct i2c_driver aw37004_driver = {
	.driver = {
		.name = "aw37004",
		.of_match_table = aw37004_of_match,
	},
	.probe = aw37004_probe,
};
module_i2c_driver(aw37004_driver);

MODULE_AUTHOR("Oleksii Onchul <oleksiionchul@gmail.com>");
MODULE_DESCRIPTION("Awinic AW37004 regulator driver");
MODULE_LICENSE("GPL");
