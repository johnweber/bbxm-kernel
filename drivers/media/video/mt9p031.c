/*
 * Driver for MT9P031 CMOS Image Sensor from Aptina
 *
 * Copyright (C) 2011, Javier Martin <javier.martin@vista-silicon.com>
 *
 * Copyright (C) 2011, Guennadi Liakhovetski <g.liakhovetski@gmx.de>
 *
 * Based on the MT9V032 driver and Bastian Hecht's code.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/log2.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <media/v4l2-subdev.h>
#include <linux/videodev2.h>

#include <media/mt9p031.h>
#include <media/v4l2-chip-ident.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-device.h>

#define MT9P031_EXTCLK_FREQ			20000000

#define MT9P031_CHIP_VERSION			0x00
#define		MT9P031_CHIP_VERSION_VALUE	0x1801
#define MT9P031_ROW_START			0x01
#define		MT9P031_ROW_START_MIN		1
#define		MT9P031_ROW_START_MAX		2004
#define		MT9P031_ROW_START_DEF		54
#define MT9P031_COLUMN_START			0x02
#define		MT9P031_COLUMN_START_MIN	1
#define		MT9P031_COLUMN_START_MAX	2750
#define		MT9P031_COLUMN_START_DEF	16
#define MT9P031_WINDOW_HEIGHT			0x03
#define		MT9P031_WINDOW_HEIGHT_MIN	2
#define		MT9P031_WINDOW_HEIGHT_MAX	2003
#define		MT9P031_WINDOW_HEIGHT_DEF	2003
#define MT9P031_WINDOW_WIDTH			0x04
#define		MT9P031_WINDOW_WIDTH_MIN	18
#define		MT9P031_WINDOW_WIDTH_MAX	2751
#define		MT9P031_WINDOW_WIDTH_DEF	2751
#define MT9P031_H_BLANKING			0x05
#define		MT9P031_H_BLANKING_VALUE	0
#define MT9P031_V_BLANKING			0x06
#define		MT9P031_V_BLANKING_VALUE	25
#define MT9P031_OUTPUT_CONTROL			0x07
#define		MT9P031_OUTPUT_CONTROL_CEN	2
#define		MT9P031_OUTPUT_CONTROL_SYN	1
#define MT9P031_SHUTTER_WIDTH_UPPER		0x08
#define MT9P031_SHUTTER_WIDTH			0x09
#define	MT9P031_PLL_CONTROL			0x10
#define		MT9P031_PLL_CONTROL_PWROFF	0x0050
#define		MT9P031_PLL_CONTROL_PWRON	0x0051
#define		MT9P031_PLL_CONTROL_USEPLL	0x0052
#define	MT9P031_PLL_CONFIG_1			0x11
#define		MT9P031_PLL_CONFIG_1_M_48MHZ	0x5000
#define		MT9P031_PLL_CONFIG_1_N_48MHZ	0x05
#define		MT9P031_PLL_CONFIG_1_M_96MHZ	0x3600
#define		MT9P031_PLL_CONFIG_1_N_96MHZ	0x05
#define	MT9P031_PLL_CONFIG_2			0x12
#define		MT9P031_PLL_CONFIG_2_P1_48MHZ	5
#define		MT9P031_PLL_CONFIG_2_P1_96MHZ	2
#define MT9P031_PIXEL_CLOCK_CONTROL		0x0a
#define MT9P031_FRAME_RESTART			0x0b
#define MT9P031_SHUTTER_DELAY			0x0c
#define MT9P031_RST				0x0d
#define		MT9P031_RST_ENABLE		1
#define		MT9P031_RST_DISABLE		0
#define MT9P031_READ_MODE_1			0x1e
#define MT9P031_READ_MODE_2			0x20
#define		MT9P031_READ_MODE_2_ROW_MIR	0x8000
#define		MT9P031_READ_MODE_2_COL_MIR	0x4000
#define MT9P031_ROW_ADDRESS_MODE		0x22
#define MT9P031_COLUMN_ADDRESS_MODE		0x23
#define MT9P031_GLOBAL_GAIN			0x35

struct mt9p031 {
	struct v4l2_subdev subdev;
	struct media_pad pad;
	struct v4l2_rect rect;  /* Sensor window */
	struct v4l2_mbus_framefmt format;
	struct mt9p031_platform_data *pdata;
	struct mutex power_lock; /* lock to protect power_count */
	int power_count;
	u16 xskip;
	u16 yskip;
	/* cache register values */
	u16 output_control;
};

