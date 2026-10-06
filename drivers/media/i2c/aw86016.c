// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW86016 bidirectional autofocus controller
 * Copyright (c) 2026 Oleksii Onchul <oleksiionchul@gmail.com>
 *
 * Register programming is based on the stock FroggerPro Shinetech IMX896
 * actuator configuration. The factory eNVM configuration is left unchanged.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-async.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#define AW86016_POSITION		CCI_REG16(0x03)
#define AW86016_CONTROL		CCI_REG8(0x02)
#define AW86016_POWER_DOWN	0x01
#define AW86016_ACTIVE		0x02
#define AW86016_MODE		CCI_REG8(0x06)
#define AW86016_FREQUENCY	CCI_REG8(0x07)
#define AW86016_MAX_FOCUS	1023

struct aw86016 {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *focus;
	/* Serializes controls and runtime power transitions. */
	struct mutex lock;
	struct regmap *regmap;
	struct regulator *vio;
	struct regulator *vdd;
	bool vio_enabled;
	bool vdd_enabled;
};

static struct aw86016 *sd_to_aw86016(struct v4l2_subdev *sd)
{
	return container_of(sd, struct aw86016, sd);
}

static int aw86016_set_position(struct aw86016 *aw86016, unsigned int position)
{
	return cci_write(aw86016->regmap, AW86016_POSITION,
			 position, NULL);
}

static int aw86016_init(struct aw86016 *aw86016)
{
	int ret = 0;

	/* Preserve the stock power-down, damping and initial-position sequence. */
	cci_write(aw86016->regmap, AW86016_CONTROL, AW86016_POWER_DOWN, &ret);
	if (ret)
		return ret;
	fsleep(1000);

	cci_write(aw86016->regmap, AW86016_CONTROL, AW86016_ACTIVE, &ret);
	cci_write(aw86016->regmap, AW86016_MODE, 0x40, &ret);
	cci_write(aw86016->regmap, AW86016_FREQUENCY, 0x78, &ret);
	if (ret)
		return ret;
	fsleep(1000);

	cci_write(aw86016->regmap, CCI_REG8(0x03), 0x02, &ret);
	cci_write(aw86016->regmap, CCI_REG8(0x04), 0x00, &ret);
	if (ret)
		return ret;
	fsleep(1000);

	/* Stock resume command and settling time. */
	cci_write(aw86016->regmap, AW86016_CONTROL, AW86016_ACTIVE, &ret);
	if (ret)
		return ret;
	fsleep(3000);

	return aw86016_set_position(aw86016, aw86016->focus->val);
}

static void aw86016_disable_supplies(struct aw86016 *aw86016)
{
	if (aw86016->vdd_enabled) {
		if (regulator_disable(aw86016->vdd))
			dev_err(aw86016->sd.dev, "failed to disable VDD\n");
		else
			aw86016->vdd_enabled = false;
		fsleep(1000);
	}
	if (aw86016->vio_enabled) {
		if (regulator_disable(aw86016->vio))
			dev_err(aw86016->sd.dev, "failed to disable VIO\n");
		else
			aw86016->vio_enabled = false;
	}
}

static int aw86016_power_on(struct aw86016 *aw86016)
{
	int ret;

	/* Keep the camera's shared I2C interface rail powered while in use. */
	if (!aw86016->vio_enabled) {
		ret = regulator_enable(aw86016->vio);
		if (ret)
			return ret;
		aw86016->vio_enabled = true;
	}

	if (!aw86016->vdd_enabled) {
		ret = regulator_enable(aw86016->vdd);
		if (ret)
			goto disable_supplies;
		aw86016->vdd_enabled = true;
	}
	/* Stock VAF power-up delay. */
	fsleep(10000);

	ret = aw86016_init(aw86016);
	if (!ret)
		return 0;

disable_supplies:
	aw86016_disable_supplies(aw86016);
	return ret;
}

static int aw86016_power_off(struct aw86016 *aw86016)
{
	struct device *dev = aw86016->sd.dev;
	int ret;

	ret = cci_write(aw86016->regmap, AW86016_CONTROL,
			AW86016_POWER_DOWN, NULL);
	if (ret)
		return ret;
	fsleep(3000);

	ret = regulator_disable(aw86016->vdd);
	if (ret)
		goto restore_focus;
	aw86016->vdd_enabled = false;
	fsleep(1000);

	ret = regulator_disable(aw86016->vio);
	if (!ret) {
		aw86016->vio_enabled = false;
		return 0;
	}

	/* A failed suspend must leave the device usable and powered. */
	if (regulator_enable(aw86016->vdd)) {
		dev_err(dev, "failed to restore VDD after suspend failure\n");
		return ret;
	}
	aw86016->vdd_enabled = true;
	fsleep(10000);
restore_focus:
	if (aw86016_init(aw86016))
		dev_err(dev, "failed to restore focus after suspend failure\n");
	return ret;
}

