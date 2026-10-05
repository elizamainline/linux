// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony IMX896 50 MP image sensor
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries
 * Copyright (c) 2026 Oleksii Onchul <oleksiionchul@gmail.com>
 *
 * Initial 19.2 MHz, three-trio C-PHY support. Register sequences come from
 * FroggerPro_shinetech_imx896_wide's stock sensor configuration.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define IMX896_REG_CHIP_ID	CCI_REG16(0x0016)
#define IMX896_CHIP_ID		0x0896
#define IMX896_REG_STREAMING	CCI_REG8(0x0100)
#define IMX896_STREAMING_ON	0x01
#define IMX896_STREAMING_OFF	0x00
#define IMX896_REG_GROUP_HOLD	CCI_REG8(0x0104)
#define IMX896_REG_VTS		CCI_REG16(0x0340)
#define IMX896_VTS_MAX		65535
#define IMX896_REG_EXPOSURE	CCI_REG16(0x0202)
#define IMX896_EXPOSURE_MIN	10
#define IMX896_EXPOSURE_DEFAULT	1000
#define IMX896_REG_AGAIN		CCI_REG16(0x0204)
/* Sony gain encoding: gain = 16384 / (16384 - register), up to 64x. */
#define IMX896_AGAIN_MIN		0
#define IMX896_AGAIN_MAX		16128
#define IMX896_REG_DGAIN		CCI_REG16(0x020e)
#define IMX896_DGAIN_MIN		256
#define IMX896_DGAIN_MAX		4095
#define IMX896_REG_TEST_PATTERN	CCI_REG16(0x0600)
#define IMX896_XVCLK_RATE	19200000
#define IMX896_NATIVE_WIDTH	8192
#define IMX896_NATIVE_HEIGHT	6144

/* Half the C-PHY symbol rate: 19.2 MHz * 1955 / 19 / 2. */
static const s64 imx896_link_freqs[] = { 987789474 };

/* Stock power-up order: 1.8 V analog, 2.8 V analog, core, I/O. */
static const char * const imx896_supply_names[] = {
	"vana1", "vana2", "vdig", "vif",
};

struct imx896_mode {
	u32 width;
	u32 height;
	u32 code;
	u32 hts;
	u32 vts_min;
	u32 vts_def;
	u32 pixel_rate;
	u32 exposure_margin;
	const struct cci_reg_sequence *regs;
	u32 num_regs;
};

