// SPDX-License-Identifier: GPL-2.0-only
/*
 * ICS RT6010 haptic controller
 *
 * Register programming derived from the ICSense vendor RT6010 driver.
 * Copyright (c) 2022 ICSense Semiconductor CO., LTD
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define RT6010_ID		0x6b
#define RT6010_RAM_SIZE		0x600
#define RT6010_LIST_BASE	0x400
#define RT6010_WAVE_BASE	0x420
#define RT6010_F0		1700

#define RT6010_REG_ID		0x00
#define RT6010_REG_RESET		0x02
#define RT6010_REG_RAM_CFG	0x07
#define RT6010_REG_RAM_ADDR_L	0x08
#define RT6010_REG_RAM_ADDR_H	0x09
#define RT6010_REG_RAM_DATA	0x0a
#define RT6010_REG_FIFO_AE_L	0x0c
#define RT6010_REG_FIFO_AE_H	0x0d
#define RT6010_REG_FIFO_AF_L	0x0e
#define RT6010_REG_FIFO_AF_H	0x0f
#define RT6010_REG_PLAY_MODE	0x11
#define RT6010_REG_PLAY_CTRL	0x12
#define RT6010_REG_WAVE_BASE_L	0x1c
#define RT6010_REG_WAVE_BASE_H	0x1d
#define RT6010_REG_LIST_BASE_L	0x1e
#define RT6010_REG_LIST_BASE_H	0x1f
#define RT6010_REG_GAIN		0x20
#define RT6010_REG_SYS_CFG	0x23
#define RT6010_REG_F0_L		0x2d
#define RT6010_REG_F0_H		0x2e
#define RT6010_REG_BEMF_CFG1	0x50
#define RT6010_REG_BEMF_CFG2	0x51
#define RT6010_REG_BOOST_CFG1	0x55
#define RT6010_REG_BOOST_CFG2	0x56
#define RT6010_REG_BOOST_CFG3	0x57
#define RT6010_REG_BOOST_CFG4	0x58
#define RT6010_REG_BOOST_CFG5	0x59
#define RT6010_REG_PA_CFG1	0x5a
#define RT6010_REG_PA_CFG2	0x5b
#define RT6010_REG_PMU_CFG2	0x5d
#define RT6010_REG_PMU_CFG3	0x5e
#define RT6010_REG_PMU_CFG4	0x5f
#define RT6010_REG_OSC_CFG1	0x60
#define RT6010_REG_EFS_DATA	0x67
#define RT6010_REG_EFS_INDEX	0x68
#define RT6010_REG_EFS_CTRL	0x69

struct rt6010 {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset;
	struct input_dev *input;
	struct work_struct work;
	unsigned int magnitude;
};

static const struct regmap_config rt6010_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x69,
};

static int rt6010_efuse_read(struct rt6010 *rt, unsigned int index, u8 *value)
{
	unsigned int reg;
	int ret, i;

	ret = regmap_write(rt->regmap, RT6010_REG_EFS_INDEX, index);
	if (ret)
		return ret;
	ret = regmap_write(rt->regmap, RT6010_REG_EFS_CTRL, 2);
	if (ret)
		return ret;

	for (i = 0; i < 100; i++) {
		usleep_range(10, 20);
		ret = regmap_read(rt->regmap, RT6010_REG_EFS_CTRL, &reg);
		if (ret)
			return ret;
		if (!(reg & 2)) {
			ret = regmap_read(rt->regmap, RT6010_REG_EFS_DATA, &reg);
			if (!ret)
				*value = reg;
			return ret;
		}
	}

	return -ETIMEDOUT;
}

static int rt6010_apply_trim(struct rt6010 *rt)
{
	u8 efs[4];
	u32 trim;
	unsigned int value;
	int ret, i;

	ret = regmap_write(rt->regmap, RT6010_REG_RESET, 1);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(efs); i++) {
		ret = rt6010_efuse_read(rt, i, &efs[i]);
		if (ret)
			return ret;
	}
	trim = efs[0] | (efs[1] << 8) | (efs[2] << 16) | (efs[3] << 24);

	ret = regmap_read(rt->regmap, RT6010_REG_PMU_CFG3, &value);
	if (ret)
		return ret;
	value = (value & 0x4f) | (((trim >> 9) & 1) << 7) |
		(((trim >> 8) & 1) << 4);
	ret = regmap_write(rt->regmap, RT6010_REG_PMU_CFG3, value);
	if (ret)
		return ret;
	ret = regmap_write(rt->regmap, RT6010_REG_PMU_CFG4, trim & 0xff);
	if (ret)
		return ret;
	ret = regmap_write(rt->regmap, RT6010_REG_OSC_CFG1,
			   ((trim >> 10) & 0xff) ^ 0x80);
	if (ret)
		return ret;

	/* The analog settings and the efuse version split follow the vendor driver. */
	ret = regmap_write(rt->regmap, RT6010_REG_BOOST_CFG1, 1);
	if (ret)
		return ret;
	ret = regmap_write(rt->regmap, RT6010_REG_BOOST_CFG2, 5);
	if (ret)
		return ret;
	ret = regmap_write(rt->regmap, RT6010_REG_BOOST_CFG3, 0x0a);
	if (ret)
		return ret;
	if ((trim >> 30) == 1) {
		ret = regmap_write(rt->regmap, 0x2a, 0x0f);
		if (ret)
			return ret;
		ret = regmap_write(rt->regmap, RT6010_REG_BEMF_CFG1, 0);
		if (ret)
			return ret;
		ret = regmap_write(rt->regmap, RT6010_REG_BOOST_CFG4, 0x50);
	} else {
		ret = regmap_write(rt->regmap, 0x2a, 0x0b);
		if (ret)
			return ret;
		ret = regmap_write(rt->regmap, RT6010_REG_BEMF_CFG1, 6);
		if (ret)
			return ret;
		ret = regmap_write(rt->regmap, RT6010_REG_BOOST_CFG4, 0x58);
	}
	if (ret)
		return ret;
	ret = regmap_write(rt->regmap, RT6010_REG_BOOST_CFG5, 0x1c);
	if (ret)
		return ret;
	ret = regmap_write(rt->regmap, RT6010_REG_PA_CFG1, 0x2c);
	if (ret)
		return ret;
	ret = regmap_write(rt->regmap, RT6010_REG_PA_CFG2, 3);
	if (ret)
		return ret;
	ret = regmap_write(rt->regmap, RT6010_REG_PMU_CFG2, 0x1c);
	if (ret)
		return ret;
	ret = regmap_write(rt->regmap, RT6010_REG_BEMF_CFG2, 3);
	if (ret)
		return ret;
	return regmap_write(rt->regmap, RT6010_REG_BEMF_CFG2, 1);
}

