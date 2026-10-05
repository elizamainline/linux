// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm CSIPHY v2.2.1 - D-PHY and three-trio C-PHY
 *
 * Copyright (c) 2023-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/math64.h>

#include "camss.h"
#include "camss-csiphy.h"

#define CSIPHY_RESET                 0x1000
#define CSIPHY_LANE_ENABLE           0x1014
#define CSIPHY_POWER_CTRL            0x1018
#define CSIPHY_COMMON_CTRL           0x101c
#define CSIPHY_IRQ_CMD               0x1028
#define CSIPHY_IRQ_MASK(n)           (0x102c + 4 * (n))
#define CSIPHY_IRQ_CLEAR(n)          (0x1058 + 4 * (n))
#define CSIPHY_IRQ_STATUS(n)         (0x10b0 + 4 * (n))
#define CSIPHY_IRQ_REGS              11

struct csiphy_reg {
	u16 offset;
	u8 value;
	u16 delay_us;
	bool settle;
};

/* Four data lanes and the dedicated clock lane, from the v2.2.1 settings. */
static const struct csiphy_reg dphy_regs[] = {
	{ 0x0e94, 0x00, 0, false },
	{ 0x0ea0, 0x00, 0, false },
	{ 0x0e90, 0x0f, 0, false },
	{ 0x0e98, 0x08, 0, false },
	{ 0x0e94, 0x07, 209, false },
	{ 0x0094, 0x00, 0, false },
	{ 0x00a0, 0x00, 0, false },
	{ 0x0090, 0x0f, 0, false },
	{ 0x0098, 0x08, 0, false },
	{ 0x0094, 0x07, 209, false },
	{ 0x0494, 0x00, 0, false },
	{ 0x04a0, 0x00, 0, false },
	{ 0x0490, 0x0f, 0, false },
	{ 0x0498, 0x08, 0, false },
	{ 0x0494, 0x07, 209, false },
	{ 0x0894, 0x00, 0, false },
	{ 0x08a0, 0x00, 0, false },
	{ 0x0890, 0x0f, 0, false },
	{ 0x0898, 0x08, 0, false },
	{ 0x0894, 0x07, 209, false },
	{ 0x0c94, 0x00, 0, false },
	{ 0x0ca0, 0x00, 0, false },
	{ 0x0c90, 0x0f, 0, false },
	{ 0x0c98, 0x08, 0, false },
	{ 0x0c94, 0x07, 209, false },
	{ 0x0e30, 0x00, 0, false },
	{ 0x0e28, 0x04, 0, false },
	{ 0x0e00, 0x80, 0, false },
	{ 0x0e0c, 0xff, 0, false },
	{ 0x0e38, 0x1f, 0, false },
	{ 0x0e2c, 0x01, 0, false },
	{ 0x0e34, 0x0f, 0, false },
	{ 0x0e1c, 0x0a, 0, false },
	{ 0x0e14, 0x80, 0, false },
	{ 0x0e3c, 0xb8, 0, false },
	{ 0x0e04, 0x2c, 0, false },
	{ 0x0e20, 0x00, 0, false },
	{ 0x0e08, 0x19, 0, true },
	{ 0x0e10, 0x72, 0, false },
	{ 0x0030, 0x00, 0, false },
	{ 0x0000, 0x8e, 0, false },
	{ 0x0038, 0xfe, 0, false },
	{ 0x002c, 0x01, 0, false },
	{ 0x0034, 0x0f, 0, false },
	{ 0x001c, 0x0a, 0, false },
	{ 0x0014, 0x50, 0, false },
	{ 0x003c, 0xb8, 0, false },
	{ 0x0004, 0x2c, 0, false },
	{ 0x0020, 0x00, 0, false },
	{ 0x0008, 0x19, 0, true },
	{ 0x0010, 0x72, 0, false },
	{ 0x0430, 0x00, 0, false },
	{ 0x0400, 0x8e, 0, false },
	{ 0x0438, 0xfe, 0, false },
	{ 0x042c, 0x01, 0, false },
	{ 0x0434, 0x0f, 0, false },
	{ 0x041c, 0x0a, 0, false },
	{ 0x0414, 0x50, 0, false },
	{ 0x043c, 0xb8, 0, false },
	{ 0x0404, 0x2c, 0, false },
	{ 0x0420, 0x00, 0, false },
	{ 0x0408, 0x19, 0, true },
	{ 0x0410, 0x72, 0, false },
	{ 0x0830, 0x00, 0, false },
	{ 0x0800, 0x8e, 0, false },
	{ 0x0838, 0xfe, 0, false },
	{ 0x082c, 0x01, 0, false },
	{ 0x0834, 0x0f, 0, false },
	{ 0x081c, 0x0a, 0, false },
	{ 0x0814, 0x50, 0, false },
	{ 0x083c, 0xb8, 0, false },
	{ 0x0804, 0x2c, 0, false },
	{ 0x0820, 0x00, 0, false },
	{ 0x0808, 0x19, 0, true },
	{ 0x0810, 0x72, 0, false },
	{ 0x0c30, 0x00, 0, false },
	{ 0x0c00, 0x8e, 0, false },
	{ 0x0c38, 0xfe, 0, false },
	{ 0x0c2c, 0x01, 0, false },
	{ 0x0c34, 0x0f, 0, false },
	{ 0x0c1c, 0x0a, 0, false },
	{ 0x0c14, 0x50, 0, false },
	{ 0x0c3c, 0xb8, 0, false },
	{ 0x0c04, 0x2c, 0, false },
	{ 0x0c20, 0x00, 0, false },
	{ 0x0c08, 0x19, 0, true },
	{ 0x0c10, 0x72, 0, false },
};