static const struct cci_reg_sequence imx896_global_regs[] = {
	{ CCI_REG8(0x0136), 0x13 },
	{ CCI_REG8(0x0137), 0x33 },
	{ CCI_REG8(0x0138), 0x01 },
	{ CCI_REG8(0xf800), 0xfd },
	{ CCI_REG8(0xf801), 0xfd },
	{ CCI_REG8(0xf802), 0xd7 },
	{ CCI_REG8(0xf803), 0xe3 },
	{ CCI_REG8(0xf804), 0x55 },
	{ CCI_REG8(0xf805), 0xc0 },
	{ CCI_REG8(0xf806), 0x03 },
	{ CCI_REG8(0xf807), 0x01 },
	{ CCI_REG8(0xf808), 0x16 },
	{ CCI_REG8(0xf809), 0xfa },
	{ CCI_REG8(0xf80a), 0x82 },
	{ CCI_REG8(0xf80b), 0x00 },
	{ CCI_REG8(0xf80c), 0x55 },
	{ CCI_REG8(0xf80d), 0xc4 },
	{ CCI_REG8(0xf80e), 0x03 },
	{ CCI_REG8(0xf80f), 0x01 },
	{ CCI_REG8(0xf810), 0x1b },
	{ CCI_REG8(0xf811), 0x24 },
	{ CCI_REG8(0xf812), 0xc2 },
	{ CCI_REG8(0xf813), 0xfc },
	{ CCI_REG8(0xf814), 0x55 },
	{ CCI_REG8(0xf815), 0x90 },
	{ CCI_REG8(0xf816), 0x11 },
	{ CCI_REG8(0xf817), 0x01 },
	{ CCI_REG8(0xf818), 0x00 },
	{ CCI_REG8(0xf819), 0x00 },
	{ CCI_REG8(0xf81a), 0x51 },
	{ CCI_REG8(0xf81b), 0x00 },
	{ CCI_REG8(0xf81c), 0x55 },
	{ CCI_REG8(0xf81d), 0x94 },
	{ CCI_REG8(0xf81e), 0x11 },
	{ CCI_REG8(0xf81f), 0x01 },
	{ CCI_REG8(0xf820), 0x16 },
	{ CCI_REG8(0xf821), 0xfa },
	{ CCI_REG8(0xf822), 0x00 },
	{ CCI_REG8(0xf823), 0x00 },
	{ CCI_REG8(0xf824), 0x55 },
	{ CCI_REG8(0xf825), 0x98 },
	{ CCI_REG8(0xf826), 0x11 },
	{ CCI_REG8(0xf827), 0x01 },
	{ CCI_REG8(0xf828), 0xb4 },
	{ CCI_REG8(0xf829), 0x00 },
	{ CCI_REG8(0xf82a), 0x20 },
	{ CCI_REG8(0xf82b), 0xef },
	{ CCI_REG8(0xf82c), 0x55 },
	{ CCI_REG8(0xf82d), 0x64 },
	{ CCI_REG8(0xf82e), 0x39 },
	{ CCI_REG8(0xf82f), 0x00 },
	{ CCI_REG8(0xf830), 0x32 },
	{ CCI_REG8(0xf831), 0xc8 },
	{ CCI_REG8(0xf832), 0x17 },
	{ CCI_REG8(0xf833), 0xfa },
	{ CCI_REG8(0xf884), 0x4f },
	{ CCI_REG8(0xf885), 0x06 },
	{ CCI_REG8(0xf886), 0xac },
	{ CCI_REG8(0xf887), 0x27 },
	{ CCI_REG8(0xf888), 0xac },
	{ CCI_REG8(0xf889), 0x1d },
	{ CCI_REG8(0xf88a), 0xac },
	{ CCI_REG8(0xf88b), 0x14 },
	{ CCI_REG8(0xf88c), 0xd4 },
	{ CCI_REG8(0xf88d), 0x18 },
	{ CCI_REG8(0xf88e), 0x08 },
	{ CCI_REG8(0xf88f), 0x00 },
	{ CCI_REG8(0xf890), 0xf0 },
	{ CCI_REG8(0xf891), 0x45 },
	{ CCI_REG8(0xf892), 0x06 },
	{ CCI_REG8(0xf893), 0xd8 },
	{ CCI_REG8(0xf894), 0xd4 },
	{ CCI_REG8(0xf895), 0x21 },
	{ CCI_REG8(0xf896), 0x42 },
	{ CCI_REG8(0xf897), 0x84 },
	{ CCI_REG8(0xf898), 0x1e },
	{ CCI_REG8(0xf899), 0x44 },
	{ CCI_REG8(0xf89a), 0x29 },
	{ CCI_REG8(0xf89b), 0x14 },
	{ CCI_REG8(0xf89c), 0x90 },
	{ CCI_REG8(0xf89d), 0x0c },
	{ CCI_REG8(0xf89e), 0xd4 },
	{ CCI_REG8(0xf89f), 0x21 },
	{ CCI_REG8(0xf8a0), 0x42 },
	{ CCI_REG8(0xf8a1), 0x86 },
	{ CCI_REG8(0xf8a2), 0x1e },
	{ CCI_REG8(0xf8a3), 0x44 },
	{ CCI_REG8(0xf8a4), 0x0f },
	{ CCI_REG8(0xf8a5), 0x14 },
	{ CCI_REG8(0xf8a6), 0x80 },
	{ CCI_REG8(0xf8a7), 0x04 },
	{ CCI_REG8(0xf8a8), 0x00 },
	{ CCI_REG8(0xf8a9), 0x14 },
	{ CCI_REG8(0xf8aa), 0x00 },
	{ CCI_REG8(0xf8ab), 0x41 },
	{ CCI_REG8(0xf8ac), 0xa1 },
	{ CCI_REG8(0xf8ad), 0x50 },
	{ CCI_REG8(0xf8ae), 0xa8 },
	{ CCI_REG8(0xf8af), 0x14 },
	{ CCI_REG8(0xf8b0), 0xa8 },
	{ CCI_REG8(0xf8b1), 0x1d },
	{ CCI_REG8(0xf8b2), 0xa8 },
	{ CCI_REG8(0xf8b3), 0x27 },
	{ CCI_REG8(0xf8b4), 0xa0 },
	{ CCI_REG8(0xf8b5), 0x09 },
	{ CCI_REG8(0xf8b6), 0x4f },
	{ CCI_REG8(0xf8b7), 0x06 },
	{ CCI_REG8(0xf8b8), 0xac },
	{ CCI_REG8(0xf8b9), 0x27 },
	{ CCI_REG8(0xf8ba), 0xac },
	{ CCI_REG8(0xf8bb), 0x1d },
	{ CCI_REG8(0xf8bc), 0xac },
	{ CCI_REG8(0xf8bd), 0x14 },
	{ CCI_REG8(0xf8be), 0xd0 },
	{ CCI_REG8(0xf8bf), 0x21 },
	{ CCI_REG8(0xf8c0), 0x42 },
	{ CCI_REG8(0xf8c1), 0x84 },
	{ CCI_REG8(0xf8c2), 0x1e },
	{ CCI_REG8(0xf8c3), 0x00 },
	{ CCI_REG8(0xf8c4), 0xbe },
	{ CCI_REG8(0xf8c5), 0x54 },
	{ CCI_REG8(0xf8c6), 0xd0 },
	{ CCI_REG8(0xf8c7), 0x21 },
	{ CCI_REG8(0xf8c8), 0x42 },
	{ CCI_REG8(0xf8c9), 0x86 },
	{ CCI_REG8(0xf8ca), 0x1e },
	{ CCI_REG8(0xf8cb), 0x05 },
	{ CCI_REG8(0xf8cc), 0xd4 },
	{ CCI_REG8(0xf8cd), 0x18 },
	{ CCI_REG8(0xf8ce), 0x27 },
	{ CCI_REG8(0xf8cf), 0xf8 },
	{ CCI_REG8(0xf8d0), 0xf1 },
	{ CCI_REG8(0xf8d1), 0x41 },
	{ CCI_REG8(0xf8d2), 0x00 },
	{ CCI_REG8(0xf8d3), 0x02 },
	{ CCI_REG8(0xf8d4), 0x1a },
	{ CCI_REG8(0xf8d5), 0x40 },
	{ CCI_REG8(0xf8d6), 0xd2 },
	{ CCI_REG8(0xf8d7), 0x20 },
	{ CCI_REG8(0xf8d8), 0x44 },
	{ CCI_REG8(0xf8d9), 0x00 },
	{ CCI_REG8(0xf8da), 0x1a },
	{ CCI_REG8(0xf8db), 0x22 },
	{ CCI_REG8(0xf8dc), 0x20 },
	{ CCI_REG8(0xf8dd), 0x21 },
	{ CCI_REG8(0xf8de), 0x90 },
	{ CCI_REG8(0xf8df), 0x04 },
	{ CCI_REG8(0xf8e0), 0x80 },
	{ CCI_REG8(0xf8e1), 0x0e },
	{ CCI_REG8(0xf8e2), 0xfc },
	{ CCI_REG8(0xf8e3), 0x41 },
	{ CCI_REG8(0xf8e4), 0x7f },
	{ CCI_REG8(0xf8e5), 0xff },
	{ CCI_REG8(0xf8e6), 0xfc },
	{ CCI_REG8(0xf8e7), 0x31 },
	{ CCI_REG8(0xf8e8), 0xff },
	{ CCI_REG8(0xf8e9), 0xff },
	{ CCI_REG8(0xf8ea), 0x50 },
	{ CCI_REG8(0xf8eb), 0x00 },
	{ CCI_REG8(0xf8ec), 0x80 },
	{ CCI_REG8(0xf8ed), 0x0e },
	{ CCI_REG8(0xf8ee), 0x02 },
	{ CCI_REG8(0xf8ef), 0x21 },
	{ CCI_REG8(0xf8f0), 0x11 },
	{ CCI_REG8(0xf8f1), 0x00 },
	{ CCI_REG8(0xf8f2), 0xfa },
	{ CCI_REG8(0xf8f3), 0xe8 },
	{ CCI_REG8(0xf8f4), 0x07 },
	{ CCI_REG8(0xf8f5), 0x14 },
	{ CCI_REG8(0xf8f6), 0x00 },
	{ CCI_REG8(0xf8f7), 0x01 },
	{ CCI_REG8(0xf8f8), 0x15 },
	{ CCI_REG8(0xf8f9), 0xf0 },
	{ CCI_REG8(0xf8fa), 0x6f },
	{ CCI_REG8(0xf8fb), 0xf5 },
	{ CCI_REG8(0xf8fc), 0x28 },
	{ CCI_REG8(0xf8fd), 0x05 },
	{ CCI_REG8(0xf8fe), 0xa6 },
	{ CCI_REG8(0xf8ff), 0x13 },
	{ CCI_REG8(0xf900), 0xa5 },
	{ CCI_REG8(0xf901), 0x53 },
	{ CCI_REG8(0xf902), 0x50 },
	{ CCI_REG8(0xf903), 0x02 },
	{ CCI_REG8(0xf904), 0xfa },
	{ CCI_REG8(0xf905), 0xe9 },
	{ CCI_REG8(0xf906), 0xd0 },
	{ CCI_REG8(0xf907), 0x20 },
	{ CCI_REG8(0xf908), 0x00 },
	{ CCI_REG8(0xf909), 0x02 },
	{ CCI_REG8(0xf90a), 0x00 },
	{ CCI_REG8(0xf90b), 0x13 },
	{ CCI_REG8(0xf90c), 0x00 },
	{ CCI_REG8(0xf90d), 0x30 },
	{ CCI_REG8(0xf90e), 0x05 },
	{ CCI_REG8(0xf90f), 0x20 },
	{ CCI_REG8(0xf910), 0xb8 },
	{ CCI_REG8(0xf911), 0x07 },
	{ CCI_REG8(0xf912), 0x19 },
	{ CCI_REG8(0xf913), 0x40 },
	{ CCI_REG8(0xf914), 0x50 },
	{ CCI_REG8(0xf915), 0x01 },
	{ CCI_REG8(0xf916), 0xfa },
	{ CCI_REG8(0xf917), 0xe8 },
	{ CCI_REG8(0xf918), 0x07 },
	{ CCI_REG8(0xf919), 0x86 },
	{ CCI_REG8(0xf91a), 0x00 },
	{ CCI_REG8(0xf91b), 0x10 },
	{ CCI_REG8(0xf91c), 0x80 },
	{ CCI_REG8(0xf91d), 0x04 },
	{ CCI_REG8(0xf91e), 0x5f },
	{ CCI_REG8(0xf91f), 0xf0 },
	{ CCI_REG8(0xf920), 0xd1 },
	{ CCI_REG8(0xf921), 0x18 },
	{ CCI_REG8(0xf922), 0x26 },
	{ CCI_REG8(0xf923), 0x58 },
	{ CCI_REG8(0xf924), 0x1a },
	{ CCI_REG8(0xf925), 0x12 },
	{ CCI_REG8(0xf926), 0xd1 },
	{ CCI_REG8(0xf927), 0x18 },
	{ CCI_REG8(0xf928), 0x28 },
	{ CCI_REG8(0xf929), 0x40 },
	{ CCI_REG8(0xf92a), 0xf6 },
	{ CCI_REG8(0xf92b), 0x11 },
	{ CCI_REG8(0xf92c), 0x08 },
	{ CCI_REG8(0xf92d), 0x3a },
	{ CCI_REG8(0xf92e), 0x0f },
	{ CCI_REG8(0xf92f), 0x21 },
	{ CCI_REG8(0xf930), 0xd2 },
	{ CCI_REG8(0xf931), 0x01 },
	{ CCI_REG8(0xf932), 0x00 },
	{ CCI_REG8(0xf933), 0x00 },
	{ CCI_REG8(0xf934), 0x01 },
	{ CCI_REG8(0xf935), 0x02 },
	{ CCI_REG8(0xf936), 0x0e },
	{ CCI_REG8(0xf937), 0x03 },
	{ CCI_REG8(0xf938), 0x28 },
	{ CCI_REG8(0xf939), 0x03 },
	{ CCI_REG8(0xf93a), 0xa4 },
	{ CCI_REG8(0xf93b), 0x03 },
	{ CCI_REG8(0xf93c), 0xa5 },
	{ CCI_REG8(0xf93d), 0x23 },
	{ CCI_REG8(0xf93e), 0xfc },
	{ CCI_REG8(0xf93f), 0x42 },
	{ CCI_REG8(0xf940), 0xff },
	{ CCI_REG8(0xf941), 0xfe },
	{ CCI_REG8(0xf942), 0xfc },
	{ CCI_REG8(0xf943), 0x32 },
	{ CCI_REG8(0xf944), 0xff },
	{ CCI_REG8(0xf945), 0xff },
	{ CCI_REG8(0xf946), 0x24 },
	{ CCI_REG8(0xf947), 0x02 },
	{ CCI_REG8(0xf948), 0xa5 },
	{ CCI_REG8(0xf949), 0x30 },
	{ CCI_REG8(0xf94a), 0x13 },
	{ CCI_REG8(0xf94b), 0x00 },
	{ CCI_REG8(0xf94c), 0xf1 },
	{ CCI_REG8(0xf94d), 0x42 },
	{ CCI_REG8(0xf94e), 0x00 },
	{ CCI_REG8(0xf94f), 0x02 },
	{ CCI_REG8(0xf950), 0x02 },
	{ CCI_REG8(0xf951), 0x02 },
	{ CCI_REG8(0xf952), 0x2a },
	{ CCI_REG8(0xf953), 0x02 },
	{ CCI_REG8(0xf954), 0xa6 },
	{ CCI_REG8(0xf955), 0x00 },
	{ CCI_REG8(0xf956), 0xa5 },
	{ CCI_REG8(0xf957), 0x20 },
	{ CCI_REG8(0xf958), 0xfc },
	{ CCI_REG8(0xf959), 0x12 },
	{ CCI_REG8(0xf95a), 0x3f },
	{ CCI_REG8(0xf95b), 0xff },
	{ CCI_REG8(0xf95c), 0x22 },
	{ CCI_REG8(0xf95d), 0x02 },
	{ CCI_REG8(0xf95e), 0xa5 },
	{ CCI_REG8(0xf95f), 0x02 },
	{ CCI_REG8(0xf960), 0x0e },
	{ CCI_REG8(0xf961), 0x20 },
	{ CCI_REG8(0xf962), 0xfa },
	{ CCI_REG8(0xf963), 0x00 },
	{ CCI_REG8(0xf964), 0x00 },
	{ CCI_REG8(0xf965), 0x24 },
	{ CCI_REG8(0xf966), 0xd1 },
	{ CCI_REG8(0xf967), 0x18 },
	{ CCI_REG8(0xf968), 0x28 },
	{ CCI_REG8(0xf969), 0x0c },
	{ CCI_REG8(0xf96a), 0x3a },
	{ CCI_REG8(0xf96b), 0x18 },
	{ CCI_REG8(0xf96c), 0xd0 },
	{ CCI_REG8(0xf96d), 0x18 },
	{ CCI_REG8(0xf96e), 0x26 },
	{ CCI_REG8(0xf96f), 0x76 },
	{ CCI_REG8(0xf970), 0x1a },
	{ CCI_REG8(0xf971), 0x00 },
	{ CCI_REG8(0xf972), 0x50 },
	{ CCI_REG8(0xf973), 0x01 },
	{ CCI_REG8(0xf974), 0xfa },
	{ CCI_REG8(0xf975), 0x00 },
	{ CCI_REG8(0xf976), 0x00 },
	{ CCI_REG8(0xf977), 0x12 },
	{ CCI_REG8(0xf978), 0xd1 },
	{ CCI_REG8(0xf979), 0x18 },
	{ CCI_REG8(0xf97a), 0x28 },
	{ CCI_REG8(0xf97b), 0x0c },
	{ CCI_REG8(0xf97c), 0x3a },
	{ CCI_REG8(0xf97d), 0x1b },
	{ CCI_REG8(0xf97e), 0xa8 },
	{ CCI_REG8(0xf97f), 0x14 },
	{ CCI_REG8(0xf980), 0xa8 },
	{ CCI_REG8(0xf981), 0x1d },
	{ CCI_REG8(0xf982), 0xa8 },
	{ CCI_REG8(0xf983), 0x27 },
	{ CCI_REG8(0xf984), 0xa0 },
	{ CCI_REG8(0xf985), 0x09 },
	{ CCI_REG8(0xf986), 0x4f },
	{ CCI_REG8(0xf987), 0x86 },
	{ CCI_REG8(0xf988), 0xac },
	{ CCI_REG8(0xf989), 0x17 },
	{ CCI_REG8(0xf98a), 0x50 },
	{ CCI_REG8(0xf98b), 0x12 },
	{ CCI_REG8(0xf98c), 0x50 },
	{ CCI_REG8(0xf98d), 0x03 },
	{ CCI_REG8(0xf98e), 0xfa },
	{ CCI_REG8(0xf98f), 0xe9 },
	{ CCI_REG8(0xf990), 0x18 },
	{ CCI_REG8(0xf991), 0x76 },
	{ CCI_REG8(0xf992), 0xa8 },
	{ CCI_REG8(0xf993), 0x17 },
	{ CCI_REG8(0xf994), 0xa0 },
	{ CCI_REG8(0xf995), 0x05 },
	{ CCI_REG8(0xf996), 0x4f },
	{ CCI_REG8(0xf997), 0x06 },
	{ CCI_REG8(0xf998), 0xac },
	{ CCI_REG8(0xf999), 0x27 },
	{ CCI_REG8(0xf99a), 0xac },
	{ CCI_REG8(0xf99b), 0x1d },
	{ CCI_REG8(0xf99c), 0xac },
	{ CCI_REG8(0xf99d), 0x14 },
	{ CCI_REG8(0xf99e), 0xfa },
	{ CCI_REG8(0xf99f), 0xe8 },
	{ CCI_REG8(0xf9a0), 0xf6 },
	{ CCI_REG8(0xf9a1), 0x2c },
	{ CCI_REG8(0xf9a2), 0xd0 },
	{ CCI_REG8(0xf9a3), 0x20 },
	{ CCI_REG8(0xf9a4), 0x40 },
	{ CCI_REG8(0xf9a5), 0x4f },
	{ CCI_REG8(0xf9a6), 0x1e },
	{ CCI_REG8(0xf9a7), 0x05 },
	{ CCI_REG8(0xf9a8), 0xd4 },
	{ CCI_REG8(0xf9a9), 0x18 },
	{ CCI_REG8(0xf9aa), 0x08 },
	{ CCI_REG8(0xf9ab), 0x00 },
	{ CCI_REG8(0xf9ac), 0xf6 },
	{ CCI_REG8(0xf9ad), 0x40 },
	{ CCI_REG8(0xf9ae), 0x06 },
	{ CCI_REG8(0xf9af), 0xe7 },
	{ CCI_REG8(0xf9b0), 0xbe },
	{ CCI_REG8(0xf9b1), 0x06 },
	{ CCI_REG8(0xf9b2), 0xf0 },
	{ CCI_REG8(0xf9b3), 0x40 },
	{ CCI_REG8(0xf9b4), 0x06 },
	{ CCI_REG8(0xf9b5), 0xe8 },
	{ CCI_REG8(0xf9b6), 0x29 },
	{ CCI_REG8(0xf9b7), 0x25 },
	{ CCI_REG8(0xf9b8), 0x90 },
	{ CCI_REG8(0xf9b9), 0x2c },
	{ CCI_REG8(0xf9ba), 0x80 },
	{ CCI_REG8(0xf9bb), 0x22 },
	{ CCI_REG8(0xf9bc), 0xf6 },
	{ CCI_REG8(0xf9bd), 0x41 },
	{ CCI_REG8(0xf9be), 0x05 },
	{ CCI_REG8(0xf9bf), 0x1c },
	{ CCI_REG8(0xf9c0), 0xb8 },
	{ CCI_REG8(0xf9c1), 0x8b },
	{ CCI_REG8(0xf9c2), 0xd0 },
	{ CCI_REG8(0xf9c3), 0x18 },
	{ CCI_REG8(0xf9c4), 0x28 },
	{ CCI_REG8(0xf9c5), 0x40 },
	{ CCI_REG8(0xf9c6), 0xf0 },
	{ CCI_REG8(0xf9c7), 0x02 },
	{ CCI_REG8(0xf9c8), 0x02 },
	{ CCI_REG8(0xf9c9), 0x74 },
	{ CCI_REG8(0xf9ca), 0xf6 },
	{ CCI_REG8(0xf9cb), 0x40 },
	{ CCI_REG8(0xf9cc), 0x07 },
	{ CCI_REG8(0xf9cd), 0x10 },
	{ CCI_REG8(0xf9ce), 0x0f },
	{ CCI_REG8(0xf9cf), 0x20 },
	{ CCI_REG8(0xf9d0), 0xfa },
	{ CCI_REG8(0xf9d1), 0xe8 },
	{ CCI_REG8(0xf9d2), 0x05 },
	{ CCI_REG8(0xf9d3), 0xf4 },
	{ CCI_REG8(0xf9d4), 0x80 },
	{ CCI_REG8(0xf9d5), 0x04 },
	{ CCI_REG8(0xf9d6), 0x5f },
	{ CCI_REG8(0xf9d7), 0xf0 },
	{ CCI_REG8(0xf9d8), 0x29 },
	{ CCI_REG8(0xf9d9), 0x25 },
	{ CCI_REG8(0xf9da), 0x90 },
	{ CCI_REG8(0xf9db), 0x0a },
	{ CCI_REG8(0xf9dc), 0x50 },
	{ CCI_REG8(0xf9dd), 0x31 },
	{ CCI_REG8(0xf9de), 0xfa },
	{ CCI_REG8(0xf9df), 0xe8 },
	{ CCI_REG8(0xf9e0), 0x05 },
	{ CCI_REG8(0xf9e1), 0xe6 },
	{ CCI_REG8(0xf9e2), 0x80 },
	{ CCI_REG8(0xf9e3), 0x04 },
	{ CCI_REG8(0xf9e4), 0x09 },
	{ CCI_REG8(0xf9e5), 0x50 },
	{ CCI_REG8(0xf9e6), 0xf4 },
	{ CCI_REG8(0xf9e7), 0x40 },
	{ CCI_REG8(0xf9e8), 0x06 },
	{ CCI_REG8(0xf9e9), 0xec },
	{ CCI_REG8(0xf9ea), 0x50 },
	{ CCI_REG8(0xf9eb), 0xa1 },
	{ CCI_REG8(0xf9ec), 0xfa },
	{ CCI_REG8(0xf9ed), 0xe8 },
	{ CCI_REG8(0xf9ee), 0x05 },
	{ CCI_REG8(0xf9ef), 0xd8 },
	{ CCI_REG8(0xf9f0), 0xfa },
	{ CCI_REG8(0xf9f1), 0xe8 },
	{ CCI_REG8(0xf9f2), 0x05 },
	{ CCI_REG8(0xf9f3), 0x08 },
	{ CCI_REG8(0xf9f4), 0xa8 },
	{ CCI_REG8(0xf9f5), 0x14 },
	{ CCI_REG8(0xf9f6), 0xa8 },
	{ CCI_REG8(0xf9f7), 0x1d },
	{ CCI_REG8(0xf9f8), 0xa8 },
	{ CCI_REG8(0xf9f9), 0x27 },
	{ CCI_REG8(0xf9fa), 0xa0 },
	{ CCI_REG8(0xf9fb), 0x09 },
	{ CCI_REG8(0x97d8), 0x00 },
	{ CCI_REG8(0x97d9), 0x01 },
	{ CCI_REG8(0x97da), 0x0b },
	{ CCI_REG8(0x97db), 0x38 },
	{ CCI_REG8(0x4331), 0x01 },
	{ CCI_REG8(0x3304), 0x00 },
	{ CCI_REG8(0x33f0), 0x02 },
	{ CCI_REG8(0x33f1), 0x03 },
	{ CCI_REG8(0x0111), 0x03 },
	{ CCI_REG8(0xa71e), 0x00 },
	{ CCI_REG8(0xa722), 0x00 },
	{ CCI_REG8(0xad01), 0x0a },
	{ CCI_REG8(0xad02), 0x0a },
	{ CCI_REG8(0xad0e), 0x02 },
	{ CCI_REG8(0xae48), 0x01 },
	{ CCI_REG8(0xae49), 0x61 },
	{ CCI_REG8(0xae4a), 0x01 },
	{ CCI_REG8(0xae4b), 0xdf },
	{ CCI_REG8(0xae4c), 0x02 },
	{ CCI_REG8(0xae4d), 0xd0 },
	{ CCI_REG8(0xae4e), 0x01 },
	{ CCI_REG8(0xae4f), 0x61 },
	{ CCI_REG8(0xae50), 0x01 },
	{ CCI_REG8(0xae51), 0xdf },
	{ CCI_REG8(0xae52), 0x02 },
	{ CCI_REG8(0xae53), 0xd0 },
	{ CCI_REG8(0xaea3), 0x28 },
	{ CCI_REG8(0xaea9), 0x28 },
	{ CCI_REG8(0xaebb), 0xaa },
	{ CCI_REG8(0xaebd), 0xe9 },
	{ CCI_REG8(0xaec1), 0xaa },
	{ CCI_REG8(0xaec3), 0xe9 },
	{ CCI_REG8(0xaec9), 0x78 },
	{ CCI_REG8(0xaecf), 0x78 },
	{ CCI_REG8(0xaed3), 0x0c },
	{ CCI_REG8(0xaed5), 0x42 },
	{ CCI_REG8(0xaed9), 0x0c },
	{ CCI_REG8(0xaedb), 0x42 },
	{ CCI_REG8(0xaef7), 0x50 },
	{ CCI_REG8(0xaef9), 0x50 },
	{ CCI_REG8(0xaf5b), 0x50 },
	{ CCI_REG8(0xaf5d), 0x50 },
};

