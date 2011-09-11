/*
 * LCD panel driver for TFC S9700RTWV35TR-01B
 *
 * Copyright (C) 2011 Texas Instruments Inc
 * Author: Roger Monk <r-monk@ti.com>
 * From Original by : Vaibhav Hiremath <hvaibhav@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>

#include <plat/display.h>


static struct omap_video_timings tfc_timings = {
	.x_res		= 800,
	.y_res		= 480,

	.pixel_clock	= 30000,

	.hsw		= 49,
	.hfp		= 41,
	.hbp		= 40,

	.vsw		= 4,
	.vfp		= 14,
	.vbp		= 29,
};

static int tfc_panel_probe(struct omap_dss_device *dssdev)
{
	dssdev->panel.config = OMAP_DSS_LCD_TFT | OMAP_DSS_LCD_IVS |
		OMAP_DSS_LCD_IHS; // | OMAP_DSS_LCD_IEO; - TODO check this - doesn't work with this enabled
	dssdev->panel.acb = 0x0;
	dssdev->panel.timings = tfc_timings;

	return 0;
}

static void tfc_panel_remove(struct omap_dss_device *dssdev)
{
}

static int tfc_panel_enable(struct omap_dss_device *dssdev)
{
	int r = 0;

	if (dssdev->state == OMAP_DSS_DISPLAY_ACTIVE)
                return 0;

        r = omapdss_dpi_display_enable(dssdev);
        if (r)
                goto err0;

	/* wait couple of vsyncs until enabling the LCD */
	msleep(50);

	if (dssdev->platform_enable){
		r = dssdev->platform_enable(dssdev);
		if(r)
			goto err1;
	}

	return 0;
err1:	
	omapdss_dpi_display_disable(dssdev);
err0:
	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;
	return r;
}

static void tfc_panel_disable(struct omap_dss_device *dssdev)
{
        if (dssdev->state != OMAP_DSS_DISPLAY_ACTIVE)
                return;

	if (dssdev->platform_disable)
		dssdev->platform_disable(dssdev);

	/* wait at least 5 vsyncs after disabling the LCD */

	msleep(100);

	omapdss_dpi_display_disable(dssdev);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
}

static int tfc_panel_suspend(struct omap_dss_device *dssdev)
{
	tfc_panel_disable(dssdev);
	return 0;
}

static int tfc_panel_resume(struct omap_dss_device *dssdev)
{
	return tfc_panel_enable(dssdev);
}

static void tfc_panel_get_timings(struct omap_dss_device *dssdev,
		            struct omap_video_timings *timings)
{
	    *timings = dssdev->panel.timings;
}

static struct omap_dss_driver tfc_s9700_driver = {
	.probe		= tfc_panel_probe,
	.remove		= tfc_panel_remove,

	.enable		= tfc_panel_enable,
	.disable	= tfc_panel_disable,
	.suspend	= tfc_panel_suspend,
	.resume		= tfc_panel_resume,
	.get_timings = tfc_panel_get_timings,
	.driver         = {
		.name   = "tfc_s9700_panel",
		.owner  = THIS_MODULE,
	},
};

static int __init tfc_panel_drv_init(void)
{
	return omap_dss_register_driver(&tfc_s9700_driver);
}

static void __exit tfc_panel_drv_exit(void)
{
	omap_dss_unregister_driver(&tfc_s9700_driver);
}

module_init(tfc_panel_drv_init);
module_exit(tfc_panel_drv_exit);
MODULE_LICENSE("GPL");