static int aw86016_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct aw86016 *aw86016 = container_of(ctrl->handler, struct aw86016, ctrls);
	struct device *dev = aw86016->sd.dev;
	int ret;

	if (ctrl->id != V4L2_CID_FOCUS_ABSOLUTE)
		return -EINVAL;

	/* Cache controls while suspended; resume restores the requested focus. */
	ret = pm_runtime_get_if_in_use(dev);
	if (ret <= 0)
		return ret;

	ret = aw86016_set_position(aw86016, ctrl->val);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static const struct v4l2_ctrl_ops aw86016_ctrl_ops = {
	.s_ctrl = aw86016_set_ctrl,
};

static int aw86016_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int aw86016_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_mark_last_busy(sd->dev);
	pm_runtime_put_autosuspend(sd->dev);
	return 0;
}

static const struct v4l2_subdev_internal_ops aw86016_internal_ops = {
	.open = aw86016_open,
	.close = aw86016_close,
};

static const struct v4l2_subdev_ops aw86016_ops = { };

static int aw86016_runtime_resume(struct device *dev)
{
	struct aw86016 *aw86016 = sd_to_aw86016(dev_get_drvdata(dev));
	int ret;

	mutex_lock(&aw86016->lock);
	ret = aw86016_power_on(aw86016);
	mutex_unlock(&aw86016->lock);
	return ret;
}

static int aw86016_runtime_suspend(struct device *dev)
{
	struct aw86016 *aw86016 = sd_to_aw86016(dev_get_drvdata(dev));
	int ret;

	mutex_lock(&aw86016->lock);
	ret = aw86016_power_off(aw86016);
	mutex_unlock(&aw86016->lock);
	return ret;
}

static int aw86016_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw86016 *aw86016;
	int ret;

	aw86016 = devm_kzalloc(dev, sizeof(*aw86016), GFP_KERNEL);
	if (!aw86016)
		return -ENOMEM;

	aw86016->vio = devm_regulator_get(dev, "vio");
	if (IS_ERR(aw86016->vio))
		return dev_err_probe(dev, PTR_ERR(aw86016->vio), "failed to get VIO\n");
	aw86016->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(aw86016->vdd))
		return dev_err_probe(dev, PTR_ERR(aw86016->vdd), "failed to get VDD\n");

	aw86016->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(aw86016->regmap))
		return dev_err_probe(dev, PTR_ERR(aw86016->regmap),
				     "failed to initialize CCI regmap\n");

	mutex_init(&aw86016->lock);
	v4l2_i2c_subdev_init(&aw86016->sd, client, &aw86016_ops);
	aw86016->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	aw86016->sd.internal_ops = &aw86016_internal_ops;
	aw86016->sd.entity.function = MEDIA_ENT_F_LENS;

	v4l2_ctrl_handler_init(&aw86016->ctrls, 1);
	aw86016->ctrls.lock = &aw86016->lock;
	/* Stock Shinetech IMX896 initial DAC code. */
	aw86016->focus = v4l2_ctrl_new_std(&aw86016->ctrls, &aw86016_ctrl_ops,
					   V4L2_CID_FOCUS_ABSOLUTE, 0,
					   AW86016_MAX_FOCUS, 1, 40);
	aw86016->sd.ctrl_handler = &aw86016->ctrls;
	ret = aw86016->ctrls.error;
	if (ret)
		goto free_ctrls;

	ret = media_entity_pads_init(&aw86016->sd.entity, 0, NULL);
	if (ret)
		goto free_ctrls;

	/* No verified chip-ID value: successful programming checks I2C. */
	ret = aw86016_power_on(aw86016);
	if (ret) {
		dev_err_probe(dev, ret, "failed to initialize actuator\n");
		goto clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	ret = v4l2_async_register_subdev(&aw86016->sd);
	if (ret)
		goto power_off;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return 0;

power_off:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	aw86016_power_off(aw86016);
	aw86016_disable_supplies(aw86016);
	pm_runtime_set_suspended(dev);
clean_entity:
	media_entity_cleanup(&aw86016->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(&aw86016->ctrls);
	mutex_destroy(&aw86016->lock);
	return ret;
}

static void aw86016_remove(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw86016 *aw86016 = sd_to_aw86016(i2c_get_clientdata(client));

	v4l2_async_unregister_subdev(&aw86016->sd);
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev)) {
		if (aw86016_power_off(aw86016))
			dev_err(dev, "failed to power off actuator\n");
	}
	aw86016_disable_supplies(aw86016);
	pm_runtime_set_suspended(dev);
	media_entity_cleanup(&aw86016->sd.entity);
	v4l2_ctrl_handler_free(&aw86016->ctrls);
	mutex_destroy(&aw86016->lock);
}

static const struct dev_pm_ops aw86016_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	RUNTIME_PM_OPS(aw86016_runtime_suspend, aw86016_runtime_resume, NULL)
};

static const struct of_device_id aw86016_of_match[] = {
	{ .compatible = "awinic,aw86016" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw86016_of_match);

static struct i2c_driver aw86016_driver = {
	.driver = {
		.name = "aw86016",
		.pm = pm_ptr(&aw86016_pm_ops),
		.of_match_table = aw86016_of_match,
	},
	.probe = aw86016_probe,
	.remove = aw86016_remove,
};
module_i2c_driver(aw86016_driver);

MODULE_DESCRIPTION("Awinic AW86016 bidirectional autofocus driver");
MODULE_LICENSE("GPL");