static const struct cci_reg_sequence imx896_4096x3072_regs[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x02 },
	{ CCI_REG8(0x3239), 0x00 },
	{ CCI_REG8(0x0342), 0x2e },
	{ CCI_REG8(0x0343), 0x80 },
	{ CCI_REG8(0x3850), 0x00 },
	{ CCI_REG8(0x3851), 0xa9 },
	{ CCI_REG8(0x0340), 0x0e },
	{ CCI_REG8(0x0341), 0xbe },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x1f },
	{ CCI_REG8(0x0349), 0xff },
	{ CCI_REG8(0x034a), 0x17 },
	{ CCI_REG8(0x034b), 0xff },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x00 },
	{ CCI_REG8(0x3005), 0x02 },
	{ CCI_REG8(0x3006), 0x02 },
	{ CCI_REG8(0x3140), 0x0a },
	{ CCI_REG8(0x3144), 0x00 },
	{ CCI_REG8(0x3148), 0x04 },
	{ CCI_REG8(0x31c0), 0x41 },
	{ CCI_REG8(0x31c1), 0x41 },
	{ CCI_REG8(0x3205), 0x00 },
	{ CCI_REG8(0x323c), 0x01 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x10 },
	{ CCI_REG8(0x040d), 0x00 },
	{ CCI_REG8(0x040e), 0x0c },
	{ CCI_REG8(0x040f), 0x00 },
	{ CCI_REG8(0x034c), 0x10 },
	{ CCI_REG8(0x034d), 0x00 },
	{ CCI_REG8(0x034e), 0x0c },
	{ CCI_REG8(0x034f), 0x00 },
	{ CCI_REG8(0x0301), 0x06 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0x3e },
	{ CCI_REG8(0x030b), 0x02 },
	{ CCI_REG8(0x030d), 0x13 },
	{ CCI_REG8(0x030e), 0x07 },
	{ CCI_REG8(0x030f), 0xa3 },
	{ CCI_REG8(0x3104), 0x01 },
	{ CCI_REG8(0x324c), 0x01 },
	{ CCI_REG8(0x3800), 0x01 },
	{ CCI_REG8(0x3801), 0x01 },
	{ CCI_REG8(0x3802), 0x01 },
	{ CCI_REG8(0x38a0), 0x00 },
	{ CCI_REG8(0x38a1), 0x00 },
	{ CCI_REG8(0x38a2), 0x00 },
	{ CCI_REG8(0x38a3), 0x00 },
	{ CCI_REG8(0x38a8), 0x00 },
	{ CCI_REG8(0x38a9), 0x00 },
	{ CCI_REG8(0x38aa), 0x00 },
	{ CCI_REG8(0x38ab), 0x00 },
	{ CCI_REG8(0x38d0), 0x00 },
	{ CCI_REG8(0x38d1), 0x00 },
	{ CCI_REG8(0x38d2), 0x00 },
	{ CCI_REG8(0x38d3), 0x00 },
	{ CCI_REG8(0x38e0), 0x00 },
	{ CCI_REG8(0x38e1), 0x00 },
	{ CCI_REG8(0x38e2), 0x00 },
	{ CCI_REG8(0x38e3), 0x00 },
	{ CCI_REG8(0x38e4), 0x00 },
	{ CCI_REG8(0x38e5), 0x00 },
	{ CCI_REG8(0x38e6), 0x00 },
	{ CCI_REG8(0x38e7), 0x00 },
	{ CCI_REG8(0x38e8), 0x00 },
	{ CCI_REG8(0x38e9), 0x00 },
	{ CCI_REG8(0x39ac), 0x01 },
	{ CCI_REG8(0x3b00), 0x00 },
	{ CCI_REG8(0x3b01), 0x00 },
	{ CCI_REG8(0x3b04), 0x00 },
	{ CCI_REG8(0x3b05), 0x00 },
	{ CCI_REG8(0x3b06), 0x00 },
	{ CCI_REG8(0x3b07), 0x00 },
	{ CCI_REG8(0x3b0a), 0x00 },
	{ CCI_REG8(0x3b0b), 0x00 },
	{ CCI_REG8(0x0202), 0x03 },
	{ CCI_REG8(0x0203), 0xe8 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x3107), 0x00 },
	{ CCI_REG8(0x3264), 0x00 },
	{ CCI_REG8(0x3265), 0x00 },
	{ CCI_REG8(0x3266), 0x00 },
	{ CCI_REG8(0x3267), 0x00 },
	{ CCI_REG8(0x3268), 0x00 },
	{ CCI_REG8(0x3103), 0x00 },
	{ CCI_REG8(0x3422), 0x01 },
	{ CCI_REG8(0x3423), 0xfc },
	{ CCI_REG8(0x30ac), 0x00 },
	{ CCI_REG8(0x30f6), 0x01 },
	{ CCI_REG8(0x30ad), 0x30 },
	{ CCI_REG8(0x30a4), 0x00 },
	{ CCI_REG8(0x30a6), 0x00 },
	{ CCI_REG8(0x30f2), 0x01 },
	{ CCI_REG8(0x30f3), 0x01 },
	{ CCI_REG8(0x30a5), 0x30 },
	{ CCI_REG8(0x30a7), 0x30 },
	{ CCI_REG8(0x30a2), 0x00 },
	{ CCI_REG8(0x30f1), 0x01 },
	{ CCI_REG8(0x30a3), 0x30 },
	{ CCI_REG8(0x38a0), 0x00 },
	{ CCI_REG8(0x38a1), 0x2e },
	{ CCI_REG8(0x38a2), 0x00 },
	{ CCI_REG8(0x38a3), 0x00 },
	{ CCI_REG8(0x38a8), 0x00 },
	{ CCI_REG8(0x38a9), 0x2e },
	{ CCI_REG8(0x38aa), 0x00 },
	{ CCI_REG8(0x38ab), 0x00 },
	{ CCI_REG8(0x38d0), 0x05 },
	{ CCI_REG8(0x38d1), 0xb0 },
	{ CCI_REG8(0x38d2), 0x05 },
	{ CCI_REG8(0x38d3), 0xb0 },
	{ CCI_REG8(0x3b00), 0x03 },
	{ CCI_REG8(0x3b01), 0xfa },
	{ CCI_REG8(0x3b04), 0x03 },
	{ CCI_REG8(0x3b05), 0xfa },
	{ CCI_REG8(0x3104), 0x01 },
	{ CCI_REG8(0x9cd2), 0x03 },
	{ CCI_REG8(0x9cd3), 0xe8 },
};