static struct mt9p031 *to_mt9p031(const struct i2c_client *client)
{
	return container_of(i2c_get_clientdata(client), struct mt9p031, subdev);
}

static int reg_read(struct i2c_client *client, const u8 reg)
{
	s32 data = i2c_smbus_read_word_data(client, reg);
	return data < 0 ? data : swab16(data);
}

static int reg_write(struct i2c_client *client, const u8 reg,
			const u16 data)
{
	return i2c_smbus_write_word_data(client, reg, swab16(data));
}

static int mt9p031_set_output_control(struct mt9p031 *mt9p031, u16 clear,
					u16 set)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9p031->subdev);
	u16 value = (mt9p031->output_control & ~clear) | set;
	int ret;

	ret = reg_write(client, MT9P031_OUTPUT_CONTROL, value);
	if (ret < 0)
		return ret;
	mt9p031->output_control = value;
	return 0;
}

static int mt9p031_reset(struct i2c_client *client)
{
	struct mt9p031 *mt9p031 = to_mt9p031(client);
	int ret;

	/* Disable chip output, synchronous option update */
	ret = reg_write(client, MT9P031_RST, MT9P031_RST_ENABLE);
	if (ret < 0)
		return ret;
	ret = reg_write(client, MT9P031_RST, MT9P031_RST_DISABLE);
	if (ret < 0)
		return ret;
	return mt9p031_set_output_control(mt9p031,
					MT9P031_OUTPUT_CONTROL_CEN, 0);
}

static int mt9p031_power_on(struct mt9p031 *mt9p031)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mt9p031->subdev);
	int ret;

	/* Ensure RESET_BAR is low */
	if (mt9p031->pdata->reset) {
		mt9p031->pdata->reset(&mt9p031->subdev, 1);
		msleep(1);
	}
	/* Emable clock */
	if (mt9p031->pdata->set_xclk)
		mt9p031->pdata->set_xclk(&mt9p031->subdev, MT9P031_EXTCLK_FREQ);
	/* Now RESET_BAR must be high */
	if (mt9p031->pdata->reset) {
		mt9p031->pdata->reset(&mt9p031->subdev, 0);
		msleep(1);
	}
	/* soft reset */
	ret = mt9p031_reset(client);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to reset the camera\n");
		return ret;
	}
	return 0;
}

static void mt9p031_power_off(struct mt9p031 *mt9p031)
{
	if (mt9p031->pdata->reset) {
		mt9p031->pdata->reset(&mt9p031->subdev, 1);
		msleep(1);
	}
	if (mt9p031->pdata->set_xclk)
		mt9p031->pdata->set_xclk(&mt9p031->subdev, 0);
}

static int mt9p031_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_fh *fh,
				struct v4l2_subdev_mbus_code_enum *code)
{
	struct mt9p031 *mt9p031 = container_of(sd, struct mt9p031, subdev);

	if (code->pad || code->index)
		return -EINVAL;

	code->code = mt9p031->format.code;
	return 0;
}

static struct v4l2_mbus_framefmt *mt9p031_get_pad_format(
	struct mt9p031 *mt9p031,
	struct v4l2_subdev_fh *fh,
	unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(fh, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &mt9p031->format;
	default:
		return NULL;
	}
}

static struct v4l2_rect *mt9p031_get_pad_crop(struct mt9p031 *mt9p031,
			struct v4l2_subdev_fh *fh, unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_crop(fh, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &mt9p031->rect;
	default:
		return NULL;
	}
}

static int mt9p031_get_crop(struct v4l2_subdev *sd,
				struct v4l2_subdev_fh *fh,
				struct v4l2_subdev_crop *crop)
{
	struct mt9p031 *mt9p031 = container_of(sd, struct mt9p031, subdev);
	struct v4l2_rect *rect = mt9p031_get_pad_crop(mt9p031, fh, crop->pad,
							crop->which);
	if (!rect)
		return -EINVAL;

	crop->rect = *rect;

	return 0;
}

static u16 mt9p031_skip_for_crop(s32 source, s32 *target, s32 max_skip)
{
	unsigned int skip;

	if (source - source / 4 < *target) {
		*target = source;
		return 1;
	}

	skip = DIV_ROUND_CLOSEST(source, *target);
	if (skip > max_skip)
		skip = max_skip;
	*target = 2 * DIV_ROUND_UP(source, 2 * skip);

	return skip;
}

