#ifndef MT9P031_H
#define MT9P031_H

struct v4l2_subdev;

enum {
	MT9P031_COLOR_VERSION = 0,
	MT9P031_MONOCHROME_VERSION = 1,
};

enum {
	MT9P031_VDD_IO_1V8 = 0,
	MT9P031_VDD_IO_2V8 = 1,
};

struct mt9p031_platform_data {
	int (*set_xclk)(struct v4l2_subdev *subdev, int hz);
	int (*reset)(struct v4l2_subdev *subdev, int active);
	int vdd_io; /* MT9P031_VDD_IO_1V8 or MT9P031_VDD_IO_2V8 */
	int version; /* MT9P031_COLOR_VERSION or MT9P031_MONOCHROME_VERSION */
};

#endif