static const struct imx896_mode imx896_mode = {
	.width = 4096,
	.height = 3072,
	.code = MEDIA_BUS_FMT_SRGGB10_1X10,
	.hts = 11904,
	.vts_min = 3774,
	.vts_def = 3774,
	/* VT PLL: 19.2 MHz / 3 * 318 / 6 * 4 pixel clocks. */
	.pixel_rate = 1356800000,
	.exposure_margin = 64,
	.regs = imx896_4096x3072_regs,
	.num_regs = ARRAY_SIZE(imx896_4096x3072_regs),
};

struct imx896 {
	const struct imx896_mode *mode;
	struct v4l2_subdev	sd;
	struct media_pad	pad;
	struct regmap		*regmap;
	struct clk		*inclk;
	struct gpio_desc	*reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx896_supply_names)];
	unsigned int num_supplies;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*link_freq;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*exposure;
};

static inline struct imx896 *sd_to_imx896(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx896, sd);
}

static int imx896_power_on(struct device *dev)
{
	struct imx896 *imx896 = sd_to_imx896(dev_get_drvdata(dev));
	unsigned int i;
	int ret;

	for (i = 0; i < imx896->num_supplies; i++) {
		ret = regulator_enable(imx896->supplies[i].consumer);
		if (ret)
			goto err_supplies;
		usleep_range(1000, 2000);
	}

	ret = clk_prepare_enable(imx896->inclk);
	if (ret)
		goto err_supplies;
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(imx896->reset_gpio, 0);
	usleep_range(2000, 3000);

	return 0;

err_supplies:
	while (i)
		regulator_disable(imx896->supplies[--i].consumer);
	return ret;
}

