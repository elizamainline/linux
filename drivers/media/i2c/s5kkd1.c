// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung S5KKD1 image sensor
 * Copyright (c) 2026 Oleksii Onchul <oleksiionchul@gmail.com>
 *
 * Register sequences from the FroggerPro Qtech S5KKD1 sensor module data.
 * Initially supports the stock 3280x2464 RAW10, four-lane D-PHY mode.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define S5KKD1_REG_PAGE		CCI_REG16(0xfcfc)
#define S5KKD1_PAGE_MAIN		0x4000
#define S5KKD1_REG_CHIP_ID	CCI_REG16(0x0000)
#define S5KKD1_CHIP_ID		0x4841
#define S5KKD1_REG_STREAM	CCI_REG16(0x0100)
#define S5KKD1_REG_GROUP_HOLD	CCI_REG8(0x0104)
#define S5KKD1_REG_EXPOSURE	CCI_REG16(0x0202)
#define S5KKD1_REG_AGAIN		CCI_REG16(0x0204)
#define S5KKD1_REG_DGAIN		CCI_REG16(0x020e)
#define S5KKD1_REG_VTS		CCI_REG16(0x0340)
#define S5KKD1_REG_TEST_PATTERN	CCI_REG16(0x0600)
#define S5KKD1_WIDTH		3280
#define S5KKD1_HEIGHT		2464
#define S5KKD1_HTS		3968
#define S5KKD1_VTS		4728
#define S5KKD1_EXPOSURE_MARGIN	11
#define S5KKD1_MCLK		19200000
/* Stock outputPixelClock * RAW10 bits / (four lanes * two edges). */
#define S5KKD1_LINK_FREQ		638700000LL
/* VT PLL: 19.2 MHz / 3 * 132 / 6 * 4. */
#define S5KKD1_PIXEL_RATE	563200000

struct s5kkd1 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[3];
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
};

static const s64 s5kkd1_link_freqs[] = { S5KKD1_LINK_FREQ };
static const char * const s5kkd1_test_patterns[] = {
	"Disabled", "Solid color", "Color bars", "Fade to grey color bars",
};

static const struct cci_reg_sequence s5kkd1_reset_regs[] = {
	{ CCI_REG16(0xfcfc), 0x4000 },
	{ CCI_REG16(0x0000), 0x0000 },
	{ CCI_REG16(0x0000), 0x4841 },
	{ CCI_REG16(0x6010), 0x0001 },
};

