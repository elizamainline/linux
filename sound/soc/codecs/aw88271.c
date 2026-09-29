// SPDX-License-Identifier: GPL-2.0-only
//
// Awinic AW88271 I2S smart speaker amplifier
//
// The register configuration is supplied by the board's ACF firmware. The
// ACF parser is shared with other Awinic amplifiers, but the power sequence
// and register layout below are specific to the AW88271 (chip ID 0x2329).

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>
#include <sound/soc.h>

#include "aw88395/aw88395_device.h"

#define AW88271_ID_REG		0x00
#define AW88271_SYSST_REG	0x01
#define AW88271_SYSINTM_REG	0x03
#define AW88271_SYSCTRL_REG	0x04
#define AW88271_LPC_REG		0x59
#define AW88271_CHIP_ID		0x2329
#define AW88271_SOFT_RESET	0x55aa
#define AW88271_REG_MAX		0x7f
#define AW88271_ACF_FILE	"aw882xx_acf.bin"

#define AW88271_PWDN		BIT(0)
#define AW88271_AMPPD		BIT(1)
#define AW88271_HMUTE		BIT(8)
#define AW88271_I2C_WEN_MASK	GENMASK(11, 10)
#define AW88271_I2C_WEN	BIT(11)
#define AW88271_I2STXEN	BIT(13)
#define AW88271_ULS_HMUTE	BIT(15)
#define AW88271_LPC_DETEN	BIT(5)

#define AW88271_CLOCK_READY	(BIT(4) | BIT(0))
#define AW88271_AMP_READY	(AW88271_CLOCK_READY | BIT(9))
#define AW88271_FAULTS		(BIT(14) | BIT(11) | BIT(3) | BIT(1))

struct aw88271 {
	struct aw_device *aw_dev;
	struct regmap *regmap;
	struct gpio_desc *reset;
	struct mutex lock;
	unsigned int profile;
	unsigned int applied_profile;
	unsigned int tx_enable;
	unsigned int lpc_enable;
	bool running;
	bool configured;
};

static const struct regmap_config aw88271_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = AW88271_REG_MAX,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

static void aw88271_assert_reset(void *data)
{
	gpiod_set_value_cansleep(data, 1);
}

static int aw88271_power_down(struct aw88271 *aw)
{
	int ret;

	ret = regmap_update_bits(aw->regmap, AW88271_SYSCTRL_REG,
				 AW88271_ULS_HMUTE | AW88271_HMUTE,
				 AW88271_ULS_HMUTE | AW88271_HMUTE);
	if (ret)
		return ret;

	usleep_range(5000, 5100);
	ret = regmap_update_bits(aw->regmap, AW88271_SYSCTRL_REG,
				 AW88271_I2STXEN | AW88271_AMPPD | AW88271_PWDN,
				 AW88271_AMPPD | AW88271_PWDN);
	if (!ret)
		aw->running = false;

	return ret;
}

static int aw88271_check_status(struct aw88271 *aw, unsigned int ready)
{
	unsigned int status;
	int i, ret;

	for (i = 0; i < 10; i++) {
		ret = regmap_read(aw->regmap, AW88271_SYSST_REG, &status);
		if (ret)
			return ret;
		if (status & AW88271_FAULTS) {
			dev_err(aw->aw_dev->dev,
				"amplifier fault: SYSST=0x%04x\n", status);
			return -EIO;
		}
		if ((status & ready) == ready)
			return 0;
		usleep_range(2000, 2100);
	}

	dev_err(aw->aw_dev->dev, "amplifier not ready: SYSST=0x%04x\n", status);
	return -ETIMEDOUT;
}

static int aw88271_apply_profile(struct aw88271 *aw)
{
	struct aw_prof_desc *profile;
	struct aw_sec_data_desc *regs;
	const u8 *data;
	unsigned int addr, value;
	int i, ret;

	ret = aw88395_dev_get_prof_data(aw->aw_dev, aw->profile, &profile);
	if (ret)
		return ret;

	regs = &profile->sec_desc[AW88395_DATA_TYPE_REG];
	if (!regs->data || !regs->len || regs->len % 4)
		return -EINVAL;

	ret = regmap_write(aw->regmap, AW88271_ID_REG, AW88271_SOFT_RESET);
	if (ret)
		return ret;
	usleep_range(1000, 1100);

	data = regs->data;
	aw->tx_enable = 0;
	aw->lpc_enable = 0;
	for (i = 0; i < regs->len; i += 4) {
		addr = get_unaligned_le16(data + i);
		value = get_unaligned_le16(data + i + 2);
		if (addr > AW88271_REG_MAX)
			return -EINVAL;

		if (addr == AW88271_SYSINTM_REG)
			value = 0xffff;
		if (addr == AW88271_SYSCTRL_REG) {
			aw->tx_enable = value & AW88271_I2STXEN;
			value &= ~(AW88271_I2C_WEN_MASK | AW88271_I2STXEN);
			value |= AW88271_I2C_WEN | AW88271_PWDN |
				 AW88271_AMPPD | AW88271_HMUTE | AW88271_ULS_HMUTE;
		}
		if (addr == AW88271_LPC_REG)
			aw->lpc_enable = value & AW88271_LPC_DETEN;

		ret = regmap_write(aw->regmap, addr, value);
		if (ret)
			return ret;
	}

	aw->applied_profile = aw->profile;
	aw->configured = true;
	return 0;
}