static int imx896_power_off(struct device *dev)
{
	struct imx896 *imx896 = sd_to_imx896(dev_get_drvdata(dev));
	unsigned int i = imx896->num_supplies;

	gpiod_set_value_cansleep(imx896->reset_gpio, 1);
	usleep_range(1000, 2000);
	clk_disable_unprepare(imx896->inclk);
	while (i)
		regulator_disable(imx896->supplies[--i].consumer);

	return 0;
}

static int imx896_check_id(struct imx896 *imx896)
{
	struct device *dev = imx896->sd.dev;
	u64 chip_id;
	int ret;

	ret = cci_read(imx896->regmap, IMX896_REG_CHIP_ID, &chip_id, NULL);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip ID\n");

	if (chip_id != IMX896_CHIP_ID)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected chip ID 0x%04llx, expected 0x%04x\n",
				     chip_id, IMX896_CHIP_ID);

	return 0;
}

static int imx896_check_hwcfg(struct imx896 *imx896)
{
	struct device *dev = imx896->sd.dev;
	struct fwnode_handle *fwnode = dev_fwnode(dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_UNKNOWN,
	};
	unsigned long freq_bitmap;
	int ret;

	if (!fwnode)
		return -ENODEV;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return dev_err_probe(dev, -EINVAL, "no endpoint found\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse endpoint\n");

	if (bus_cfg.bus_type != V4L2_MBUS_CSI2_CPHY ||
	    bus_cfg.bus.mipi_csi2.num_data_lanes != 3) {
		ret = dev_err_probe(dev, -EINVAL, "three C-PHY trios required\n");
		goto out;
	}

	for (unsigned int i = 0; i < 3; i++) {
		if (bus_cfg.bus.mipi_csi2.data_lanes[i] != i + 1) {
			ret = dev_err_probe(dev, -EINVAL, "unsupported trio order\n");
			goto out;
		}
	}

	ret = v4l2_link_freq_to_bitmap(dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       imx896_link_freqs,
				       ARRAY_SIZE(imx896_link_freqs),
				       &freq_bitmap);

out:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int imx896_init(struct imx896 *imx896,
		       const struct imx896_mode *mode)
{
	int ret = 0;

	cci_multi_reg_write(imx896->regmap, imx896_global_regs,
			    ARRAY_SIZE(imx896_global_regs), &ret);
	cci_multi_reg_write(imx896->regmap, mode->regs, mode->num_regs, &ret);
	/* Normal readout; the mounting rotation is described by firmware. */
	cci_write(imx896->regmap, CCI_REG8(0x0101), 0, &ret);

	return ret;
}

static const char * const imx896_test_pattern_menu[] = {
	"Disabled",
	"Custom Pattern",
};

static int imx896_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx896 *imx896 =
		container_of(ctrl->handler, struct imx896, ctrl_handler);
	struct device *dev = imx896->sd.dev;
	const struct imx896_mode *mode = imx896->mode;
	u32 exposure_max;
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		exposure_max = mode->height + ctrl->val - mode->exposure_margin;
		ret = __v4l2_ctrl_modify_range(imx896->exposure,
					       imx896->exposure->minimum,
					       exposure_max,
					       imx896->exposure->step,
					       min_t(u32, exposure_max, IMX896_EXPOSURE_DEFAULT));
		if (ret)
			return ret;
	}

	if (pm_runtime_get_if_active(dev) <= 0)
		return 0;

	ret = cci_write(imx896->regmap, IMX896_REG_GROUP_HOLD, 1, NULL);
	if (ret)
		goto out_pm;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = cci_write(imx896->regmap, IMX896_REG_EXPOSURE,
				ctrl->val, NULL);
		break;

	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(imx896->regmap, IMX896_REG_AGAIN,
				ctrl->val, NULL);
		break;

	case V4L2_CID_DIGITAL_GAIN:
		ret = cci_write(imx896->regmap, IMX896_REG_DGAIN,
				ctrl->val, NULL);
		break;

	case V4L2_CID_VBLANK:
		ret = cci_write(imx896->regmap, IMX896_REG_VTS,
				mode->height + ctrl->val, NULL);
		break;

	case V4L2_CID_TEST_PATTERN:
		ret = 0;
		if (ctrl->val)
			cci_write(imx896->regmap, CCI_REG16(0xa200), 0, &ret);
		cci_write(imx896->regmap, IMX896_REG_TEST_PATTERN,
			  ctrl->val ? 5 : 0, &ret);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	/* Release hold even after a failed update. */
	{
		int release_ret;

		release_ret = cci_write(imx896->regmap, IMX896_REG_GROUP_HOLD,
					0, NULL);
		if (!ret)
			ret = release_ret;
	}

