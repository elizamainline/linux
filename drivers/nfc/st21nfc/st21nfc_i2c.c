// SPDX-License-Identifier: GPL-2.0-only
/* Raw NCI I2C transport for STMicroelectronics ST21NFC controllers. */

#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/nfc.h>
#include <linux/string.h>

#include <net/nfc/nci_core.h>

#define ST21NFC_MODE_SET_OID	0x02
#define ST21NFC_MODE_RESET_TIMEOUT_MS	500

#define ST21NFC_PROTOCOLS (NFC_PROTO_JEWEL_MASK | NFC_PROTO_MIFARE_MASK | \
			   NFC_PROTO_FELICA_MASK | NFC_PROTO_ISO14443_MASK | \
			   NFC_PROTO_ISO14443_B_MASK | NFC_PROTO_ISO15693_MASK | \
			   NFC_PROTO_NFC_DEP_MASK)

struct st21nfc_i2c {
	struct i2c_client *client;
	struct nci_dev *ndev;
	struct gpio_desc *reset;
	struct completion mode_response;
	struct completion mode_reset;
	u8 mode_status;
	bool waiting_for_mode_reset;
};

static int st21nfc_open(struct nci_dev *ndev)
{
	struct st21nfc_i2c *priv = nci_get_drvdata(ndev);

	/* Reset is active low. Release it before accepting controller IRQs. */
	gpiod_set_value_cansleep(priv->reset, 0);
	msleep(80);
	enable_irq(priv->client->irq);

	return 0;
}

static int st21nfc_close(struct nci_dev *ndev)
{
	struct st21nfc_i2c *priv = nci_get_drvdata(ndev);

	disable_irq(priv->client->irq);
	gpiod_set_value_cansleep(priv->reset, 1);

	return 0;
}

static int st21nfc_send(struct nci_dev *ndev, struct sk_buff *skb)
{
	struct st21nfc_i2c *priv = nci_get_drvdata(ndev);
	int ret;

	dev_dbg(&priv->client->dev, "TX: %*ph\n", skb->len, skb->data);
	ret = i2c_master_send(priv->client, skb->data, skb->len);
	if (ret >= 0 && ret != skb->len)
		ret = -EREMOTEIO;
	else if (ret >= 0)
		ret = 0;

	if (ret)
		kfree_skb(skb);
	else
		consume_skb(skb);
	return ret;
}

static int st21nfc_mode_rsp(struct nci_dev *ndev, struct sk_buff *skb)
{
	struct st21nfc_i2c *priv = nci_get_drvdata(ndev);

	if (!skb->len)
		return -EINVAL;

	if (READ_ONCE(priv->waiting_for_mode_reset)) {
		priv->mode_status = skb->data[0];
		complete(&priv->mode_response);
	}
	return 0;
}

static int st21nfc_mode_ntf(struct nci_dev *ndev, struct sk_buff *skb)
{
	/* Firmware trace notifications use this opcode too. */
	return 0;
}

static const struct nci_driver_ops st21nfc_prop_ops[] = {
	{
		.opcode = nci_opcode_pack(NCI_GID_PROPRIETARY,
					  ST21NFC_MODE_SET_OID),
		.rsp = st21nfc_mode_rsp,
		.ntf = st21nfc_mode_ntf,
	},
};

static int st21nfc_setup(struct nci_dev *ndev)
{
	struct st21nfc_i2c *priv = nci_get_drvdata(ndev);
	static const u8 mode_on[] = { 0x2f, 0x02, 0x02, 0x02, 0x01 };
	static const u8 core_init_v2[] = { 0x00, 0x00 };
	unsigned long response_seen;
	unsigned long reset_seen;
	int ret;

	/* ST54L requires NFC mode to be enabled after the first CORE_INIT. */
	reinit_completion(&priv->mode_response);
	reinit_completion(&priv->mode_reset);
	priv->mode_status = 0xff;
	WRITE_ONCE(priv->waiting_for_mode_reset, true);
	/*
	 * Send directly: CORE_RESET_NTF can arrive before the mode response and
	 * would otherwise complete nci_prop_cmd() as the wrong request.
	 */
	dev_dbg(&priv->client->dev, "TX: %*ph\n", (int)sizeof(mode_on), mode_on);
	ret = i2c_master_send(priv->client, mode_on, sizeof(mode_on));
	if (ret != sizeof(mode_on)) {
		if (ret >= 0)
			ret = -EREMOTEIO;
		dev_err(&priv->client->dev, "failed to send NFC mode command: %d\n", ret);
		goto out;
	}

	/*
	 * The command restarts the controller. The ST HAL retries CORE_INIT
	 * after 500 ms even if no reset notification arrives.
	 */
	response_seen = wait_for_completion_timeout(&priv->mode_response,
					msecs_to_jiffies(ST21NFC_MODE_RESET_TIMEOUT_MS));
	if (!response_seen)
		dev_warn(&priv->client->dev, "NFC mode response not received\n");
	if (response_seen && priv->mode_status != NCI_STATUS_OK) {
		ret = nci_to_errno(priv->mode_status);
		dev_err(&priv->client->dev, "NFC mode rejected: status 0x%02x\n",
			priv->mode_status);
		goto out;
	}
	reset_seen = wait_for_completion_timeout(&priv->mode_reset,
				    msecs_to_jiffies(ST21NFC_MODE_RESET_TIMEOUT_MS));
	if (!reset_seen)
		dev_warn(&priv->client->dev, "NFC mode reset notification not received\n");
	flush_workqueue(ndev->rx_wq);
	dev_dbg(&priv->client->dev, "NCI version after mode reset: 0x%02x\n",
		ndev->nci_ver);
	ret = nci_core_cmd(ndev, NCI_OP_CORE_INIT_CMD,
			   sizeof(core_init_v2), core_init_v2);
	if (ret)
		dev_err(&priv->client->dev, "CORE_INIT after NFC mode change failed: %d\n",
			ret);
out:
	WRITE_ONCE(priv->waiting_for_mode_reset, false);
	return ret;
}