static int mt9p031_set_params(struct i2c_client *client,
				struct v4l2_rect *rect, u16 xskip, u16 yskip)
{
	struct mt9p031 *mt9p031 = to_mt9p031(client);
	int ret;
	u16 xbin, ybin;
	const u16 hblank = MT9P031_H_BLANKING_VALUE,
		vblank = MT9P031_V_BLANKING_VALUE;
	__s32 left;

	/*
	* TODO: Attention! When implementing horizontal flipping, adjust
	* alignment according to R2 "Column Start" description in the datasheet
	*/
	if (xskip & 1) {
		xbin = 1;
		left = rect->left & (~3);
	} else if (xskip & 2) {
		xbin = 2;
		left = rect->left & (~7);
	} else {
		xbin = 4;
		left = rect->left & (~15);
	}
	ybin = min(yskip, (u16)4);

	/* Disable register update, reconfigure atomically */
	ret = mt9p031_set_output_control(mt9p031, 0,
					MT9P031_OUTPUT_CONTROL_SYN);
	if (ret < 0)
		return ret;

	dev_dbg(&client->dev, "skip %u:%u, rect %ux%u@%u:%u\n",
		xskip, yskip, rect->width, rect->height, rect->left, rect->top);

	/* Blanking and start values - default... */
	ret = reg_write(client, MT9P031_H_BLANKING, hblank);
	if (ret < 0)
		return ret;
	ret = reg_write(client, MT9P031_V_BLANKING, vblank);
	if (ret < 0)
		return ret;

	ret = reg_write(client, MT9P031_COLUMN_ADDRESS_MODE,
				((xbin - 1) << 4) | (xskip - 1));
	if (ret < 0)
		return ret;
	ret = reg_write(client, MT9P031_ROW_ADDRESS_MODE,
				((ybin - 1) << 4) | (yskip - 1));
	if (ret < 0)
		return ret;

	dev_dbg(&client->dev, "new physical left %u, top %u\n",
		rect->left, rect->top);

	ret = reg_write(client, MT9P031_COLUMN_START,
				rect->left);
	if (ret < 0)
		return ret;
	ret = reg_write(client, MT9P031_ROW_START,
				rect->top);
	if (ret < 0)
		return ret;

	ret = reg_write(client, MT9P031_WINDOW_WIDTH,
				rect->width - 1);
	if (ret < 0)
		return ret;
	ret = reg_write(client, MT9P031_WINDOW_HEIGHT,
				rect->height - 1);
	if (ret < 0)
		return ret;

	/* Re-enable register update, commit all changes */
	ret = mt9p031_set_output_control(mt9p031,
					MT9P031_OUTPUT_CONTROL_SYN, 0);
	if (ret < 0)
		return ret;

	mt9p031->xskip = xskip;
	mt9p031->yskip = yskip;
	return ret;
}

static int mt9p031_set_crop(struct v4l2_subdev *sd,
				struct v4l2_subdev_fh *fh,
				struct v4l2_subdev_crop *crop)
{
	struct mt9p031 *mt9p031 = container_of(sd, struct mt9p031, subdev);
	struct v4l2_mbus_framefmt *f;
	struct v4l2_rect *c;
	struct v4l2_rect rect;
	u16 xskip, yskip;
	s32 width, height;

	dev_dbg(mt9p031->subdev.v4l2_dev->dev, "%s(%ux%u@%u:%u : %u)\n",
			__func__, crop->rect.width, crop->rect.height,
			crop->rect.left, crop->rect.top, crop->which);

	/*
	* Clamp the crop rectangle boundaries and align them to a multiple of 2
	* pixels.
	*/
	rect.width = ALIGN(clamp(crop->rect.width,
				MT9P031_WINDOW_WIDTH_MIN,
				MT9P031_WINDOW_WIDTH_MAX), 2);
	rect.height = ALIGN(clamp(crop->rect.height,
				MT9P031_WINDOW_HEIGHT_MIN,
				MT9P031_WINDOW_HEIGHT_MAX), 2);
	rect.left = ALIGN(clamp(crop->rect.left,
				MT9P031_COLUMN_START_MIN,
				MT9P031_COLUMN_START_MAX), 2);
	rect.top = ALIGN(clamp(crop->rect.top,
				MT9P031_ROW_START_MIN,
				MT9P031_ROW_START_MAX), 2);