static int aw88271_start(struct aw88271 *aw)
{
	int ret;

	if (aw->running)
		return 0;
	if (!aw->configured || aw->applied_profile != aw->profile) {
		ret = aw88271_apply_profile(aw);
		if (ret)
			return ret;
	}
	ret = regmap_update_bits(aw->regmap, AW88271_LPC_REG,
				 AW88271_LPC_DETEN, 0);
	if (ret)
		goto fail;

	ret = regmap_update_bits(aw->regmap, AW88271_SYSCTRL_REG,
				 AW88271_PWDN, 0);
	if (ret)
		goto fail;
	usleep_range(2000, 2100);

	ret = aw88271_check_status(aw, AW88271_CLOCK_READY);
	if (ret)
		goto fail;

	ret = regmap_update_bits(aw->regmap, AW88271_SYSCTRL_REG,
				 AW88271_AMPPD, 0);
	if (ret)
		goto fail;
	usleep_range(1000, 1100);

	ret = aw88271_check_status(aw, AW88271_AMP_READY);
	if (ret)
		goto fail;

	ret = regmap_update_bits(aw->regmap, AW88271_SYSCTRL_REG,
				 AW88271_I2STXEN, aw->tx_enable);
	if (ret)
		goto fail;
	ret = regmap_update_bits(aw->regmap, AW88271_SYSCTRL_REG,
				 AW88271_ULS_HMUTE | AW88271_HMUTE, 0);
	if (ret)
		goto fail;
	ret = regmap_update_bits(aw->regmap, AW88271_LPC_REG,
				 AW88271_LPC_DETEN, aw->lpc_enable);
	if (ret)
		goto fail;

	aw->running = true;
	return 0;

fail:
	aw88271_power_down(aw);
	return ret;
}

static int aw88271_playback_event(struct snd_soc_dapm_widget *widget,
				  struct snd_kcontrol *control, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(widget->dapm);
	struct aw88271 *aw = snd_soc_component_get_drvdata(component);
	int ret;

	mutex_lock(&aw->lock);
	if (event == SND_SOC_DAPM_PRE_PMU)
		ret = aw88271_start(aw);
	else
		ret = aw88271_power_down(aw);
	mutex_unlock(&aw->lock);

	return ret;
}

static const struct snd_soc_dapm_widget aw88271_widgets[] = {
	SND_SOC_DAPM_AIF_IN_E("AIF_RX", "Speaker Playback", 0,
				 SND_SOC_NOPM, 0, 0, aw88271_playback_event,
				 SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("OUT"),
};

static const struct snd_soc_dapm_route aw88271_routes[] = {
	{ "OUT", NULL, "AIF_RX" },
};

static int aw88271_profile_info(struct snd_kcontrol *control,
				struct snd_ctl_elem_info *info)
{
	struct aw88271 *aw = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));
	char *name;
	int ret;

	info->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	info->count = 1;
	info->value.enumerated.items = aw->aw_dev->prof_info.count;
	if (!info->value.enumerated.items)
		return -EINVAL;
	if (info->value.enumerated.item >= info->value.enumerated.items)
		info->value.enumerated.item = info->value.enumerated.items - 1;
	ret = aw88395_dev_get_prof_name(aw->aw_dev,
					info->value.enumerated.item, &name);
	if (ret)
		return ret;
	strscpy(info->value.enumerated.name, name);
	return 0;
}

static int aw88271_profile_get(struct snd_kcontrol *control,
			       struct snd_ctl_elem_value *value)
{
	struct aw88271 *aw = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));

	value->value.enumerated.item[0] = aw->profile;
	return 0;
}

static int aw88271_profile_put(struct snd_kcontrol *control,
			       struct snd_ctl_elem_value *value)
{
	struct aw88271 *aw = snd_soc_component_get_drvdata(snd_kcontrol_chip(control));
	unsigned int profile = value->value.enumerated.item[0];
	int ret = 0;

	if (profile >= aw->aw_dev->prof_info.count)
		return -EINVAL;

	mutex_lock(&aw->lock);
	if (aw->running)
		ret = -EBUSY;
	else if (aw->profile != profile) {
		aw->profile = profile;
		ret = 1;
	}
	mutex_unlock(&aw->lock);

	return ret;
}

static const struct snd_kcontrol_new aw88271_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Profile",
		.info = aw88271_profile_info,
		.get = aw88271_profile_get,
		.put = aw88271_profile_put,
	},
};

static int aw88271_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BC_FC ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;

	return 0;
}

static const struct snd_soc_dai_ops aw88271_dai_ops = {
	.set_fmt = aw88271_set_fmt,
};