static const struct nci_ops st21nfc_ops = {
	.open = st21nfc_open,
	.close = st21nfc_close,
	.send = st21nfc_send,
	.setup = st21nfc_setup,
	.prop_ops = st21nfc_prop_ops,
	.n_prop_ops = ARRAY_SIZE(st21nfc_prop_ops),
};

static irqreturn_t st21nfc_irq_thread(int irq, void *data)
{
	struct st21nfc_i2c *priv = data;
	struct i2c_client *client = priv->client;
	struct sk_buff *skb;
	u8 hdr[NCI_CTRL_HDR_SIZE];
	unsigned int i;
	int ret;

	ret = i2c_master_recv(client, hdr, sizeof(hdr));
	if (ret != sizeof(hdr)) {
		dev_err_ratelimited(&client->dev, "NCI header read failed: %d\n", ret);
		return IRQ_HANDLED;
	}

	/* The controller may pad an I2C read with idle (0x7e) bytes. */
	for (i = 0; i < sizeof(hdr) && hdr[0] == 0x7e; i++) {
		memmove(hdr, hdr + 1, sizeof(hdr) - 1);
		ret = i2c_master_recv(client, hdr + sizeof(hdr) - 1, 1);
		if (ret != 1)
			return IRQ_HANDLED;
	}
	if (hdr[0] == 0x7e)
		return IRQ_HANDLED;

	skb = alloc_skb(sizeof(hdr) + hdr[2], GFP_KERNEL);
	if (!skb)
		return IRQ_HANDLED;

	skb_put_data(skb, hdr, sizeof(hdr));
	if (hdr[2]) {
		ret = i2c_master_recv(client, skb_put(skb, hdr[2]), hdr[2]);
		if (ret != hdr[2]) {
			dev_err_ratelimited(&client->dev,
					    "NCI payload read failed: %d\n", ret);
			kfree_skb(skb);
			return IRQ_HANDLED;
		}
	}

	dev_dbg(&client->dev, "RX: %*ph\n", skb->len, skb->data);
	nci_recv_frame(priv->ndev, skb);
	if (hdr[0] == 0x60 && hdr[1] == 0x00 &&
	    READ_ONCE(priv->waiting_for_mode_reset))
		complete(&priv->mode_reset);
	return IRQ_HANDLED;
}

static int st21nfc_probe(struct i2c_client *client)
{
	struct st21nfc_i2c *priv;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	init_completion(&priv->mode_response);
	init_completion(&priv->mode_reset);
	priv->reset = devm_gpiod_get(&client->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset))
		return dev_err_probe(&client->dev, PTR_ERR(priv->reset),
				     "failed to get reset GPIO\n");

	priv->ndev = nci_allocate_device(&st21nfc_ops, ST21NFC_PROTOCOLS, 0, 0);
	if (!priv->ndev)
		return -ENOMEM;

	nci_set_parent_dev(priv->ndev, &client->dev);
	nci_set_drvdata(priv->ndev, priv);
	i2c_set_clientdata(client, priv);

	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					st21nfc_irq_thread, IRQF_ONESHOT,
					dev_name(&client->dev), priv);
	if (ret)
		goto free_device;
	disable_irq(client->irq);

	ret = nci_register_device(priv->ndev);
	if (ret)
		goto free_irq;

	return 0;

free_irq:
	enable_irq(client->irq);
	devm_free_irq(&client->dev, client->irq, priv);
free_device:
	nci_free_device(priv->ndev);
	return ret;
}

static void st21nfc_remove(struct i2c_client *client)
{
	struct st21nfc_i2c *priv = i2c_get_clientdata(client);

	nci_unregister_device(priv->ndev);
	enable_irq(client->irq);
	devm_free_irq(&client->dev, client->irq, priv);
	nci_free_device(priv->ndev);
}

static const struct of_device_id st21nfc_of_match[] = {
	{ .compatible = "st,st21nfc" },
	{ }
};
MODULE_DEVICE_TABLE(of, st21nfc_of_match);

static struct i2c_driver st21nfc_driver = {
	.driver = {
		.name = "st21nfc_i2c",
		.of_match_table = st21nfc_of_match,
	},
	.probe = st21nfc_probe,
	.remove = st21nfc_remove,
};
module_i2c_driver(st21nfc_driver);

MODULE_DESCRIPTION("ST21NFC raw NCI I2C driver");
MODULE_LICENSE("GPL");