/* C-PHY mission settings from cam_csiphy_2_2_1_hwreg.h. */
static const struct csiphy_reg cphy_regs[] = {
	{ 0x0294, 0x09, 0, false },
	{ 0x02f4, 0x00, 0, false },
	{ 0x02f8, 0x00, 0, false },
	{ 0x02fc, 0x00, 0, false },
	{ 0x02f0, 0xef, 211, false },
	{ 0x0694, 0x09, 0, false },
	{ 0x06f4, 0x00, 0, false },
	{ 0x06f8, 0x00, 0, false },
	{ 0x06fc, 0x00, 0, false },
	{ 0x06f0, 0xef, 211, false },
	{ 0x0a94, 0x09, 0, false },
	{ 0x0af4, 0x00, 0, false },
	{ 0x0af8, 0x00, 0, false },
	{ 0x0afc, 0x00, 0, false },
	{ 0x0af0, 0xef, 211, false },
	{ 0x0204, 0x00, 0, false },
	{ 0x02e4, 0x00, 0, false },
	{ 0x02e8, 0x7f, 0, false },
	{ 0x02ec, 0x7f, 0, false },
	{ 0x0218, 0x3e, 0, false },
	{ 0x021c, 0x41, 0, false },
	{ 0x0220, 0x41, 0, false },
	{ 0x0224, 0x7f, 0, false },
	{ 0x0228, 0x00, 0, false },
	{ 0x022c, 0x00, 0, false },
	{ 0x0264, 0x01, 0, false },
	{ 0x0244, 0xb2, 0, false },
	{ 0x0310, 0x35, 0, false },
	{ 0x02bc, 0xd0, 0, false },
	{ 0x0254, 0x00, 0, false },
	{ 0x0240, 0x00, 0, false },
	{ 0x0260, 0xa8, 0, false },
	{ 0x0284, 0x00, 0, false },
	{ 0x0290, 0x02, 0, false },
	{ 0x0604, 0x00, 0, false },
	{ 0x06e4, 0x00, 0, false },
	{ 0x06e8, 0x7f, 0, false },
	{ 0x06ec, 0x7f, 0, false },
	{ 0x0618, 0x3e, 0, false },
	{ 0x061c, 0x41, 0, false },
	{ 0x0620, 0x41, 0, false },
	{ 0x0624, 0x7f, 0, false },
	{ 0x0628, 0x00, 0, false },
	{ 0x062c, 0x00, 0, false },
	{ 0x0664, 0x01, 0, false },
	{ 0x0644, 0xb2, 0, false },
	{ 0x0710, 0x35, 0, false },
	{ 0x06bc, 0xd0, 0, false },
	{ 0x0654, 0x00, 0, false },
	{ 0x0640, 0x00, 0, false },
	{ 0x0660, 0xa8, 0, false },
	{ 0x0684, 0x00, 0, false },
	{ 0x0690, 0x02, 0, false },
	{ 0x0a04, 0x00, 0, false },
	{ 0x0ae4, 0x00, 0, false },
	{ 0x0ae8, 0x7f, 0, false },
	{ 0x0aec, 0x7f, 0, false },
	{ 0x0a18, 0x3e, 0, false },
	{ 0x0a1c, 0x41, 0, false },
	{ 0x0a20, 0x41, 0, false },
	{ 0x0a24, 0x7f, 0, false },
	{ 0x0a28, 0x00, 0, false },
	{ 0x0a2c, 0x00, 0, false },
	{ 0x0a64, 0x01, 0, false },
	{ 0x0a44, 0xb2, 0, false },
	{ 0x0b10, 0x35, 0, false },
	{ 0x0abc, 0xd0, 0, false },
	{ 0x0a54, 0x00, 0, false },
	{ 0x0a40, 0x00, 0, false },
	{ 0x0a60, 0xa8, 0, false },
	{ 0x0a84, 0x00, 0, false },
	{ 0x0a90, 0x02, 0, false },
};