out_pm:
	pm_runtime_put_autosuspend(dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx896_ctrl_ops = {
	.s_ctrl = imx896_s_ctrl,
};

static int imx896_init_controls(struct imx896 *imx896)
{
	const struct imx896_mode *mode = imx896->mode;
	struct v4l2_ctrl_handler *hdlr = &imx896->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	u32 vblank_min, vblank_def, vblank_max, hblank;
	int ret;

	ret = v4l2_fwnode_device_parse(imx896->sd.dev, &props);
	if (ret)
		return ret;

	v4l2_ctrl_handler_init(hdlr, 10);

	imx896->link_freq =
		v4l2_ctrl_new_int_menu(hdlr, NULL, V4L2_CID_LINK_FREQ,
				       ARRAY_SIZE(imx896_link_freqs) - 1,
				       0,
				       imx896_link_freqs);

	v4l2_ctrl_new_std(hdlr, &imx896_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  mode->pixel_rate, mode->pixel_rate, 1,
			  mode->pixel_rate);

	vblank_min = mode->vts_min - mode->height;
	vblank_def = mode->vts_def - mode->height;
	vblank_max = IMX896_VTS_MAX - mode->height;
	v4l2_ctrl_new_std(hdlr, &imx896_ctrl_ops, V4L2_CID_VBLANK,
			  vblank_min, vblank_max, 1, vblank_def);

	hblank = mode->hts - mode->width;
	imx896->hblank =
		v4l2_ctrl_new_std(hdlr, NULL, V4L2_CID_HBLANK,
				  hblank, hblank, 1, hblank);

	imx896->exposure =
		v4l2_ctrl_new_std(hdlr, &imx896_ctrl_ops, V4L2_CID_EXPOSURE,
				  IMX896_EXPOSURE_MIN,
				  mode->vts_def - mode->exposure_margin,
				  1, IMX896_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std(hdlr, &imx896_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX896_AGAIN_MIN, IMX896_AGAIN_MAX,
			  1, IMX896_AGAIN_MIN);

	v4l2_ctrl_new_std(hdlr, &imx896_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  IMX896_DGAIN_MIN, IMX896_DGAIN_MAX,
			  1, IMX896_DGAIN_MIN);

	v4l2_ctrl_new_std_menu_items(hdlr, &imx896_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx896_test_pattern_menu) - 1,
				     0, 0, imx896_test_pattern_menu);

	ret = v4l2_ctrl_new_fwnode_properties(hdlr, &imx896_ctrl_ops, &props);
	if (ret) {
		v4l2_ctrl_handler_free(hdlr);
		return ret;
	}

	if (hdlr->error) {
		v4l2_ctrl_handler_free(hdlr);
		return hdlr->error;
	}

	imx896->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	imx896->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx896->sd.ctrl_handler = hdlr;

	return 0;
}

static int imx896_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct imx896 *imx896 = sd_to_imx896(sd);
	const struct imx896_mode *mode = imx896->mode;
	struct device *dev = sd->dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	ret = imx896_init(imx896, mode);
	if (ret) {
		dev_err(dev, "failed to write init registers: %d\n", ret);
		goto err_pm;
	}

	ret = __v4l2_ctrl_handler_setup(&imx896->ctrl_handler);
	if (ret)
		goto err_pm;

	ret = cci_write(imx896->regmap, IMX896_REG_STREAMING,
			IMX896_STREAMING_ON, NULL);
	if (ret) {
		dev_err(dev, "failed to start streaming: %d\n", ret);
		goto err_pm;
	}

	return 0;

err_pm:
	cci_write(imx896->regmap, IMX896_REG_STREAMING,
		  IMX896_STREAMING_OFF, NULL);
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static int imx896_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  u32 pad, u64 streams_mask)
{
	struct imx896 *imx896 = sd_to_imx896(sd);
	struct device *dev = sd->dev;
	int ret;

	ret = cci_write(imx896->regmap, IMX896_REG_STREAMING,
			IMX896_STREAMING_OFF, NULL);
	if (ret)
		dev_warn(dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(dev);

	return 0;
}

static int imx896_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index != 0)
		return -EINVAL;

	code->code = sd_to_imx896(sd)->mode->code;

	return 0;
}

