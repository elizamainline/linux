// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dongwoon DW9827C closed-loop autofocus controller
 * Copyright (c) 2026 Oleksii Onchul <oleksiionchul@gmail.com>
 *
 * Register programming is based on the stock FroggerPro Qtech and AAC
 * S5KJN5 actuator configurations. Factory Hall/PID calibration stays in NVM.
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

#define DW9827C_POSITION		CCI_REG16(0x00)
#define DW9827C_CONTROL		CCI_REG8(0x02)
#define DW9827C_STANDBY		0x40
#define DW9827C_ACTIVE		0x00
#define DW9827C_INIT		CCI_REG8(0x7c)
#define DW9827C_MAX_FOCUS		4095
#define DW9827C_FOCUS_SHIFT	4

struct dw9827c {
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

static struct dw9827c *sd_to_dw9827c(struct v4l2_subdev *sd)
{
	return container_of(sd, struct dw9827c, sd);
}

static int dw9827c_set_position(struct dw9827c *dw9827c, unsigned int position)
{
	return cci_write(dw9827c->regmap, DW9827C_POSITION,
			 position << DW9827C_FOCUS_SHIFT, NULL);
}

static int dw9827c_init(struct dw9827c *dw9827c)
{
	int ret = 0;

	/* Stock initialization, followed by the stock wake-up sequence. */
	cci_write(dw9827c->regmap, DW9827C_INIT, 0x00, &ret);
	cci_write(dw9827c->regmap, DW9827C_CONTROL, DW9827C_STANDBY, &ret);
	cci_write(dw9827c->regmap, DW9827C_CONTROL, DW9827C_ACTIVE, &ret);
	if (ret)
		return ret;

	fsleep(3000);

	return dw9827c_set_position(dw9827c, dw9827c->focus->val);
}

static void dw9827c_disable_supplies(struct dw9827c *dw9827c)
{
	if (dw9827c->vdd_enabled) {
		if (regulator_disable(dw9827c->vdd))
			dev_err(dw9827c->sd.dev, "failed to disable VDD\n");
		else
			dw9827c->vdd_enabled = false;
		fsleep(1000);
	}
	if (dw9827c->vio_enabled) {
		if (regulator_disable(dw9827c->vio))
			dev_err(dw9827c->sd.dev, "failed to disable VIO\n");
		else
			dw9827c->vio_enabled = false;
	}
}

static int dw9827c_power_on(struct dw9827c *dw9827c)
{
	int ret;

	/* VIO precedes VAF in both stock module power sequences. */
	ret = regulator_enable(dw9827c->vio);
	if (ret)
		return ret;
	dw9827c->vio_enabled = true;
	fsleep(1000);

	ret = regulator_enable(dw9827c->vdd);
	if (ret)
		goto disable_vio;
	dw9827c->vdd_enabled = true;
	fsleep(5000);

	ret = dw9827c_init(dw9827c);
	if (!ret)
		return 0;

disable_vio:
	dw9827c_disable_supplies(dw9827c);
	return ret;
}

static int dw9827c_power_off(struct dw9827c *dw9827c)
{
	struct device *dev = dw9827c->sd.dev;
	int ret;

	ret = cci_write(dw9827c->regmap, DW9827C_CONTROL,
			DW9827C_STANDBY, NULL);
	if (ret)
		return ret;
	fsleep(3000);

	ret = regulator_disable(dw9827c->vdd);
	if (ret)
		goto restore_focus;
	dw9827c->vdd_enabled = false;
	fsleep(1000);

	ret = regulator_disable(dw9827c->vio);
	if (!ret) {
		dw9827c->vio_enabled = false;
		return 0;
	}

	/* A failed suspend must leave the device usable and powered. */
	if (regulator_enable(dw9827c->vdd)) {
		dev_err(dev, "failed to restore VDD after suspend failure\n");
		return ret;
	}
	dw9827c->vdd_enabled = true;
	fsleep(5000);
restore_focus:
	if (dw9827c_init(dw9827c))
		dev_err(dev, "failed to restore focus after suspend failure\n");
	return ret;
}

static int dw9827c_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct dw9827c *dw9827c = container_of(ctrl->handler, struct dw9827c, ctrls);
	struct device *dev = dw9827c->sd.dev;
	int ret;

	if (ctrl->id != V4L2_CID_FOCUS_ABSOLUTE)
		return -EINVAL;

	/* Cache controls while suspended; resume restores the requested focus. */
	ret = pm_runtime_get_if_in_use(dev);
	if (ret <= 0)
		return ret;

	ret = dw9827c_set_position(dw9827c, ctrl->val);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static const struct v4l2_ctrl_ops dw9827c_ctrl_ops = {
	.s_ctrl = dw9827c_set_ctrl,
};

static int dw9827c_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int dw9827c_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_mark_last_busy(sd->dev);
	pm_runtime_put_autosuspend(sd->dev);
	return 0;
}