	c = mt9p031_get_pad_crop(mt9p031, fh, crop->pad, crop->which);

	if (rect.width != c->width || rect.height != c->height) {
		/*
		* Reset the output image size if the crop rectangle size has
		* been modified.
		*/
		f = mt9p031_get_pad_format(mt9p031, fh, crop->pad,
						crop->which);
		width = f->width;
		height = f->height;

		xskip = mt9p031_skip_for_crop(rect.width, &width, 7);
		yskip = mt9p031_skip_for_crop(rect.height, &height, 8);
	} else {
		xskip = mt9p031->xskip;
		yskip = mt9p031->yskip;
		f = NULL;
	}
	if (f) {
		f->width = width;
		f->height = height;
	}

	*c = rect;
	crop->rect = rect;

	mt9p031->xskip = xskip;
	mt9p031->yskip = yskip;
	mt9p031->rect = *c;
	return 0;
}

static int mt9p031_get_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_fh *fh,
				struct v4l2_subdev_format *fmt)
{
	struct mt9p031 *mt9p031 = container_of(sd, struct mt9p031, subdev);

	fmt->format =
		*mt9p031_get_pad_format(mt9p031, fh, fmt->pad, fmt->which);
	return 0;
}

static u16 mt9p031_skip_for_scale(s32 *source, s32 target,
					s32 max_skip, s32 max)
{
	unsigned int skip;

	if (*source - *source / 4 < target) {
		*source = target;
		return 1;
	}

	skip = min(max, *source + target / 2) / target;
	if (skip > max_skip)
		skip = max_skip;
	*source = target * skip;

	return skip;
}

static int mt9p031_set_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_fh *fh,
				struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *__format;
	struct v4l2_rect *__crop, rect;
	struct mt9p031 *mt9p031 = container_of(sd, struct mt9p031, subdev);
	unsigned int width;
	unsigned int height;
	u16 xskip, yskip;

	__crop = mt9p031_get_pad_crop(mt9p031, fh, format->pad, format->which);

	width = clamp_t(int, ALIGN(format->format.width, 2), 2,
						MT9P031_WINDOW_WIDTH_MAX);
	height = clamp_t(int, ALIGN(format->format.height, 2), 2,
						MT9P031_WINDOW_HEIGHT_MAX);

	rect.width = __crop->width;
	rect.height = __crop->height;

	xskip = mt9p031_skip_for_scale(&rect.width, width, 7,
				MT9P031_WINDOW_WIDTH_MAX);
	if (rect.width + __crop->left > MT9P031_WINDOW_WIDTH_MAX)
		rect.left = (MT9P031_WINDOW_WIDTH_MAX - rect.width) / 2;
	else
		rect.left = __crop->left;
	yskip = mt9p031_skip_for_scale(&rect.height, height, 8,
				MT9P031_WINDOW_HEIGHT_MAX);
	if (rect.height + __crop->top > MT9P031_WINDOW_HEIGHT_MAX)
		rect.top = (MT9P031_WINDOW_HEIGHT_MAX - rect.height) / 2;
	else
		rect.top = __crop->top;

	dev_dbg(mt9p031->subdev.v4l2_dev->dev, "%s(%ux%u : %u)\n", __func__,
		width, height, format->which);
	if (__crop)
		*__crop = rect;

	__format = mt9p031_get_pad_format(mt9p031, fh, format->pad,
						format->which);
	__format->width = width;
	__format->height = height;
	format->format = *__format;

	mt9p031->xskip = xskip;
	mt9p031->yskip = yskip;
	mt9p031->rect = *__crop;
	return 0;
}