/* Short-channel AFE/CDR settings for 1.7 to 2.0 Gsymbols/s. */
static const struct csiphy_reg cphy_2gsps_regs[] = {
	{ 0x0268, 0xf1, 0, false },
	{ 0x0294, 0x01, 0, false },
	{ 0x0278, 0x2e, 0, false },
	{ 0x0288, 0x20, 0, false },
	{ 0x026c, 0x07, 0, false },
	{ 0x028c, 0x30, 0, false },
	{ 0x0270, 0x00, 0, false },
	{ 0x0274, 0x03, 0, false },
	{ 0x0668, 0xf1, 0, false },
	{ 0x0694, 0x01, 0, false },
	{ 0x0678, 0x2e, 0, false },
	{ 0x0688, 0x20, 0, false },
	{ 0x066c, 0x07, 0, false },
	{ 0x068c, 0x30, 0, false },
	{ 0x0670, 0x00, 0, false },
	{ 0x0674, 0x03, 0, false },
	{ 0x0a68, 0xf1, 0, false },
	{ 0x0a94, 0x01, 0, false },
	{ 0x0a78, 0x2e, 0, false },
	{ 0x0a88, 0x20, 0, false },
	{ 0x0a6c, 0x07, 0, false },
	{ 0x0a8c, 0x30, 0, false },
	{ 0x0a70, 0x00, 0, false },
	{ 0x0a74, 0x03, 10, false },
	{ 0x020c, 0x27, 0, false },
	{ 0x0208, 0x00, 0, false },
	{ 0x0210, 0x00, 0, false },
	{ 0x0214, 0x00, 0, false },
	{ 0x060c, 0x27, 0, false },
	{ 0x0608, 0x00, 0, false },
	{ 0x0610, 0x00, 0, false },
	{ 0x0614, 0x00, 0, false },
	{ 0x0a0c, 0x27, 0, false },
	{ 0x0a08, 0x00, 0, false },
	{ 0x0a10, 0x00, 0, false },
	{ 0x0a14, 0x00, 0, false },
};

/* Stock short-channel profile for 0.9 to 1.0 Gsymbols/s. */
static const struct csiphy_reg cphy_1gsps_regs[] = {
	{ 0x0268, 0xf1, 0, false },
	{ 0x0294, 0x01, 0, false },
	{ 0x0278, 0x58, 0, false },
	{ 0x0288, 0x20, 0, false },
	{ 0x026c, 0x05, 0, false },
	{ 0x028c, 0x30, 0, false },
	{ 0x0270, 0x00, 0, false },
	{ 0x0274, 0x03, 0, false },
	{ 0x0668, 0xf1, 0, false },
	{ 0x0694, 0x01, 0, false },
	{ 0x0678, 0x58, 0, false },
	{ 0x0688, 0x20, 0, false },
	{ 0x066c, 0x05, 0, false },
	{ 0x068c, 0x30, 0, false },
	{ 0x0670, 0x00, 0, false },
	{ 0x0674, 0x03, 0, false },
	{ 0x0a68, 0xf1, 0, false },
	{ 0x0a94, 0x01, 0, false },
	{ 0x0a78, 0x58, 0, false },
	{ 0x0a88, 0x20, 0, false },
	{ 0x0a6c, 0x05, 0, false },
	{ 0x0a8c, 0x30, 0, false },
	{ 0x0a70, 0x00, 0, false },
	{ 0x0a74, 0x03, 10, false },
	{ 0x020c, 0x3c, 0, false },
	{ 0x0208, 0x00, 0, false },
	{ 0x0210, 0x00, 0, false },
	{ 0x0214, 0x09, 0, false },
	{ 0x060c, 0x3c, 0, false },
	{ 0x0608, 0x00, 0, false },
	{ 0x0610, 0x00, 0, false },
	{ 0x0614, 0x09, 0, false },
	{ 0x0a0c, 0x3c, 0, false },
	{ 0x0a08, 0x00, 0, false },
	{ 0x0a10, 0x00, 0, false },
	{ 0x0a14, 0x09, 0, false },
};

static u8 csiphy_get_lane_mask(struct csiphy_lanes_cfg *cfg)
{
	u8 mask = cfg->cphy ? 0 : BIT(7);
	unsigned int i;

	for (i = 0; i < cfg->num_data; i++)
		mask |= BIT(cfg->data[i].pos * 2 + cfg->cphy);

	return mask;
}