static struct snd_soc_dai_driver aw88271_dai = {
	.name = "aw88271-aif",
	.playback = {
		.stream_name = "Speaker Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &aw88271_dai_ops,
};

static int aw88271_load_firmware(struct aw88271 *aw)
{
	const struct firmware *fw;
	struct aw_container *cfg;
	const char *name;
	char *profile_name;
	int i, ret;

	ret = device_property_read_string(aw->aw_dev->dev, "firmware-name", &name);
	if (ret)
		name = AW88271_ACF_FILE;

	ret = request_firmware(&fw, name, aw->aw_dev->dev);
	if (ret)
		return dev_err_probe(aw->aw_dev->dev, ret,
				     "unable to load %s\n", name);
	if (fw->size > INT_MAX) {
		ret = -EFBIG;
		goto release;
	}

	cfg = devm_kzalloc(aw->aw_dev->dev,
			   struct_size(cfg, data, fw->size), GFP_KERNEL);
	if (!cfg) {
		ret = -ENOMEM;
		goto release;
	}
	cfg->len = fw->size;
	memcpy(cfg->data, fw->data, fw->size);
	release_firmware(fw);

	ret = aw88395_dev_load_acf_check(aw->aw_dev, cfg);
	if (ret)
		return ret;
	ret = aw88395_dev_cfg_load(aw->aw_dev, cfg);
	if (ret)
		return ret;

	for (i = 0; i < aw->aw_dev->prof_info.count; i++) {
		ret = aw88395_dev_get_prof_name(aw->aw_dev, i, &profile_name);
		if (ret)
			return ret;
		if (!strcmp(profile_name, "Music")) {
			aw->profile = i;
			return aw88271_apply_profile(aw);
		}
	}

	return dev_err_probe(aw->aw_dev->dev, -EINVAL,
			     "no Music profile for channel %u\n", aw->aw_dev->channel);

release:
	release_firmware(fw);
	return ret;
}

static int aw88271_component_probe(struct snd_soc_component *component)
{
	struct aw88271 *aw = snd_soc_component_get_drvdata(component);

	return aw88271_load_firmware(aw);
}

static const struct snd_soc_component_driver aw88271_component = {
	.probe = aw88271_component_probe,
	.controls = aw88271_controls,
	.num_controls = ARRAY_SIZE(aw88271_controls),
	.dapm_widgets = aw88271_widgets,
	.num_dapm_widgets = ARRAY_SIZE(aw88271_widgets),
	.dapm_routes = aw88271_routes,
	.num_dapm_routes = ARRAY_SIZE(aw88271_routes),
};

static int aw88271_i2c_probe(struct i2c_client *client)
{
	struct aw88271 *aw;
	unsigned int chip_id, channel;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	aw = devm_kzalloc(&client->dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;
	aw->aw_dev = devm_kzalloc(&client->dev, sizeof(*aw->aw_dev), GFP_KERNEL);
	if (!aw->aw_dev)
		return -ENOMEM;
	mutex_init(&aw->lock);

	aw->reset = devm_gpiod_get(&client->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(aw->reset))
		return dev_err_probe(&client->dev, PTR_ERR(aw->reset),
				     "unable to get reset GPIO\n");
	usleep_range(1000, 1100);
	gpiod_set_value_cansleep(aw->reset, 0);
	usleep_range(2000, 2100);
	ret = devm_add_action_or_reset(&client->dev, aw88271_assert_reset, aw->reset);
	if (ret)
		return ret;

	aw->regmap = devm_regmap_init_i2c(client, &aw88271_regmap_config);
	if (IS_ERR(aw->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(aw->regmap),
				     "unable to create regmap\n");

	ret = regmap_read(aw->regmap, AW88271_ID_REG, &chip_id);
	if (ret)
		return dev_err_probe(&client->dev, ret, "unable to read chip ID\n");
	if (chip_id != AW88271_CHIP_ID)
		return dev_err_probe(&client->dev, -ENODEV,
				     "unexpected chip ID 0x%04x\n", chip_id);

	ret = device_property_read_u32(&client->dev, "awinic,audio-channel", &channel);
	if (ret)
		return dev_err_probe(&client->dev, ret, "missing audio channel\n");
	aw->aw_dev->dev = &client->dev;
	aw->aw_dev->i2c = client;
	aw->aw_dev->regmap = aw->regmap;
	aw->aw_dev->chip_id = chip_id;
	aw->aw_dev->channel = channel;
	aw->aw_dev->prof_info.prof_type = AW88395_DEV_NONE_TYPE_ID;
	i2c_set_clientdata(client, aw);

	return devm_snd_soc_register_component(&client->dev, &aw88271_component,
					       &aw88271_dai, 1);
}

static const struct of_device_id aw88271_of_match[] = {
	{ .compatible = "awinic,aw88271" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw88271_of_match);

static struct i2c_driver aw88271_driver = {
	.driver = {
		.name = "aw88271",
		.of_match_table = aw88271_of_match,
	},
	.probe = aw88271_i2c_probe,
};
module_i2c_driver(aw88271_driver);

MODULE_DESCRIPTION("ASoC AW88271 smart speaker amplifier driver");
MODULE_FIRMWARE(AW88271_ACF_FILE);
MODULE_LICENSE("GPL");
