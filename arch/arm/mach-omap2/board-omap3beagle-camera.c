#include <linux/gpio.h>
#include <linux/regulator/machine.h>

#include <plat/i2c.h>

#include <media/mt9p031.h>
#include <asm/mach-types.h>
#include "devices.h"
#include "../../../drivers/media/video/omap3isp/isp.h"

#define MT9P031_RESET_GPIO	98
#define MT9P031_XCLK		ISP_XCLK_A

static struct regulator *reg_1v8, *reg_2v8;

static int beagle_cam_set_xclk(struct v4l2_subdev *subdev, int hz)
{
	struct isp_device *isp = v4l2_dev_to_isp_device(subdev->v4l2_dev);
	int ret;

	ret = isp->platform_cb.set_xclk(isp, hz, MT9P031_XCLK);
	return 0;
}

static int beagle_cam_reset(struct v4l2_subdev *subdev, int active)
{
	/* Set RESET_BAR to !active */
	gpio_set_value(MT9P031_RESET_GPIO, !active);

	return 0;
}

static struct mt9p031_platform_data beagle_mt9p031_platform_data = {
	.set_xclk               = beagle_cam_set_xclk,
	.reset                  = beagle_cam_reset,
	.vdd_io			= MT9P031_VDD_IO_1V8,
	.version		= MT9P031_COLOR_VERSION,
};

static struct i2c_board_info mt9p031_camera_i2c_device = {
	I2C_BOARD_INFO("mt9p031", 0x48),
	.platform_data = &beagle_mt9p031_platform_data,
};

static struct isp_subdev_i2c_board_info mt9p031_camera_subdevs[] = {
	{
		.board_info = &mt9p031_camera_i2c_device,
		.i2c_adapter_id = 2,
	},
	{ NULL, 0, },
};

static struct isp_v4l2_subdevs_group beagle_camera_subdevs[] = {
	{
		.subdevs = mt9p031_camera_subdevs,
		.interface = ISP_INTERFACE_PARALLEL,
		.bus = {
				.parallel = {
					.data_lane_shift = 0,
					.clk_pol = 1,
					.bridge = ISPCTRL_PAR_BRIDGE_DISABLE,
				}
		},
	},
	{ },
};

static struct isp_platform_data beagle_isp_platform_data = {
	.subdevs = beagle_camera_subdevs,
};

static int __init beagle_camera_init(void)
{
	if (!machine_is_omap3_beagle() || !cpu_is_omap3630())
		return 0;

	reg_1v8 = regulator_get(NULL, "cam_1v8");
	if (IS_ERR(reg_1v8))
		pr_err("%s: cannot get cam_1v8 regulator\n", __func__);
	else
		regulator_enable(reg_1v8);

	reg_2v8 = regulator_get(NULL, "cam_2v8");
	if (IS_ERR(reg_2v8))
		pr_err("%s: cannot get cam_2v8 regulator\n", __func__);
	else
		regulator_enable(reg_2v8);

	omap_register_i2c_bus(2, 100, NULL, 0);
	gpio_request(MT9P031_RESET_GPIO, "cam_rst");
	gpio_direction_output(MT9P031_RESET_GPIO, 0);
	omap3_init_camera(&beagle_isp_platform_data);
	return 0;
}
late_initcall(beagle_camera_init);