static const struct v4l2_subdev_internal_ops dw9827c_internal_ops = {
	.open = dw9827c_open,
	.close = dw9827c_close,
};

static const struct v4l2_subdev_ops dw9827c_ops = { };

static int dw9827c_runtime_resume(struct device *dev)
{
	struct dw9827c *dw9827c = sd_to_dw9827c(dev_get_drvdata(dev));
	int ret;

	mutex_lock(&dw9827c->lock);
	ret = dw9827c_power_on(dw9827c);
	mutex_unlock(&dw9827c->lock);
	return ret;
}

static int dw9827c_runtime_suspend(struct device *dev)
{
	struct dw9827c *dw9827c = sd_to_dw9827c(dev_get_drvdata(dev));
	int ret;

	mutex_lock(&dw9827c->lock);
	ret = dw9827c_power_off(dw9827c);
	mutex_unlock(&dw9827c->lock);
	return ret;
}

static int dw9827c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct dw9827c *dw9827c;
	int ret;

	dw9827c = devm_kzalloc(dev, sizeof(*dw9827c), GFP_KERNEL);
	if (!dw9827c)
		return -ENOMEM;

	dw9827c->vio = devm_regulator_get(dev, "vio");
	if (IS_ERR(dw9827c->vio))
		return dev_err_probe(dev, PTR_ERR(dw9827c->vio), "failed to get VIO\n");
	dw9827c->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(dw9827c->vdd))
		return dev_err_probe(dev, PTR_ERR(dw9827c->vdd), "failed to get VDD\n");

	dw9827c->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(dw9827c->regmap))
		return dev_err_probe(dev, PTR_ERR(dw9827c->regmap),
				     "failed to initialize CCI regmap\n");

	mutex_init(&dw9827c->lock);
	v4l2_i2c_subdev_init(&dw9827c->sd, client, &dw9827c_ops);
	dw9827c->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	dw9827c->sd.internal_ops = &dw9827c_internal_ops;
	dw9827c->sd.entity.function = MEDIA_ENT_F_LENS;

	v4l2_ctrl_handler_init(&dw9827c->ctrls, 1);
	dw9827c->ctrls.lock = &dw9827c->lock;
	/* Initial DAC code 150 is shared by the two stock FroggerPro modules. */
	dw9827c->focus = v4l2_ctrl_new_std(&dw9827c->ctrls, &dw9827c_ctrl_ops,
					   V4L2_CID_FOCUS_ABSOLUTE, 0,
					   DW9827C_MAX_FOCUS, 1, 150);
	dw9827c->sd.ctrl_handler = &dw9827c->ctrls;
	ret = dw9827c->ctrls.error;
	if (ret)
		goto free_ctrls;

	ret = media_entity_pads_init(&dw9827c->sd.entity, 0, NULL);
	if (ret)
		goto free_ctrls;

	/* No documented chip-ID register: successful programming checks I2C. */
	ret = dw9827c_power_on(dw9827c);
	if (ret) {
		dev_err_probe(dev, ret, "failed to initialize actuator\n");
		goto clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	ret = v4l2_async_register_subdev(&dw9827c->sd);
	if (ret)
		goto power_off;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return 0;

power_off:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	dw9827c_power_off(dw9827c);
	dw9827c_disable_supplies(dw9827c);
	pm_runtime_set_suspended(dev);
clean_entity:
	media_entity_cleanup(&dw9827c->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(&dw9827c->ctrls);
	mutex_destroy(&dw9827c->lock);
	return ret;
}

static void dw9827c_remove(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct dw9827c *dw9827c = sd_to_dw9827c(i2c_get_clientdata(client));

	v4l2_async_unregister_subdev(&dw9827c->sd);
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev)) {
		if (dw9827c_power_off(dw9827c))
			dev_err(dev, "failed to power off actuator\n");
	}
	dw9827c_disable_supplies(dw9827c);
	pm_runtime_set_suspended(dev);
	media_entity_cleanup(&dw9827c->sd.entity);
	v4l2_ctrl_handler_free(&dw9827c->ctrls);
	mutex_destroy(&dw9827c->lock);
}

static const struct dev_pm_ops dw9827c_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	RUNTIME_PM_OPS(dw9827c_runtime_suspend, dw9827c_runtime_resume, NULL)
};

static const struct of_device_id dw9827c_of_match[] = {
	{ .compatible = "dongwoon,dw9827c" },
	{ }
};
MODULE_DEVICE_TABLE(of, dw9827c_of_match);

static struct i2c_driver dw9827c_driver = {
	.driver = {
		.name = "dw9827c",
		.pm = pm_ptr(&dw9827c_pm_ops),
		.of_match_table = dw9827c_of_match,
	},
	.probe = dw9827c_probe,
	.remove = dw9827c_remove,
};
module_i2c_driver(dw9827c_driver);

MODULE_DESCRIPTION("Dongwoon DW9827C closed-loop autofocus driver");
MODULE_LICENSE("GPL");