static int mt9p031_pll_enable(struct i2c_client *client)
{
	struct mt9p031 *mt9p031 = to_mt9p031(client);
	int ret;

	ret = reg_write(client, MT9P031_PLL_CONTROL, MT9P031_PLL_CONTROL_PWRON);
	if (ret < 0)
		return ret;

	/* Always set the maximum frequency allowed by VDD_IO */
	if (mt9p031->pdata->vdd_io == MT9P031_VDD_IO_2V8) {
		ret = reg_write(client, MT9P031_PLL_CONFIG_1,
			MT9P031_PLL_CONFIG_1_M_96MHZ |
			MT9P031_PLL_CONFIG_1_N_96MHZ);
		if (ret < 0)
			return ret;
		ret = reg_write(client, MT9P031_PLL_CONFIG_2,
				MT9P031_PLL_CONFIG_2_P1_96MHZ);
		if (ret < 0)
			return ret;
	} else {
		ret = reg_write(client, MT9P031_PLL_CONFIG_1,
			MT9P031_PLL_CONFIG_1_M_48MHZ |
			MT9P031_PLL_CONFIG_1_N_48MHZ);
		if (ret < 0)
			return ret;
		ret = reg_write(client, MT9P031_PLL_CONFIG_2,
				MT9P031_PLL_CONFIG_2_P1_48MHZ);
		if (ret < 0)
			return ret;
	}
	mdelay(1);
	ret = reg_write(client, MT9P031_PLL_CONTROL,
			MT9P031_PLL_CONTROL_PWRON |
			MT9P031_PLL_CONTROL_USEPLL);
	mdelay(1);
	return ret;
}

static inline int mt9p031_pll_disable(struct i2c_client *client)
{
	return reg_write(client, MT9P031_PLL_CONTROL,
			 MT9P031_PLL_CONTROL_PWROFF);
}

static int mt9p031_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mt9p031 *mt9p031 = container_of(sd, struct mt9p031, subdev);
	struct i2c_client *client = v4l2_get_subdevdata(&mt9p031->subdev);
	struct v4l2_rect rect = mt9p031->rect;
	u16 xskip = mt9p031->xskip;
	u16 yskip = mt9p031->yskip;
	int ret;

	if (enable) {
		ret = mt9p031_set_params(client, &rect, xskip, yskip);
		if (ret < 0)
			return ret;
		/* Switch to master "normal" mode */
		ret = mt9p031_set_output_control(mt9p031, 0,
						MT9P031_OUTPUT_CONTROL_CEN);
		if (ret < 0)
			return ret;
		ret = mt9p031_pll_enable(client);
	} else {
		/* Stop sensor readout */
		ret = mt9p031_set_output_control(mt9p031,
						MT9P031_OUTPUT_CONTROL_CEN, 0);
		if (ret < 0)
			return ret;
		ret = mt9p031_pll_disable(client);
	}
	return ret;
}

static int mt9p031_video_probe(struct i2c_client *client)
{
	s32 data;

	/* Read out the chip version register */
	data = reg_read(client, MT9P031_CHIP_VERSION);
	if (data != MT9P031_CHIP_VERSION_VALUE) {
		dev_err(&client->dev,
			"No MT9P031 chip detected, register read %x\n", data);
		return -ENODEV;
	}

	dev_info(&client->dev, "Detected a MT9P031 chip ID %x\n", data);

	return 0;
}

static int mt9p031_set_power(struct v4l2_subdev *sd, int on)
{
	struct mt9p031 *mt9p031 = container_of(sd, struct mt9p031, subdev);
	int ret = 0;

	mutex_lock(&mt9p031->power_lock);

	/*
	* If the power count is modified from 0 to != 0 or from != 0 to 0,
	* update the power state.
	*/
	if (mt9p031->power_count == !on) {
		if (on) {
			ret = mt9p031_power_on(mt9p031);
			if (ret) {
				dev_err(mt9p031->subdev.v4l2_dev->dev,
				"Failed to power on: %d\n", ret);
				goto out;
			}
		} else {
			mt9p031_power_off(mt9p031);
		}
	}

	/* Update the power count. */
	mt9p031->power_count += on ? 1 : -1;
	WARN_ON(mt9p031->power_count < 0);

out:
	mutex_unlock(&mt9p031->power_lock);
	return ret;
}

static int mt9p031_registered(struct v4l2_subdev *sd)
{
	struct mt9p031 *mt9p031 = container_of(sd, struct mt9p031, subdev);
	struct i2c_client *client = v4l2_get_subdevdata(&mt9p031->subdev);
	int ret;

	ret = mt9p031_set_power(&mt9p031->subdev, 1);
	if (ret) {
		dev_err(&client->dev,
			"Failed to power on device: %d\n", ret);
		return ret;
	}

	ret = mt9p031_video_probe(client);

	mt9p031_set_power(&mt9p031->subdev, 0);

	return ret;
}