static const struct cci_reg_sequence s5kkd1_init_regs[] = {
	{ CCI_REG16(0x6214), 0xff7d },
	{ CCI_REG16(0x6218), 0x0000 },
	{ CCI_REG16(0x6226), 0x0001 },
	{ CCI_REG16(0x0a02), 0x0078 },
	{ CCI_REG16(0xfcfc), 0x2400 },
	{ CCI_REG16(0x7b1c), 0x17a3 },
	{ CCI_REG16(0x7b1e), 0x01fc },
	{ CCI_REG16(0x7b20), 0xe702 },
	{ CCI_REG16(0x7b22), 0xe329 },
	{ CCI_REG16(0x7b24), 0xb787 },
	{ CCI_REG16(0x7b26), 0x0024 },
	{ CCI_REG16(0x7b28), 0x3777 },
	{ CCI_REG16(0x7b2a), 0x0024 },
	{ CCI_REG16(0x7b2c), 0x9387 },
	{ CCI_REG16(0x7b2e), 0xc7c7 },
	{ CCI_REG16(0x7b30), 0x2324 },
	{ CCI_REG16(0x7b32), 0xf79e },
	{ CCI_REG16(0x7b34), 0xb747 },
	{ CCI_REG16(0x7b36), 0x0024 },
	{ CCI_REG16(0x7b38), 0x3787 },
	{ CCI_REG16(0x7b3a), 0x0024 },
	{ CCI_REG16(0x7b3c), 0x9387 },
	{ CCI_REG16(0x7b3e), 0x87ff },
	{ CCI_REG16(0x7b40), 0x1307 },
	{ CCI_REG16(0x7b42), 0x47b9 },
	{ CCI_REG16(0x7b44), 0xd8c3 },
	{ CCI_REG16(0x7b46), 0x3787 },
	{ CCI_REG16(0x7b48), 0x0024 },
	{ CCI_REG16(0x7b4a), 0xb746 },
	{ CCI_REG16(0x7b4c), 0x0024 },
	{ CCI_REG16(0x7b4e), 0x1307 },
	{ CCI_REG16(0x7b50), 0x87be },
	{ CCI_REG16(0x7b52), 0x23aa },
	{ CCI_REG16(0x7b54), 0xe6f4 },
	{ CCI_REG16(0x7b56), 0xb785 },
	{ CCI_REG16(0x7b58), 0x0024 },
	{ CCI_REG16(0x7b5a), 0x3787 },
	{ CCI_REG16(0x7b5c), 0x0024 },
	{ CCI_REG16(0x7b5e), 0x37e5 },
	{ CCI_REG16(0x7b60), 0x0120 },
	{ CCI_REG16(0x7b62), 0x1307 },
	{ CCI_REG16(0x7b64), 0xe7bb },
	{ CCI_REG16(0x7b66), 0x0146 },
	{ CCI_REG16(0x7b68), 0x9385 },
	{ CCI_REG16(0x7b6a), 0x85c3 },
	{ CCI_REG16(0x7b6c), 0x1305 },
	{ CCI_REG16(0x7b6e), 0xc504 },
	{ CCI_REG16(0x7b70), 0x98cb },
	{ CCI_REG16(0x7b72), 0x9730 },
	{ CCI_REG16(0x7b74), 0x00fc },
	{ CCI_REG16(0x7b76), 0xe780 },
	{ CCI_REG16(0x7b78), 0x0036 },
	{ CCI_REG16(0x7b7a), 0xb787 },
	{ CCI_REG16(0x7b7c), 0x0024 },
	{ CCI_REG16(0x7b7e), 0x23a6 },
	{ CCI_REG16(0x7b80), 0xa7b8 },
	{ CCI_REG16(0x7b82), 0x17a3 },
	{ CCI_REG16(0x7b84), 0x01fc },
	{ CCI_REG16(0x7b86), 0x6700 },
	{ CCI_REG16(0x7b88), 0xc325 },
	{ CCI_REG16(0x7b8a), 0x0000 },
	{ CCI_REG16(0x7b8c), 0x0000 },
	{ CCI_REG16(0x7b8e), 0x0000 },
	{ CCI_REG16(0x7b90), 0x0000 },
	{ CCI_REG16(0x7b92), 0x0000 },
	{ CCI_REG16(0x7b94), 0x17a3 },
	{ CCI_REG16(0x7b96), 0x01fc },
	{ CCI_REG16(0x7b98), 0xe702 },
	{ CCI_REG16(0x7b9a), 0x6322 },
	{ CCI_REG16(0x7b9c), 0x97f0 },
	{ CCI_REG16(0x7b9e), 0x00fc },
	{ CCI_REG16(0x7ba0), 0xe780 },
	{ CCI_REG16(0x7ba2), 0xc031 },
	{ CCI_REG16(0x7ba4), 0xb777 },
	{ CCI_REG16(0x7ba6), 0x0024 },
	{ CCI_REG16(0x7ba8), 0x03c5 },
	{ CCI_REG16(0x7baa), 0x8710 },
	{ CCI_REG16(0x7bac), 0x9d45 },
	{ CCI_REG16(0x7bae), 0x9710 },
	{ CCI_REG16(0x7bb0), 0x01fc },
	{ CCI_REG16(0x7bb2), 0xe780 },
	{ CCI_REG16(0x7bb4), 0x2029 },
	{ CCI_REG16(0x7bb6), 0x17a3 },
	{ CCI_REG16(0x7bb8), 0x01fc },
	{ CCI_REG16(0x7bba), 0x6700 },
	{ CCI_REG16(0x7bbc), 0x8322 },
	{ CCI_REG16(0x7bbe), 0x17a3 },
	{ CCI_REG16(0x7bc0), 0x01fc },
	{ CCI_REG16(0x7bc2), 0xe702 },
	{ CCI_REG16(0x7bc4), 0xc31f },
	{ CCI_REG16(0x7bc6), 0x9700 },
	{ CCI_REG16(0x7bc8), 0x01fc },
	{ CCI_REG16(0x7bca), 0xe780 },
	{ CCI_REG16(0x7bcc), 0x6077 },
	{ CCI_REG16(0x7bce), 0x1965 },
	{ CCI_REG16(0x7bd0), 0x0146 },
	{ CCI_REG16(0x7bd2), 0x8965 },
	{ CCI_REG16(0x7bd4), 0x1305 },
	{ CCI_REG16(0x7bd6), 0x0522 },
	{ CCI_REG16(0x7bd8), 0x9780 },
	{ CCI_REG16(0x7bda), 0xfffb },
	{ CCI_REG16(0x7bdc), 0xe780 },
	{ CCI_REG16(0x7bde), 0x8077 },
	{ CCI_REG16(0x7be0), 0x17a3 },
	{ CCI_REG16(0x7be2), 0x01fc },
	{ CCI_REG16(0x7be4), 0x6700 },
	{ CCI_REG16(0x7be6), 0xe31f },
	{ CCI_REG16(0x7be8), 0x17a3 },
	{ CCI_REG16(0x7bea), 0x01fc },
	{ CCI_REG16(0x7bec), 0xe702 },
	{ CCI_REG16(0x7bee), 0x231d },
	{ CCI_REG16(0x7bf0), 0x3764 },
	{ CCI_REG16(0x7bf2), 0x0024 },
	{ CCI_REG16(0x7bf4), 0xb767 },
	{ CCI_REG16(0x7bf6), 0x0024 },
	{ CCI_REG16(0x7bf8), 0x1304 },
	{ CCI_REG16(0x7bfa), 0x04a6 },
	{ CCI_REG16(0x7bfc), 0x9387 },
	{ CCI_REG16(0x7bfe), 0x071f },
	{ CCI_REG16(0x7c00), 0x0329 },
	{ CCI_REG16(0x7c02), 0x440e },
	{ CCI_REG16(0x7c04), 0x8324 },
	{ CCI_REG16(0x7c06), 0x4425 },
	{ CCI_REG16(0x7c08), 0x03c7 },
	{ CCI_REG16(0x7c0a), 0xc724 },
	{ CCI_REG16(0x7c0c), 0x83c7 },
	{ CCI_REG16(0x7c0e), 0xd724 },
	{ CCI_REG16(0x7c10), 0x3317 },
	{ CCI_REG16(0x7c12), 0xe900 },
	{ CCI_REG16(0x7c14), 0xb397 },
	{ CCI_REG16(0x7c16), 0xf400 },
	{ CCI_REG16(0x7c18), 0x2322 },
	{ CCI_REG16(0x7c1a), 0xe40e },
	{ CCI_REG16(0x7c1c), 0x232a },
	{ CCI_REG16(0x7c1e), 0xf424 },
	{ CCI_REG16(0x7c20), 0x97b0 },
	{ CCI_REG16(0x7c22), 0xfffb },
	{ CCI_REG16(0x7c24), 0xe780 },
	{ CCI_REG16(0x7c26), 0xa016 },
	{ CCI_REG16(0x7c28), 0x2322 },
	{ CCI_REG16(0x7c2a), 0x240f },
	{ CCI_REG16(0x7c2c), 0x232a },
	{ CCI_REG16(0x7c2e), 0x9424 },
	{ CCI_REG16(0x7c30), 0x17a3 },
	{ CCI_REG16(0x7c32), 0x01fc },
	{ CCI_REG16(0x7c34), 0x6700 },
	{ CCI_REG16(0x7c36), 0xe31a },
	{ CCI_REG16(0x7c38), 0x17a3 },
	{ CCI_REG16(0x7c3a), 0x01fc },
	{ CCI_REG16(0x7c3c), 0xe702 },
	{ CCI_REG16(0x7c3e), 0x2318 },
	{ CCI_REG16(0x7c40), 0xb787 },
	{ CCI_REG16(0x7c42), 0x0024 },
	{ CCI_REG16(0x7c44), 0x03a4 },
	{ CCI_REG16(0x7c46), 0xc7b8 },
	{ CCI_REG16(0x7c48), 0x0146 },
	{ CCI_REG16(0x7c4a), 0x1145 },
	{ CCI_REG16(0x7c4c), 0xa285 },
	{ CCI_REG16(0x7c4e), 0x9780 },
	{ CCI_REG16(0x7c50), 0xfffb },
	{ CCI_REG16(0x7c52), 0xe780 },
	{ CCI_REG16(0x7c54), 0x407a },
	{ CCI_REG16(0x7c56), 0x9760 },
	{ CCI_REG16(0x7c58), 0x01fc },
	{ CCI_REG16(0x7c5a), 0xe780 },
	{ CCI_REG16(0x7c5c), 0x603f },
	{ CCI_REG16(0x7c5e), 0x0546 },
	{ CCI_REG16(0x7c60), 0xa285 },
	{ CCI_REG16(0x7c62), 0x1145 },
	{ CCI_REG16(0x7c64), 0x9780 },
	{ CCI_REG16(0x7c66), 0xfffb },
	{ CCI_REG16(0x7c68), 0xe780 },
	{ CCI_REG16(0x7c6a), 0xe078 },
	{ CCI_REG16(0x7c6c), 0xb767 },
	{ CCI_REG16(0x7c6e), 0x0024 },
	{ CCI_REG16(0x7c70), 0x239a },
	{ CCI_REG16(0x7c72), 0x0738 },
	{ CCI_REG16(0x7c74), 0x17a3 },
	{ CCI_REG16(0x7c76), 0x01fc },
	{ CCI_REG16(0x7c78), 0x6700 },
	{ CCI_REG16(0x7c7a), 0xa316 },
	{ CCI_REG16(0x35cc), 0x1c80 },
	{ CCI_REG16(0x35ce), 0x0024 },
	{ CCI_REG16(0xfcfc), 0x2400 },
	{ CCI_REG16(0x005a), 0x0000 },
	{ CCI_REG16(0x005c), 0xd60e },
	{ CCI_REG16(0x0060), 0xd606 },
	{ CCI_REG16(0x00ec), 0x0000 },
	{ CCI_REG16(0x011a), 0x0000 },
	{ CCI_REG16(0x0274), 0x0202 },
	{ CCI_REG16(0x0288), 0x0000 },
	{ CCI_REG16(0x028a), 0x0000 },
	{ CCI_REG16(0x0290), 0x0000 },
	{ CCI_REG16(0x0292), 0x0000 },
	{ CCI_REG16(0x029c), 0x0000 },
	{ CCI_REG16(0x029e), 0x0000 },
	{ CCI_REG16(0x02a4), 0x0000 },
	{ CCI_REG16(0x02a6), 0x0000 },
	{ CCI_REG16(0x02a8), 0x0000 },
	{ CCI_REG16(0x02aa), 0x0000 },
	{ CCI_REG16(0x02ac), 0x0000 },
	{ CCI_REG16(0x032a), 0x0000 },
	{ CCI_REG16(0x0634), 0x0002 },
	{ CCI_REG16(0x0636), 0x0002 },
	{ CCI_REG16(0x0638), 0x080a },
	{ CCI_REG16(0x063a), 0x080a },
	{ CCI_REG16(0x063c), 0x0002 },
	{ CCI_REG16(0x063e), 0x0002 },
	{ CCI_REG16(0x0640), 0x080a },
	{ CCI_REG16(0x0642), 0x080a },
	{ CCI_REG16(0x0b48), 0x0000 },
	{ CCI_REG16(0x10ba), 0x0100 },
	{ CCI_REG16(0x10f6), 0x0100 },
	{ CCI_REG16(0x1222), 0x0100 },
	{ CCI_REG16(0x1224), 0x9411 },
	{ CCI_REG16(0x1248), 0x6018 },
	{ CCI_REG16(0x1256), 0x0b00 },
	{ CCI_REG16(0x1260), 0x0100 },
	{ CCI_REG16(0x1292), 0x0101 },
	{ CCI_REG16(0x1294), 0x0001 },
	{ CCI_REG16(0x12a4), 0x0307 },
	{ CCI_REG16(0x1c2e), 0x0300 },
	{ CCI_REG16(0x1c66), 0x530a },
	{ CCI_REG16(0x1c8e), 0x3401 },
	{ CCI_REG16(0x1cbe), 0x0000 },
	{ CCI_REG16(0x1f02), 0xca44 },
	{ CCI_REG16(0x21b0), 0x0101 },
	{ CCI_REG16(0x21b2), 0x0101 },
	{ CCI_REG16(0x2204), 0x0000 },
	{ CCI_REG16(0x22be), 0x0101 },
	{ CCI_REG16(0x2322), 0x0000 },
	{ CCI_REG16(0x2f74), 0x0050 },
	{ CCI_REG16(0x2f7c), 0x0a00 },
	{ CCI_REG16(0x3020), 0x0800 },
	{ CCI_REG16(0x302c), 0x0000 },
	{ CCI_REG16(0x303c), 0x0000 },
	{ CCI_REG16(0x30ae), 0x0200 },
	{ CCI_REG16(0x3128), 0x0001 },
	{ CCI_REG16(0x3162), 0x1400 },
	{ CCI_REG16(0x3192), 0x0000 },
	{ CCI_REG16(0x5730), 0x1ea1 },
	{ CCI_REG16(0x5736), 0x0000 },
	{ CCI_REG16(0x58c0), 0x0c00 },
	{ CCI_REG16(0x58c4), 0x0000 },
	{ CCI_REG16(0x58c6), 0x0700 },
	{ CCI_REG16(0x58c8), 0x0c00 },
	{ CCI_REG16(0x58ce), 0x0700 },
	{ CCI_REG16(0xfcfc), 0x4000 },
	{ CCI_REG16(0x001e), 0x0105 },
	{ CCI_REG16(0x011a), 0x0001 },
	{ CCI_REG16(0x0b0a), 0x0101 },
	{ CCI_REG16(0xf41c), 0x0002 },
	{ CCI_REG16(0xf44a), 0x0006 },
	{ CCI_REG16(0xf44c), 0x0b0b },
};

