// SPDX-License-Identifier: GPL-2.0-only
/*
 * SG Micro SGM38120 camera PMIC
 * Copyright (c) 2026 Oleksii Onchul <oleksiionchul@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

#define SGM38120_CHIP_ID_REG	0x00
#define SGM38120_CHIP_ID		0xd9
#define SGM38120_ENABLE_REG	0x03
#define SGM38120_VSYS_EN		BIT(7)
#define SGM38120_VOUT_REG(n)	(0x04 + (n) - 1)
#define SGM38120_EXTRA_VOUT_REG	0x1f

static const struct linear_range sgm38120_ldo12_ranges[] = {
	REGULATOR_LINEAR_RANGE(528000, 3, 125, 8000),
};

static const struct linear_range sgm38120_ldo346_ranges[] = {
	REGULATOR_LINEAR_RANGE(1504000, 0, 255, 8000),
};

static const struct linear_range sgm38120_ldo57_ranges[] = {
	REGULATOR_LINEAR_RANGE(1200000, 0, 0, 0),
	REGULATOR_LINEAR_RANGE(1504000, 1, 256, 8000),
};

static unsigned int sgm38120_extra_mask(struct regulator_dev *rdev)
{
	return rdev_get_id(rdev) == 4 ? BIT(0) : BIT(1);
}

static int sgm38120_ldo57_get_voltage_sel(struct regulator_dev *rdev)
{
	unsigned int val;
	int ret;

	ret = regmap_read(rdev->regmap, SGM38120_EXTRA_VOUT_REG, &val);
	if (ret)
		return ret;
	if (val & sgm38120_extra_mask(rdev))
		return 0;

	ret = regulator_get_voltage_sel_regmap(rdev);
	return ret < 0 ? ret : ret + 1;
}

static int sgm38120_ldo57_set_voltage_sel(struct regulator_dev *rdev,
					 unsigned int sel)
{
	unsigned int mask = sgm38120_extra_mask(rdev);
	int ret;

	if (sel) {
		ret = regulator_set_voltage_sel_regmap(rdev, sel - 1);
		if (ret)
			return ret;
	}

	return regmap_update_bits(rdev->regmap, SGM38120_EXTRA_VOUT_REG,
				  mask, sel ? 0 : mask);
}

static const struct regulator_ops sgm38120_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.map_voltage = regulator_map_voltage_linear_range,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

static const struct regulator_ops sgm38120_ldo57_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.map_voltage = regulator_map_voltage_linear_range,
	.get_voltage_sel = sgm38120_ldo57_get_voltage_sel,
	.set_voltage_sel = sgm38120_ldo57_set_voltage_sel,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

#define SGM38120_LDO(_num, _supply, _ranges, _count, _ops) { \
	.name = "ldo" #_num, \
	.of_match = "ldo" #_num, \
	.regulators_node = "regulators", \
	.id = (_num) - 1, \
	.supply_name = _supply, \
	.type = REGULATOR_VOLTAGE, \
	.owner = THIS_MODULE, \
	.ops = _ops, \
	.linear_ranges = _ranges, \
	.n_linear_ranges = ARRAY_SIZE(_ranges), \
	.n_voltages = _count, \
	.vsel_reg = SGM38120_VOUT_REG(_num), \
	.vsel_mask = 0xff, \
	.enable_reg = SGM38120_ENABLE_REG, \
	.enable_mask = BIT((_num) - 1), \
	.enable_time = 1000, \
}

static const struct regulator_desc sgm38120_regulators[] = {
	SGM38120_LDO(1, "vin12", sgm38120_ldo12_ranges, 126, &sgm38120_ops),
	SGM38120_LDO(2, "vin12", sgm38120_ldo12_ranges, 126, &sgm38120_ops),
	SGM38120_LDO(3, "vin34", sgm38120_ldo346_ranges, 256, &sgm38120_ops),
	SGM38120_LDO(4, "vin34", sgm38120_ldo346_ranges, 256, &sgm38120_ops),
	SGM38120_LDO(5, "vin5", sgm38120_ldo57_ranges, 257, &sgm38120_ldo57_ops),
	SGM38120_LDO(6, "vin6", sgm38120_ldo346_ranges, 256, &sgm38120_ops),
	SGM38120_LDO(7, "vin7", sgm38120_ldo57_ranges, 257, &sgm38120_ldo57_ops),
};

static const struct regmap_config sgm38120_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = SGM38120_EXTRA_VOUT_REG,
};

static void sgm38120_assert_reset(void *data)
{
	gpiod_set_value_cansleep(data, 1);
}

static int sgm38120_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { .dev = dev };
	struct gpio_desc *reset;
	struct regulator_dev *rdev;
	struct regmap *regmap;
	unsigned int id;
	int ret, i;

	reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "Failed to get reset GPIO\n");

	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(reset, 0);
	usleep_range(10000, 11000);
	ret = devm_add_action_or_reset(dev, sgm38120_assert_reset, reset);
	if (ret)
		return ret;

	regmap = devm_regmap_init_i2c(client, &sgm38120_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "Failed to create regmap\n");

	ret = regmap_read(regmap, SGM38120_CHIP_ID_REG, &id);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read chip ID\n");
	if (id != SGM38120_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "Unexpected chip ID: %#x\n", id);

	/* Keep system bias enabled while individual outputs are controlled. */
	ret = regmap_update_bits(regmap, SGM38120_ENABLE_REG,
				 SGM38120_VSYS_EN, SGM38120_VSYS_EN);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable system bias\n");

	config.regmap = regmap;
	for (i = 0; i < ARRAY_SIZE(sgm38120_regulators); i++) {
		rdev = devm_regulator_register(dev, &sgm38120_regulators[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev),
					     "Failed to register LDO%d\n", i + 1);
	}

	return 0;
}

static const struct of_device_id sgm38120_of_match[] = {
	{ .compatible = "sgmicro,sgm38120" },
	{ }
};
MODULE_DEVICE_TABLE(of, sgm38120_of_match);

static struct i2c_driver sgm38120_driver = {
	.driver = {
		.name = "sgm38120",
		.of_match_table = sgm38120_of_match,
	},
	.probe = sgm38120_probe,
};
module_i2c_driver(sgm38120_driver);

MODULE_DESCRIPTION("SG Micro SGM38120 camera PMIC regulator driver");
MODULE_AUTHOR("Oleksii Onchul <oleksiionchul@gmail.com>");
MODULE_LICENSE("GPL");