static int rt6010_load_waveform(struct rt6010 *rt, struct device *dev)
{
	const struct firmware *fw;
	int ret;
	size_t offset;

	ret = request_firmware(&fw, "haptic_config.bin", dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to load haptic_config.bin\n");
	if (!fw->size || fw->size > RT6010_RAM_SIZE - RT6010_WAVE_BASE) {
		ret = -EINVAL;
		goto out;
	}

	ret = regmap_write(rt->regmap, RT6010_REG_RAM_ADDR_H,
			   RT6010_WAVE_BASE >> 8);
	if (ret)
		goto out;
	ret = regmap_write(rt->regmap, RT6010_REG_RAM_ADDR_L,
			   RT6010_WAVE_BASE & 0xff);
	if (ret)
		goto out;

	for (offset = 0; offset < fw->size; offset += 32) {
		ret = regmap_raw_write(rt->regmap, RT6010_REG_RAM_DATA,
				       fw->data + offset,
				       min_t(size_t, 32, fw->size - offset));
		if (ret)
			break;
	}
out:
	release_firmware(fw);
	return ret;
}

static int rt6010_init(struct rt6010 *rt, struct device *dev)
{
	static const struct reg_sequence setup[] = {
		{ RT6010_REG_RAM_CFG, 0x08 },
		{ RT6010_REG_LIST_BASE_H, RT6010_LIST_BASE >> 8 },
		{ RT6010_REG_LIST_BASE_L, RT6010_LIST_BASE & 0xff },
		{ RT6010_REG_WAVE_BASE_H, RT6010_WAVE_BASE >> 8 },
		{ RT6010_REG_WAVE_BASE_L, RT6010_WAVE_BASE & 0xff },
		{ RT6010_REG_FIFO_AE_H, 0x02 },
		{ RT6010_REG_FIFO_AE_L, 0x00 },
		{ RT6010_REG_FIFO_AF_H, 0x03 },
		{ RT6010_REG_FIFO_AF_L, 0x00 },
		{ RT6010_REG_SYS_CFG, 0x06 },
		{ RT6010_REG_GAIN, 0x80 },
		{ RT6010_REG_F0_H, (1920000 / RT6010_F0) >> 8 },
		{ RT6010_REG_F0_L, (1920000 / RT6010_F0) & 0xff },
	};
	unsigned int id;
	int ret;

	ret = regmap_read(rt->regmap, RT6010_REG_ID, &id);
	if (ret)
		return ret;
	if (id != RT6010_ID)
		return dev_err_probe(dev, -ENODEV, "Unexpected chip ID %#x\n", id);

	ret = rt6010_apply_trim(rt);
	if (ret)
		return ret;
	ret = regmap_multi_reg_write(rt->regmap, setup, ARRAY_SIZE(setup));
	if (ret)
		return ret;
	return rt6010_load_waveform(rt, dev);
}

static void rt6010_play_work(struct work_struct *work)
{
	static const u8 playlist[] = { 1, 0, 0x7f, 0, 0, 0 };
	struct rt6010 *rt = container_of(work, struct rt6010, work);
	unsigned int magnitude = READ_ONCE(rt->magnitude);
	int ret;

	ret = regmap_write(rt->regmap, RT6010_REG_PLAY_CTRL, 0);
	if (ret)
		goto err;
	if (!magnitude)
		return;
	ret = regmap_write(rt->regmap, RT6010_REG_GAIN,
			   max(1U, magnitude * 0x80 / 0xffff));
	if (ret)
		goto err;
	ret = regmap_write(rt->regmap, RT6010_REG_RAM_ADDR_H,
			   RT6010_LIST_BASE >> 8);
	if (ret)
		goto err;
	ret = regmap_write(rt->regmap, RT6010_REG_RAM_ADDR_L,
			   RT6010_LIST_BASE & 0xff);
	if (ret)
		goto err;
	ret = regmap_raw_write(rt->regmap, RT6010_REG_RAM_DATA,
			       playlist, sizeof(playlist));
	if (ret)
		goto err;
	ret = regmap_write(rt->regmap, RT6010_REG_PLAY_MODE, 1);
	if (!ret)
		ret = regmap_write(rt->regmap, RT6010_REG_PLAY_CTRL, 1);
	if (!ret)
		return;
err:
	dev_err_ratelimited(rt->dev, "Failed to play haptic effect: %d\n", ret);
}

static int rt6010_play(struct input_dev *input, void *data,
		       struct ff_effect *effect)
{
	struct rt6010 *rt = input_get_drvdata(input);
	unsigned int magnitude = effect->u.rumble.strong_magnitude;

	if (!magnitude)
		magnitude = effect->u.rumble.weak_magnitude;
	WRITE_ONCE(rt->magnitude, magnitude);
	schedule_work(&rt->work);
	return 0;
}

static void rt6010_close(struct input_dev *input)
{
	struct rt6010 *rt = input_get_drvdata(input);

	cancel_work_sync(&rt->work);
	regmap_write(rt->regmap, RT6010_REG_PLAY_CTRL, 0);
}

static void rt6010_cleanup(void *data)
{
	struct rt6010 *rt = data;

	cancel_work_sync(&rt->work);
	regmap_write(rt->regmap, RT6010_REG_PLAY_CTRL, 0);
}

static int rt6010_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct rt6010 *rt;
	int ret;

	rt = devm_kzalloc(dev, sizeof(*rt), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;
	rt->dev = dev;

	rt->regmap = devm_regmap_init_i2c(client, &rt6010_regmap_config);
	if (IS_ERR(rt->regmap))
		return dev_err_probe(dev, PTR_ERR(rt->regmap),
				     "Failed to initialize regmap\n");
	rt->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(rt->reset))
		return dev_err_probe(dev, PTR_ERR(rt->reset),
				     "Failed to get reset GPIO\n");
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(rt->reset, 0);
	usleep_range(1000, 2000);

	ret = rt6010_init(rt, dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize RT6010\n");

	INIT_WORK(&rt->work, rt6010_play_work);
	ret = devm_add_action_or_reset(dev, rt6010_cleanup, rt);
	if (ret)
		return ret;

	rt->input = devm_input_allocate_device(dev);
	if (!rt->input)
		return -ENOMEM;
	rt->input->name = "ICS RT6010 haptics";
	rt->input->id.bustype = BUS_I2C;
	rt->input->close = rt6010_close;
	input_set_drvdata(rt->input, rt);
	input_set_capability(rt->input, EV_FF, FF_RUMBLE);
	ret = input_ff_create_memless(rt->input, NULL, rt6010_play);
	if (ret)
		return ret;

	return input_register_device(rt->input);
}

static const struct of_device_id rt6010_of_match[] = {
	{ .compatible = "ics,rt6010" },
	{ }
};
MODULE_DEVICE_TABLE(of, rt6010_of_match);

static struct i2c_driver rt6010_driver = {
	.driver = {
		.name = "ics-rt6010",
		.of_match_table = rt6010_of_match,
	},
	.probe = rt6010_probe,
};
module_i2c_driver(rt6010_driver);

MODULE_DESCRIPTION("ICS RT6010 haptic controller driver");
MODULE_FIRMWARE("haptic_config.bin");
MODULE_LICENSE("GPL");