static const struct cci_reg_sequence s5kkd1_3280x2464_regs[] = {
	{ CCI_REG16(0xfcfc), 0x4000 },
	{ CCI_REG16(0x0136), 0x1300 },
	{ CCI_REG16(0x013e), 0x00c8 },
	{ CCI_REG16(0x0304), 0x0003 },
	{ CCI_REG16(0x0306), 0x0084 },
	{ CCI_REG16(0x030c), 0x0000 },
	{ CCI_REG16(0x0302), 0x0001 },
	{ CCI_REG16(0x0300), 0x0006 },
	{ CCI_REG16(0x030e), 0x0002 },
	{ CCI_REG16(0x0310), 0x0043 },
	{ CCI_REG16(0x0322), 0x8800 },
	{ CCI_REG16(0x0312), 0x0000 },
	{ CCI_REG16(0x030a), 0x0001 },
	{ CCI_REG16(0x0308), 0x0008 },
	{ CCI_REG16(0x02ee), 0x0000 },
	{ CCI_REG16(0xfcfc), 0x2400 },
	{ CCI_REG16(0x1c6a), 0x9600 },
	{ CCI_REG16(0xfcfc), 0x4000 },
	{ CCI_REG16(0x0344), 0x0000 },
	{ CCI_REG16(0x0348), 0x19af },
	{ CCI_REG16(0x0346), 0x0000 },
	{ CCI_REG16(0x034a), 0x134f },
	{ CCI_REG16(0x0350), 0x0004 },
	{ CCI_REG16(0x0352), 0x0004 },
	{ CCI_REG16(0x034c), 0x0cd0 },
	{ CCI_REG16(0x034e), 0x09a0 },
	{ CCI_REG16(0x0900), 0x0122 },
	{ CCI_REG16(0x0404), 0x1000 },
	{ CCI_REG16(0x0936), 0x0000 },
	{ CCI_REG16(0x0c40), 0x0000 },
	{ CCI_REG16(0x0086), 0x1000 },
	{ CCI_REG16(0x0380), 0x0002 },
	{ CCI_REG16(0x0382), 0x0002 },
	{ CCI_REG16(0x0384), 0x0002 },
	{ CCI_REG16(0x0386), 0x0002 },
	{ CCI_REG16(0x0342), 0x0f80 },
	{ CCI_REG16(0x0340), 0x1278 },
	{ CCI_REG16(0x0702), 0x0000 },
	{ CCI_REG16(0x0112), 0x0a0a },
	{ CCI_REG16(0x0114), 0x0300 },
	{ CCI_REG16(0x0116), 0x2b00 },
	{ CCI_REG16(0x0118), 0x0000 },
	{ CCI_REG16(0x011c), 0x0100 },
	{ CCI_REG16(0x080e), 0x1200 },
	{ CCI_REG16(0x0816), 0x0000 },
	{ CCI_REG16(0x0d00), 0x0101 },
	{ CCI_REG16(0x0d02), 0x0001 },
	{ CCI_REG16(0x0d04), 0x0102 },
	{ CCI_REG16(0x0b06), 0x0101 },
	{ CCI_REG16(0x0b08), 0x0000 },
	{ CCI_REG16(0x0fea), 0x0b00 },
	{ CCI_REG16(0xfcfc), 0x2400 },
	{ CCI_REG16(0x01ea), 0xc017 },
	{ CCI_REG16(0x01ec), 0x0018 },
	{ CCI_REG16(0x0050), 0x0100 },
	{ CCI_REG16(0x0070), 0x0002 },
	{ CCI_REG16(0x0080), 0x0600 },
	{ CCI_REG16(0x0084), 0x0000 },
	{ CCI_REG16(0x008a), 0x0000 },
	{ CCI_REG16(0x0098), 0x0a00 },
	{ CCI_REG16(0x009a), 0x0c00 },
	{ CCI_REG16(0x009c), 0x0a00 },
	{ CCI_REG16(0x009e), 0x0c00 },
	{ CCI_REG16(0x00b4), 0x4001 },
	{ CCI_REG16(0x00b8), 0x4001 },
	{ CCI_REG16(0x00e6), 0x0a00 },
	{ CCI_REG16(0x0130), 0x0002 },
	{ CCI_REG16(0x0132), 0x0804 },
	{ CCI_REG16(0x0134), 0x0100 },
	{ CCI_REG16(0x0138), 0x0001 },
	{ CCI_REG16(0x01a6), 0x7701 },
	{ CCI_REG16(0x028c), 0x0000 },
	{ CCI_REG16(0x028e), 0x0000 },
	{ CCI_REG16(0x0294), 0x0000 },
	{ CCI_REG16(0x0296), 0x0000 },
	{ CCI_REG16(0x0298), 0x0000 },
	{ CCI_REG16(0x029a), 0x0000 },
	{ CCI_REG16(0x02a0), 0x0000 },
	{ CCI_REG16(0x02a2), 0x0000 },
	{ CCI_REG16(0x0308), 0x0000 },
	{ CCI_REG16(0x030a), 0x0000 },
	{ CCI_REG16(0x030c), 0x0000 },
	{ CCI_REG16(0x030e), 0x0000 },
	{ CCI_REG16(0x0310), 0x0000 },
	{ CCI_REG16(0x0312), 0x0000 },
	{ CCI_REG16(0x0314), 0x0000 },
	{ CCI_REG16(0x0316), 0x0000 },
	{ CCI_REG16(0x0318), 0x0000 },
	{ CCI_REG16(0x031a), 0x0000 },
	{ CCI_REG16(0x031c), 0x0000 },
	{ CCI_REG16(0x031e), 0x0000 },
	{ CCI_REG16(0x0320), 0x0000 },
	{ CCI_REG16(0x0322), 0x0000 },
	{ CCI_REG16(0x0324), 0x0000 },
	{ CCI_REG16(0x0326), 0x0000 },
	{ CCI_REG16(0x0328), 0x0000 },
	{ CCI_REG16(0x032c), 0x0000 },
	{ CCI_REG16(0x0632), 0x0110 },
	{ CCI_REG16(0x0906), 0x1000 },
	{ CCI_REG16(0x097e), 0x1000 },
	{ CCI_REG16(0x09f6), 0x1000 },
	{ CCI_REG16(0x0a6e), 0x1000 },
	{ CCI_REG16(0x0a78), 0x3000 },
	{ CCI_REG16(0x0a7a), 0x3000 },
	{ CCI_REG16(0x0ae6), 0x1000 },
	{ CCI_REG16(0x0af0), 0x3000 },
	{ CCI_REG16(0x0af2), 0x3000 },
	{ CCI_REG16(0x0b32), 0x0000 },
	{ CCI_REG16(0x0b4a), 0x0100 },
	{ CCI_REG16(0x1088), 0x0020 },
	{ CCI_REG16(0x109e), 0x1000 },
	{ CCI_REG16(0x10da), 0x0100 },
	{ CCI_REG16(0x1226), 0x0200 },
	{ CCI_REG16(0x1230), 0x0100 },
	{ CCI_REG16(0x1288), 0x0000 },
	{ CCI_REG16(0x128a), 0x0400 },
	{ CCI_REG16(0x128c), 0x0200 },
	{ CCI_REG16(0x128e), 0x0a00 },
	{ CCI_REG16(0x1290), 0x0900 },
	{ CCI_REG16(0x12a0), 0x0101 },
	{ CCI_REG16(0x12a2), 0x0100 },
	{ CCI_REG16(0x12a6), 0x0000 },
	{ CCI_REG16(0x12a8), 0x0000 },
	{ CCI_REG16(0x12aa), 0x0000 },
	{ CCI_REG16(0x12c4), 0x0402 },
	{ CCI_REG16(0x12dc), 0x0000 },
	{ CCI_REG16(0x13b6), 0x1800 },
	{ CCI_REG16(0x13bc), 0x1800 },
	{ CCI_REG16(0x1c6e), 0x0000 },
	{ CCI_REG16(0x1c9a), 0x8207 },
	{ CCI_REG16(0x1ca4), 0x8a00 },
	{ CCI_REG16(0x1ca6), 0x0206 },
	{ CCI_REG16(0x1cb0), 0xbf00 },
	{ CCI_REG16(0x1cba), 0x0101 },
	{ CCI_REG16(0x1f42), 0x0100 },
	{ CCI_REG16(0x1f5e), 0x0000 },
	{ CCI_REG16(0x2144), 0x1a00 },
	{ CCI_REG16(0x2156), 0x0000 },
	{ CCI_REG16(0x2176), 0x0000 },
	{ CCI_REG16(0x2180), 0x0000 },
	{ CCI_REG16(0x2182), 0x0000 },
	{ CCI_REG16(0x2190), 0xffff },
	{ CCI_REG16(0x21b4), 0x0101 },
	{ CCI_REG16(0x21b6), 0x0100 },
	{ CCI_REG16(0x21b8), 0x0000 },
	{ CCI_REG16(0x21ba), 0x7f00 },
	{ CCI_REG16(0x21bc), 0x0008 },
	{ CCI_REG16(0x21be), 0x0000 },
	{ CCI_REG16(0x21c0), 0x0000 },
	{ CCI_REG16(0x21c4), 0x8000 },
	{ CCI_REG16(0x21c6), 0x0108 },
	{ CCI_REG16(0x21c8), 0x0000 },
	{ CCI_REG16(0x21ca), 0x0000 },
	{ CCI_REG16(0x21ce), 0x5500 },
	{ CCI_REG16(0x21d0), 0x6600 },
	{ CCI_REG16(0x21d2), 0x10d6 },
	{ CCI_REG16(0x21fe), 0x0800 },
	{ CCI_REG16(0x2200), 0x0a00 },
	{ CCI_REG16(0x2202), 0x6cf4 },
	{ CCI_REG16(0x2206), 0x4100 },
	{ CCI_REG16(0x2208), 0x3ef6 },
	{ CCI_REG16(0x222e), 0x0000 },
	{ CCI_REG16(0x2230), 0x0000 },
	{ CCI_REG16(0x2232), 0x0000 },
	{ CCI_REG16(0x225e), 0x0000 },
	{ CCI_REG16(0x2260), 0x0000 },
	{ CCI_REG16(0x2262), 0x0000 },
	{ CCI_REG16(0x22c2), 0x0101 },
	{ CCI_REG16(0x22c8), 0x4000 },
	{ CCI_REG16(0x22ca), 0x7f00 },
	{ CCI_REG16(0x22cc), 0x8000 },
	{ CCI_REG16(0x22ce), 0x0001 },
	{ CCI_REG16(0x22d0), 0x0002 },
	{ CCI_REG16(0x22d2), 0x0004 },
	{ CCI_REG16(0x22d4), 0x0010 },
	{ CCI_REG16(0x230e), 0x54f4 },
	{ CCI_REG16(0x2310), 0x1611 },
	{ CCI_REG16(0x2312), 0x1607 },
	{ CCI_REG16(0x2314), 0x1611 },
	{ CCI_REG16(0x2316), 0x1613 },
	{ CCI_REG16(0x2318), 0x1609 },
	{ CCI_REG16(0x231a), 0x1613 },
	{ CCI_REG16(0x231c), 0x1619 },
	{ CCI_REG16(0x231e), 0x161f },
	{ CCI_REG16(0x2320), 0x6af4 },
	{ CCI_REG16(0x2324), 0x4000 },
	{ CCI_REG16(0x2326), 0x4000 },
	{ CCI_REG16(0x2328), 0x0000 },
	{ CCI_REG16(0x232a), 0x4000 },
	{ CCI_REG16(0x232c), 0x4000 },
	{ CCI_REG16(0x232e), 0x4000 },
	{ CCI_REG16(0x2330), 0x4000 },
	{ CCI_REG16(0x2332), 0x0ad6 },
	{ CCI_REG16(0x2334), 0x0c0c },
	{ CCI_REG16(0x2336), 0x0c0c },
	{ CCI_REG16(0x2338), 0x0c0c },
	{ CCI_REG16(0x233a), 0x0101 },
	{ CCI_REG16(0x233c), 0x0101 },
	{ CCI_REG16(0x233e), 0x0a0a },
	{ CCI_REG16(0x2340), 0x0a0a },
	{ CCI_REG16(0x2342), 0x0a0a },
	{ CCI_REG16(0x2344), 0x02f4 },
	{ CCI_REG16(0x2346), 0x4000 },
	{ CCI_REG16(0x2348), 0x4000 },
	{ CCI_REG16(0x234a), 0x4000 },
	{ CCI_REG16(0x234c), 0x4c00 },
	{ CCI_REG16(0x234e), 0x4c00 },
	{ CCI_REG16(0x2350), 0x4000 },
	{ CCI_REG16(0x2352), 0x4000 },
	{ CCI_REG16(0x2354), 0x4000 },
	{ CCI_REG16(0x2f70), 0xe803 },
	{ CCI_REG16(0x2f8e), 0x0101 },
	{ CCI_REG16(0x2f94), 0x0100 },
	{ CCI_REG16(0x2fa6), 0x0200 },
	{ CCI_REG16(0x2fde), 0x0000 },
	{ CCI_REG16(0x2fe0), 0x0000 },
	{ CCI_REG16(0x3018), 0x0000 },
	{ CCI_REG16(0x3098), 0x0400 },
	{ CCI_REG16(0x30a0), 0x3003 },
	{ CCI_REG16(0x30c6), 0x0000 },
	{ CCI_REG16(0x30d4), 0x0000 },
	{ CCI_REG16(0x313a), 0x0100 },
	{ CCI_REG16(0x314a), 0x0010 },
	{ CCI_REG16(0x3164), 0x0000 },
	{ CCI_REG16(0x31ce), 0x2c01 },
	{ CCI_REG16(0x3e40), 0x0100 },
	{ CCI_REG16(0x3e50), 0x0000 },
	{ CCI_REG16(0x3e54), 0xdc00 },
	{ CCI_REG16(0x5732), 0x0100 },
	{ CCI_REG16(0x5734), 0x0000 },
	{ CCI_REG16(0x58cc), 0x0100 },
	{ CCI_REG16(0x7108), 0x0000 },
	{ CCI_REG16(0xfcfc), 0x4000 },
	{ CCI_REG16(0x6226), 0x0000 },
};