static int mt9p031_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct mt9p031 *mt9p031;
	mt9p031 = container_of(sd, struct mt9p031, subdev);

	mt9p031->rect.width     = MT9P031_WINDOW_WIDTH_DEF;
	mt9p031->rect.height    = MT9P031_WINDOW_HEIGHT_DEF;
	mt9p031->rect.left      = MT9P031_COLUMN_START_DEF;
	mt9p031->rect.top       = MT9P031_ROW_START_DEF;

	if (mt9p031->pdata->version == MT9P031_MONOCHROME_VERSION)
		mt9p031->format.code = V4L2_MBUS_FMT_Y12_1X12;
	else
		mt9p031->format.code = V4L2_MBUS_FMT_SGRBG12_1X12;

	mt9p031->format.width = MT9P031_WINDOW_WIDTH_DEF;
	mt9p031->format.height = MT9P031_WINDOW_HEIGHT_DEF;
	mt9p031->format.field = V4L2_FIELD_NONE;
	mt9p031->format.colorspace = V4L2_COLORSPACE_SRGB;

	mt9p031->xskip = 1;
	mt9p031->yskip = 1;
	return mt9p031_set_power(sd, 1);
}

static int mt9p031_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return mt9p031_set_power(sd, 0);
}

static struct v4l2_subdev_core_ops mt9p031_subdev_core_ops = {
	.s_power        = mt9p031_set_power,
};

static struct v4l2_subdev_video_ops mt9p031_subdev_video_ops = {
	.s_stream       = mt9p031_s_stream,
};

static struct v4l2_subdev_pad_ops mt9p031_subdev_pad_ops = {
	.enum_mbus_code = mt9p031_enum_mbus_code,
	.get_fmt = mt9p031_get_format,
	.set_fmt = mt9p031_set_format,
	.get_crop = mt9p031_get_crop,
	.set_crop = mt9p031_set_crop,
};

static struct v4l2_subdev_ops mt9p031_subdev_ops = {
	.core   = &mt9p031_subdev_core_ops,
	.video  = &mt9p031_subdev_video_ops,
	.pad    = &mt9p031_subdev_pad_ops,
};

static const struct v4l2_subdev_internal_ops mt9p031_subdev_internal_ops = {
	.registered = mt9p031_registered,
	.open = mt9p031_open,
	.close = mt9p031_close,
};

static int mt9p031_probe(struct i2c_client *client,
				const struct i2c_device_id *did)
{
	int ret;
	struct mt9p031 *mt9p031;
	struct mt9p031_platform_data *pdata = client->dev.platform_data;
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_WORD_DATA)) {
		dev_warn(&adapter->dev,
			"I2C-Adapter doesn't support I2C_FUNC_SMBUS_WORD\n");
		return -EIO;
	}

	mt9p031 = kzalloc(sizeof(struct mt9p031), GFP_KERNEL);
	if (!mt9p031)
		return -ENOMEM;

	mutex_init(&mt9p031->power_lock);
	v4l2_i2c_subdev_init(&mt9p031->subdev, client, &mt9p031_subdev_ops);
	mt9p031->subdev.internal_ops = &mt9p031_subdev_internal_ops;

	mt9p031->pdata          = pdata;

	mt9p031->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_init(&mt9p031->subdev.entity, 1, &mt9p031->pad, 0);
	if (ret)
		return ret;

	mt9p031->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	return 0;
}

static int mt9p031_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mt9p031 *mt9p031 = container_of(sd, struct mt9p031, subdev);

	v4l2_device_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	kfree(mt9p031);

	return 0;
}

static const struct i2c_device_id mt9p031_id[] = {
	{ "mt9p031", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mt9p031_id);

static struct i2c_driver mt9p031_i2c_driver = {
	.driver = {
		.name = "mt9p031",
	},
	.probe          = mt9p031_probe,
	.remove         = mt9p031_remove,
	.id_table       = mt9p031_id,
};

static int __init mt9p031_mod_init(void)
{
	return i2c_add_driver(&mt9p031_i2c_driver);
}

static void __exit mt9p031_mod_exit(void)
{
	i2c_del_driver(&mt9p031_i2c_driver);
}

module_init(mt9p031_mod_init);
module_exit(mt9p031_mod_exit);

MODULE_DESCRIPTION("Aptina MT9P031 Camera driver");
MODULE_AUTHOR("Bastian Hecht <hechtb@gmail.com>");
MODULE_LICENSE("GPL v2");
