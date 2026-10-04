// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm CSIPHY v2.2.1 - D-PHY mode
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

static u8 csiphy_get_lane_mask(struct csiphy_lanes_cfg *cfg)
{
	u8 mask = BIT(7);
	unsigned int i;

	for (i = 0; i < cfg->num_data; i++)
		mask |= BIT(cfg->data[i].pos * 2);

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
	writel(2, csiphy->base + CSIPHY_RESET);
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

static void csiphy_lanes_enable(struct csiphy_device *csiphy,
				struct csiphy_config *cfg,
				s64 link_freq, u8 lane_mask)
{
	u32 settle = 0;
	unsigned int i;

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

	for (i = 0; i < ARRAY_SIZE(dphy_regs); i++) {
		const struct csiphy_reg *reg = &dphy_regs[i];

		writel(reg->settle ? settle : reg->value,
		       csiphy->base + reg->offset);
		if (reg->delay_us)
			udelay(reg->delay_us);
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