static struct s5kkd1 *to_s5kkd1(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5kkd1, sd);
}

static int s5kkd1_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5kkd1 *sensor = container_of(ctrl->handler, struct s5kkd1, ctrls);
	s64 exposure_max;
	int ret = 0, release_ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		exposure_max = S5KKD1_HEIGHT + ctrl->val - S5KKD1_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(sensor->exposure, 5, exposure_max, 1,
					 min_t(s64, sensor->exposure->default_value,
					       exposure_max));
	}

	/* Do not touch registers during the probe's powered, unconfigured state. */
	if (!pm_runtime_get_if_in_use(sensor->dev))
		return 0;

	cci_write(sensor->regmap, S5KKD1_REG_PAGE, S5KKD1_PAGE_MAIN, &ret);
	cci_write(sensor->regmap, S5KKD1_REG_GROUP_HOLD, 1, &ret);
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(sensor->regmap, S5KKD1_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(sensor->regmap, S5KKD1_REG_AGAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		/* The stock exposure library programs the global digital gain. */
		cci_write(sensor->regmap, S5KKD1_REG_DGAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(sensor->regmap, S5KKD1_REG_VTS,
			  S5KKD1_HEIGHT + ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(sensor->regmap, S5KKD1_REG_TEST_PATTERN, ctrl->val,
			  &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	/* Release group hold even if a control write failed. */
	release_ret = cci_write(sensor->regmap, S5KKD1_REG_GROUP_HOLD, 0, NULL);
	pm_runtime_put(sensor->dev);
	return ret ?: release_ret;
}

static const struct v4l2_ctrl_ops s5kkd1_ctrl_ops = {
	.s_ctrl = s5kkd1_set_ctrl,
};

static int s5kkd1_init_controls(struct s5kkd1 *sensor)
{
	struct v4l2_ctrl_handler *hdl = &sensor->ctrls;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(hdl, 9);
	ctrl = v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ, 0, 0,
				      s5kkd1_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE,
				 S5KKD1_PIXEL_RATE, S5KKD1_PIXEL_RATE, 1,
				 S5KKD1_PIXEL_RATE);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_HBLANK,
				 S5KKD1_HTS - S5KKD1_WIDTH,
				 S5KKD1_HTS - S5KKD1_WIDTH, 1,
				 S5KKD1_HTS - S5KKD1_WIDTH);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	sensor->vblank = v4l2_ctrl_new_std(hdl, &s5kkd1_ctrl_ops,
					   V4L2_CID_VBLANK,
					   S5KKD1_VTS - S5KKD1_HEIGHT,
					   0xffff - S5KKD1_HEIGHT, 1,
					   S5KKD1_VTS - S5KKD1_HEIGHT);
	sensor->exposure =
		v4l2_ctrl_new_std(hdl, &s5kkd1_ctrl_ops, V4L2_CID_EXPOSURE, 5,
				  S5KKD1_VTS - S5KKD1_EXPOSURE_MARGIN, 1, 1000);
	/* Gain units are 1/32 for analogue and 1/256 for digital. */
	v4l2_ctrl_new_std(hdl, &s5kkd1_ctrl_ops, V4L2_CID_ANALOGUE_GAIN, 32,
			  512, 1, 32);
	v4l2_ctrl_new_std(hdl, &s5kkd1_ctrl_ops, V4L2_CID_DIGITAL_GAIN, 256,
			  256, 1, 256);
	v4l2_ctrl_new_std_menu_items(hdl, &s5kkd1_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5kkd1_test_patterns) - 1, 0, 0,
				     s5kkd1_test_patterns);
	if (hdl->error) {
		ret = hdl->error;
		goto free_ctrls;
	}
	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret)
		goto free_ctrls;
	ret = v4l2_ctrl_new_fwnode_properties(hdl, &s5kkd1_ctrl_ops, &props);
	if (ret)
		goto free_ctrls;
	sensor->sd.ctrl_handler = hdl;
	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

static int s5kkd1_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct s5kkd1 *sensor = to_s5kkd1(sd);
	int ret;

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret)
		return ret;

	ret = cci_multi_reg_write(sensor->regmap, s5kkd1_reset_regs,
				  ARRAY_SIZE(s5kkd1_reset_regs), NULL);
	if (ret)
		goto error;
	/* Stock initSetting requires 8000 us after the software reset. */
	usleep_range(8000, 9000);
	cci_multi_reg_write(sensor->regmap, s5kkd1_init_regs,
			    ARRAY_SIZE(s5kkd1_init_regs), &ret);
	cci_multi_reg_write(sensor->regmap, s5kkd1_3280x2464_regs,
			    ARRAY_SIZE(s5kkd1_3280x2464_regs), &ret);
	if (ret)
		goto error;
	ret = __v4l2_ctrl_handler_setup(&sensor->ctrls);
	cci_write(sensor->regmap, S5KKD1_REG_STREAM, 0x0100, &ret);
	if (ret)
		goto error;
	return 0;