static int imx896_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->code != sd_to_imx896(sd)->mode->code ||
	    fse->index != 0)
		return -EINVAL;

	fse->min_width  = sd_to_imx896(sd)->mode->width;
	fse->max_width  = sd_to_imx896(sd)->mode->width;
	fse->min_height = sd_to_imx896(sd)->mode->height;
	fse->max_height = sd_to_imx896(sd)->mode->height;

	return 0;
}

static void imx896_fill_format(const struct imx896_mode *mode,
			       struct v4l2_mbus_framefmt *fmt)
{
	fmt->width	= mode->width;
	fmt->height	= mode->height;
	fmt->code	= mode->code;
	fmt->field	= V4L2_FIELD_NONE;
	fmt->colorspace	= V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc	= V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func	= V4L2_XFER_FUNC_NONE;
}

static int imx896_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx896 *imx896 = sd_to_imx896(sd);
	const struct imx896_mode *mode = imx896->mode;
	struct v4l2_mbus_framefmt *format;

	imx896_fill_format(mode, &fmt->format);

	format = v4l2_subdev_state_get_format(state, 0);
	*format = fmt->format;

	return 0;
}

static int imx896_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX896_NATIVE_WIDTH;
		sel->r.height = IMX896_NATIVE_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx896_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt =
		v4l2_subdev_state_get_format(state, 0);

	imx896_fill_format(sd_to_imx896(sd)->mode, fmt);

	return 0;
}