static void csiphy_hw_version_read(struct csiphy_device *csiphy,
				   struct device *dev)
{
	/* This PHY has no revision registers matching the legacy layout. */
	dev_dbg(dev, "CSIPHY%u v2.2.1\n", csiphy->id);
}

static void csiphy_reset(struct csiphy_device *csiphy)
{
	writel(1, csiphy->base + CSIPHY_RESET);
	usleep_range(1000, 2000);
	if (csiphy->cfg.csi2 && csiphy->cfg.csi2->lane_cfg.cphy) {
		writel(0x0e, csiphy->base + CSIPHY_RESET);
		fsleep(3048);
	} else {
		writel(2, csiphy->base + CSIPHY_RESET);
	}
}

static irqreturn_t csiphy_isr(int irq, void *dev)
{
	struct csiphy_device *csiphy = dev;
	unsigned int i;

	for (i = 0; i < CSIPHY_IRQ_REGS; i++)
		writel(readl(csiphy->base + CSIPHY_IRQ_STATUS(i)),
		       csiphy->base + CSIPHY_IRQ_CLEAR(i));

	writel(1, csiphy->base + CSIPHY_IRQ_CMD);
	writel(0, csiphy->base + CSIPHY_IRQ_CMD);
	for (i = 0; i < CSIPHY_IRQ_REGS; i++)
		writel(0, csiphy->base + CSIPHY_IRQ_CLEAR(i));

	return IRQ_HANDLED;
}

static void csiphy_write_regs(struct csiphy_device *csiphy,
			      const struct csiphy_reg *regs, unsigned int nregs,
			      u32 settle)
{
	unsigned int i;

	for (i = 0; i < nregs; i++) {
		const struct csiphy_reg *reg = &regs[i];

		writel(reg->settle ? settle : reg->value,
		       csiphy->base + reg->offset);
		if (reg->delay_us)
			udelay(reg->delay_us);
	}
}

static void csiphy_lanes_enable(struct csiphy_device *csiphy,
				struct csiphy_config *cfg,
				s64 link_freq, u8 lane_mask)
{
	u32 settle = 0;
	unsigned int i;

	/* Reapply 3-phase mode after the pipeline power-up/reset sequence. */
	if (cfg->csi2->lane_cfg.cphy) {
		writel(0x0e, csiphy->base + CSIPHY_RESET);
		fsleep(3048);
	}

	if (link_freq > 0 && csiphy->timer_clk_rate) {
		u64 ui = div64_u64(1000000000000ULL, 2 * link_freq);

		settle = div64_u64((85000 + 6 * ui) * csiphy->timer_clk_rate,
				   1000000000000ULL);
		settle = settle > 10 ? min(settle - 10, 255U) : 0;
	}

	writel(0, csiphy->base + 0x1084);
	writel(0, csiphy->base + 0x108c);
	udelay(1);
	writel(0x7a, csiphy->base + CSIPHY_COMMON_CTRL);
	writel(1, csiphy->base + CSIPHY_POWER_CTRL);

	if (cfg->csi2->lane_cfg.cphy) {
		/* Apply rate settings before the common C-PHY mission table. */
		if (link_freq <= 500000000)
			csiphy_write_regs(csiphy, cphy_1gsps_regs,
					  ARRAY_SIZE(cphy_1gsps_regs), 0);
		else
			csiphy_write_regs(csiphy, cphy_2gsps_regs,
					  ARRAY_SIZE(cphy_2gsps_regs), 0);
		csiphy_write_regs(csiphy, cphy_regs, ARRAY_SIZE(cphy_regs), 0);
	} else {
		csiphy_write_regs(csiphy, dphy_regs, ARRAY_SIZE(dphy_regs), settle);
	}

	/* Keep PHY interrupts masked; capture completion comes from CSID. */
	for (i = 0; i < CSIPHY_IRQ_REGS; i++)
		writel(0, csiphy->base + CSIPHY_IRQ_MASK(i));

	writel(lane_mask, csiphy->base + CSIPHY_LANE_ENABLE);
}

static void csiphy_lanes_disable(struct csiphy_device *csiphy,
				 struct csiphy_config *cfg)
{
	writel(0, csiphy->base + CSIPHY_LANE_ENABLE);
	writel(0, csiphy->base + CSIPHY_POWER_CTRL);
}

static int csiphy_init(struct csiphy_device *csiphy)
{
	return 0;
}

const struct csiphy_hw_ops csiphy_ops_2_2_1 = {
	.get_lane_mask = csiphy_get_lane_mask,
	.hw_version_read = csiphy_hw_version_read,
	.reset = csiphy_reset,
	.lanes_enable = csiphy_lanes_enable,
	.lanes_disable = csiphy_lanes_disable,
	.isr = csiphy_isr,
	.init = csiphy_init,
};