error:
	dev_err(sensor->dev, "Failed to start streaming: %d\n", ret);
	cci_write(sensor->regmap, S5KKD1_REG_PAGE, S5KKD1_PAGE_MAIN, NULL);
	cci_write(sensor->regmap, S5KKD1_REG_STREAM, 0, NULL);
	pm_runtime_put_sync_suspend(sensor->dev);
	return ret;
}

static int s5kkd1_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct s5kkd1 *sensor = to_s5kkd1(sd);
	int ret;

	ret = cci_write(sensor->regmap, S5KKD1_REG_STREAM, 0, NULL);
	if (ret)
		dev_err(sensor->dev, "Failed to stop streaming: %d\n", ret);
	/* Reset/power-off also stops output after a failed standby write. */
	pm_runtime_put_sync_suspend(sensor->dev);
	return ret;
}

static int s5kkd1_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	if (fmt->pad)
		return -EINVAL;
	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    v4l2_subdev_is_streaming(sd))
		return -EBUSY;
	fmt->format = (struct v4l2_mbus_framefmt){
		.width = S5KKD1_WIDTH,
		.height = S5KKD1_HEIGHT,
		.code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_RAW,
		.xfer_func = V4L2_XFER_FUNC_NONE,
		.quantization = V4L2_QUANTIZATION_FULL_RANGE,
	};
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	return 0;
}