static const struct v4l2_subdev_video_ops imx896_video_ops = {
	.s_stream	= v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx896_pad_ops = {
	.enum_mbus_code		= imx896_enum_mbus_code,
	.enum_frame_size	= imx896_enum_frame_size,
	.get_fmt		= v4l2_subdev_get_fmt,
	.set_fmt		= imx896_set_fmt,
	.get_selection		= imx896_get_selection,
	.enable_streams		= imx896_enable_streams,
	.disable_streams	= imx896_disable_streams,
};

static const struct v4l2_subdev_ops imx896_subdev_ops = {
	.video	= &imx896_video_ops,
	.pad	= &imx896_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx896_internal_ops = {
	.init_state	= imx896_init_state,
};

static int imx896_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx896 *imx896;
	unsigned int i;
	int ret;

	imx896 = devm_kzalloc(dev, sizeof(*imx896), GFP_KERNEL);
	if (!imx896)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&imx896->sd, client, &imx896_subdev_ops);

	imx896->mode = &imx896_mode;
	ret = imx896_check_hwcfg(imx896);
	if (ret)
		return ret;

	imx896->inclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(imx896->inclk))
		return dev_err_probe(dev, PTR_ERR(imx896->inclk),
				     "failed to get clock\n");

	if (clk_get_rate(imx896->inclk) != IMX896_XVCLK_RATE)
		return dev_err_probe(dev, -EINVAL,
				     "unsupported clock rate %lu Hz, expected %u Hz\n",
				     clk_get_rate(imx896->inclk),
				     IMX896_XVCLK_RATE);

	imx896->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx896->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(imx896->reset_gpio),
				     "failed to get reset GPIO\n");

	imx896->num_supplies = ARRAY_SIZE(imx896_supply_names);
	for (i = 0; i < imx896->num_supplies; i++)
		imx896->supplies[i].supply = imx896_supply_names[i];

	ret = devm_regulator_bulk_get(dev, imx896->num_supplies,
				      imx896->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	imx896->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx896->regmap))
		return dev_err_probe(dev, PTR_ERR(imx896->regmap),
				     "failed to init CCI regmap\n");

	ret = imx896_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to power on\n");

	ret = imx896_check_id(imx896);
	if (ret)
		goto err_power_off;

	ret = imx896_init_controls(imx896);
	if (ret)
		goto err_power_off;

	imx896->sd.internal_ops = &imx896_internal_ops;
	imx896->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx896->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx896->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx896->sd.entity, 1, &imx896->pad);
	if (ret)
		goto err_controls;

	imx896->sd.state_lock = imx896->ctrl_handler.lock;

	ret = v4l2_subdev_init_finalize(&imx896->sd);
	if (ret)
		goto err_entity;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&imx896->sd);
	if (ret) {
		dev_err(dev, "failed to register subdev: %d\n", ret);
		goto err_pm;
	}

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	dev_dbg(dev, "IMX896 sensor detected (chip ID 0x%04x)\n",
		IMX896_CHIP_ID);

	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);

err_entity:
	v4l2_subdev_cleanup(&imx896->sd);
	media_entity_cleanup(&imx896->sd.entity);

err_controls:
	v4l2_ctrl_handler_free(&imx896->ctrl_handler);

err_power_off:
	imx896_power_off(dev);
	return ret;
}

static void imx896_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct device *dev = &client->dev;

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	media_entity_cleanup(&sd->entity);

	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);

	if (!pm_runtime_status_suspended(dev)) {
		imx896_power_off(dev);
		pm_runtime_set_suspended(dev);
	}
}

static const struct dev_pm_ops imx896_pm_ops = {
	SET_RUNTIME_PM_OPS(imx896_power_off, imx896_power_on, NULL)
};

static const struct of_device_id imx896_of_match[] = {
	{ .compatible = "sony,imx896" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx896_of_match);

static struct i2c_driver imx896_i2c_driver = {
	.driver = {
		.name		= "imx896",
		.pm		= pm_ptr(&imx896_pm_ops),
		.of_match_table	= imx896_of_match,
	},
	.probe		= imx896_probe,
	.remove		= imx896_remove,
};

module_i2c_driver(imx896_i2c_driver);

MODULE_AUTHOR("Oleksii Onchul <oleksiionchul@gmail.com>");
MODULE_DESCRIPTION("Sony IMX896 50 MP 10-bit RAW sensor driver");
MODULE_LICENSE("GPL");