static int s5kkd1_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	return 0;
}

static int s5kkd1_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->pad || fse->index || fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;
	fse->min_width = S5KKD1_WIDTH;
	fse->max_width = S5KKD1_WIDTH;
	fse->min_height = S5KKD1_HEIGHT;
	fse->max_height = S5KKD1_HEIGHT;
	return 0;
}

static int s5kkd1_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	if (sel->pad)
		return -EINVAL;
	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r = (struct v4l2_rect){ 0, 0, 6576, 4944 };
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP:
		/* 2x2 binning, four binned pixels cropped on each edge. */
		sel->r = (struct v4l2_rect){ 8, 8, 6560, 4928 };
		return 0;
	default:
		return -EINVAL;
	}
}

static int s5kkd1_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = { .which = V4L2_SUBDEV_FORMAT_TRY };

	return s5kkd1_set_fmt(sd, state, &fmt);
}

static const struct v4l2_subdev_video_ops s5kkd1_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops s5kkd1_pad_ops = {
	.enum_mbus_code = s5kkd1_enum_mbus_code,
	.enum_frame_size = s5kkd1_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = s5kkd1_set_fmt,
	.get_selection = s5kkd1_get_selection,
	.enable_streams = s5kkd1_enable_streams,
	.disable_streams = s5kkd1_disable_streams,
};

static const struct v4l2_subdev_ops s5kkd1_subdev_ops = {
	.video = &s5kkd1_video_ops,
	.pad = &s5kkd1_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5kkd1_internal_ops = {
	.init_state = s5kkd1_init_state,
};

static const struct media_entity_operations s5kkd1_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int s5kkd1_power_on(struct device *dev)
{
	struct s5kkd1 *sensor = to_s5kkd1(dev_get_drvdata(dev));
	int ret;

	/* Stock sequence: reset low, VIO, VDIG, VANA, reset high, MCLK. */
	gpiod_set_value_cansleep(sensor->reset, 1);
	ret = regulator_enable(sensor->supplies[0].consumer);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = regulator_enable(sensor->supplies[1].consumer);
	if (ret)
		goto disable_io;
	usleep_range(2000, 3000);
	ret = regulator_enable(sensor->supplies[2].consumer);
	if (ret)
		goto disable_digital;
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(sensor->reset, 0);
	usleep_range(3000, 4000);
	ret = clk_prepare_enable(sensor->mclk);
	if (ret)
		goto assert_reset;
	usleep_range(12000, 13000);
	return 0;

assert_reset:
	gpiod_set_value_cansleep(sensor->reset, 1);
	regulator_disable(sensor->supplies[2].consumer);
disable_digital:
	regulator_disable(sensor->supplies[1].consumer);
disable_io:
	regulator_disable(sensor->supplies[0].consumer);
	return ret;
}

static int s5kkd1_power_off(struct device *dev)
{
	struct s5kkd1 *sensor = to_s5kkd1(dev_get_drvdata(dev));

	clk_disable_unprepare(sensor->mclk);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(sensor->reset, 1);
	regulator_disable(sensor->supplies[2].consumer);
	usleep_range(1000, 2000);
	regulator_disable(sensor->supplies[1].consumer);
	regulator_disable(sensor->supplies[0].consumer);
	usleep_range(1000, 2000);
	return 0;
}

static int s5kkd1_check_endpoint(struct s5kkd1 *sensor)
{
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *endpoint;
	unsigned long bitmap;
	int ret;

	endpoint =
		fwnode_graph_get_next_endpoint(dev_fwnode(sensor->dev), NULL);
	if (!endpoint)
		return -EINVAL;
	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret)
		return ret;
	if (ep.bus.mipi_csi2.num_data_lanes != 4) {
		ret = -EINVAL;
		goto free_ep;
	}
	ret = v4l2_link_freq_to_bitmap(sensor->dev, ep.link_frequencies,
				       ep.nr_of_link_frequencies,
				       s5kkd1_link_freqs,
				       ARRAY_SIZE(s5kkd1_link_freqs), &bitmap);
free_ep:
	v4l2_fwnode_endpoint_free(&ep);
	return ret;
}

static int s5kkd1_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s5kkd1 *sensor;
	u64 id;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	sensor->dev = dev;
	v4l2_i2c_subdev_init(&sensor->sd, client, &s5kkd1_subdev_ops);
	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap),
				     "Failed to init CCI\n");
	sensor->mclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(sensor->mclk))
		return dev_err_probe(dev, PTR_ERR(sensor->mclk),
				     "Failed to get MCLK\n");
	if (clk_get_rate(sensor->mclk) != S5KKD1_MCLK)
		return dev_err_probe(dev, -EINVAL, "MCLK must be 19.2 MHz\n");
	ret = s5kkd1_check_endpoint(sensor);
	if (ret)
		return dev_err_probe(dev, ret, "Invalid CSI-2 endpoint\n");
	sensor->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset))
		return dev_err_probe(dev, PTR_ERR(sensor->reset),
				     "Failed to get reset GPIO\n");
	sensor->supplies[0].supply = "vddio";
	sensor->supplies[1].supply = "vddd";
	sensor->supplies[2].supply = "vdda";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sensor->supplies),
				      sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");
	ret = s5kkd1_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to power on\n");
	ret = cci_read(sensor->regmap, S5KKD1_REG_CHIP_ID, &id, NULL);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to read chip ID\n");
		goto power_off;
	}
	if (id != S5KKD1_CHIP_ID) {
		ret = -ENODEV;
		dev_err(dev, "Unexpected chip ID: %#llx\n", id);
		goto power_off;
	}
	ret = s5kkd1_init_controls(sensor);
	if (ret)
		goto power_off;
	sensor->sd.internal_ops = &s5kkd1_internal_ops;
	sensor->sd.state_lock = sensor->ctrls.lock;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.ops = &s5kkd1_entity_ops;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto free_ctrls;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto clean_entity;

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto disable_pm;
	pm_runtime_put_sync_suspend(dev);
	return 0;

disable_pm:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&sensor->sd);
clean_entity:
	media_entity_cleanup(&sensor->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrls);
power_off:
	s5kkd1_power_off(dev);
	return ret;
}

static void s5kkd1_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5kkd1 *sensor = to_s5kkd1(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);
	pm_runtime_disable(sensor->dev);
	if (!pm_runtime_status_suspended(sensor->dev))
		s5kkd1_power_off(sensor->dev);
	pm_runtime_set_suspended(sensor->dev);
}

static const struct dev_pm_ops s5kkd1_pm_ops = {
	SET_RUNTIME_PM_OPS(s5kkd1_power_off, s5kkd1_power_on, NULL)
};

static const struct of_device_id s5kkd1_of_match[] = {
	{ .compatible = "samsung,s5kkd1" },
	{}
};
MODULE_DEVICE_TABLE(of, s5kkd1_of_match);

static struct i2c_driver s5kkd1_driver = {
	.driver = {
		.name = "s5kkd1",
		.pm = &s5kkd1_pm_ops,
		.of_match_table = s5kkd1_of_match,
	},
	.probe = s5kkd1_probe,
	.remove = s5kkd1_remove,
};
module_i2c_driver(s5kkd1_driver);

MODULE_AUTHOR("Oleksii Onchul <oleksiionchul@gmail.com>");
MODULE_DESCRIPTION("Samsung S5KKD1 image sensor driver");
MODULE_LICENSE("GPL");
