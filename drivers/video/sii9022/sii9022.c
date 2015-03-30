/*
 * Copyright (C) 2014-2018 MYIR Tech, Inc. All Rights Reserved.
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*!
 * @defgroup Framebuffer Framebuffer Driver for Sii902x.
 */

/*!
 * @file myir_sii902x.c
 *
 * @brief  Frame buffer driver for SII902X
 *
 * @ingroup Framebuffer
 */

/*!
 * Include files
 */
#define DEBUG	1

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/i2c.h>
#include <linux/fsl_devices.h>
#include <linux/interrupt.h>
#include <linux/reset.h>
#include <asm/mach-types.h>
#include "edid.h"

#define SII_EDID_LEN	512
#define DRV_NAME 		"sii9022"
#define DET_RETRY_CNT	2

struct sii9022_data {
	struct i2c_client *client;
	struct delayed_work det_work;
	struct fb_info *fbi;
	struct myir_edid_cfg edid_cfg;
	u8 cable_plugin;
	u8 edid[SII_EDID_LEN];
	bool waiting_for_fb;
	bool dft_mode_set;
	const char *mode_str;
	int bits_per_pixel;
	int retries;
} sii9022;

static void sii9022_poweron(void);
static void sii9022_poweroff(void);

#if defined(DEBUG) && defined(CONFIG_SII9022_EDID_READING)
static void dump_fb_videomode(struct fb_videomode *m)
{
	pr_debug("fb_videomode = %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
		m->refresh, m->xres, m->yres, m->pixclock, m->left_margin,
		m->right_margin, m->upper_margin, m->lower_margin,
		m->hsync_len, m->vsync_len, m->sync, m->vmode, m->flag);
}
#endif

static __attribute__ ((unused)) void dump_regs(u8 reg, int len)
{
	u8 buf[50];
	int i;

	i2c_smbus_read_i2c_block_data(sii9022.client, reg, len, buf);
	for (i = 0; i < len; i++)
		dev_dbg(&sii9022.client->dev, "reg[0x%02X]: 0x%02X\n",
				i+reg, buf[i]);
}

static ssize_t sii902x_show_name(struct device *dev,
		struct device_attribute *attr, char *buf)
{

	strcpy(buf, sii9022.fbi->fix.id);
	sprintf(buf+strlen(buf), "\n");

	return strlen(buf);
}

static DEVICE_ATTR(fb_name, S_IRUGO, sii902x_show_name, NULL);

static ssize_t sii902x_show_state(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	if (sii9022.cable_plugin == 0)
		strcpy(buf, "plugout\n");
	else
		strcpy(buf, "plugin\n");

	return strlen(buf);
}

static DEVICE_ATTR(cable_state, S_IRUGO, sii902x_show_state, NULL);

static ssize_t sii902x_show_edid(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int len = 0;
#ifdef CONFIG_SII9022_EDID_READING
	int i, j;
	for (j = 0; j < SII_EDID_LEN/16; j++) {
		for (i = 0; i < 16; i++)
			len += sprintf(buf+len, "0x%02X ",
					sii9022.edid[j*16 + i]);
		len += sprintf(buf+len, "\n");
	}
#else
	len = sprintf(buf, "EDID reading is not supported.\n");
#endif
	return len;
}

static DEVICE_ATTR(edid, S_IRUGO, sii902x_show_edid, NULL);

static void sii9022_setup(struct fb_info *fbi)
{
	u16 data[4];
	u32 refresh;
	u8 *tmp;
	int i;

	dev_dbg(&sii9022.client->dev, "Sii9022: setup..\n");

	/* Power up */
	i2c_smbus_write_byte_data(sii9022.client, 0x1E, 0x00);

	/* set TPI video mode */
	data[0] = PICOS2KHZ(fbi->var.pixclock) / 10;
	data[2] = fbi->var.hsync_len + fbi->var.left_margin +
		  fbi->var.xres + fbi->var.right_margin;
	data[3] = fbi->var.vsync_len + fbi->var.upper_margin +
		  fbi->var.yres + fbi->var.lower_margin;
	refresh = data[2] * data[3];
	refresh = (PICOS2KHZ(fbi->var.pixclock) * 1000) / refresh;
	data[1] = refresh * 100;
	tmp = (u8 *)data;
	for (i = 0; i < 8; i++)
		i2c_smbus_write_byte_data(sii9022.client, i, tmp[i]);

	/* input bus/pixel: full pixel wide (24bit), rising edge */
	i2c_smbus_write_byte_data(sii9022.client, 0x08, 0x70);
	/* Set input format to RGB */
	i2c_smbus_write_byte_data(sii9022.client, 0x09, 0x00);
	/* set output format to RGB */
	i2c_smbus_write_byte_data(sii9022.client, 0x0A, 0x00);
	/* audio setup */
	i2c_smbus_write_byte_data(sii9022.client, 0x25, 0x00);
	i2c_smbus_write_byte_data(sii9022.client, 0x26, 0x40);
	i2c_smbus_write_byte_data(sii9022.client, 0x27, 0x00);
}

#ifdef CONFIG_SII9022_EDID_READING
static int sii9022_read_edid(struct fb_info *fbi)
{
	int old, dat, ret, cnt = 100;
	unsigned short addr = 0x50;

	dev_dbg(&sii9022.client->dev, "%s\n", __func__);

	old = i2c_smbus_read_byte_data(sii9022.client, 0x1A);

	i2c_smbus_write_byte_data(sii9022.client, 0x1A, old | 0x4);
	do {
		cnt--;
		msleep(10);
		dat = i2c_smbus_read_byte_data(sii9022.client, 0x1A);
	} while ((!(dat & 0x2)) && cnt);

	if (!cnt) {
		ret = -1;
		goto done;
	}

	i2c_smbus_write_byte_data(sii9022.client, 0x1A, old | 0x06);

	/* edid reading */
	ret = edid_read(sii9022.client->adapter, addr,
				sii9022.edid, &sii9022.edid_cfg, fbi);

	printk(KERN_ERR"myir_edid_read: ret=%d", ret);

	cnt = 100;
	do {
		cnt--;
		i2c_smbus_write_byte_data(sii9022.client, 0x1A, old & ~0x6);
		msleep(10);
		dat = i2c_smbus_read_byte_data(sii9022.client, 0x1A);
	} while ((dat & 0x6) && cnt);

	if (!cnt) {
		ret = -1;
	}

done:

	i2c_smbus_write_byte_data(sii9022.client, 0x1A, old);
	return ret;
}

static int sii9022_cable_connected(void)
{
	int i;
	const struct fb_videomode *mode;
	struct fb_videomode m;
	int ret;

	ret = sii9022_read_edid(sii9022.fbi);
	if (ret < 0) {
		dev_err(&sii9022.client->dev,
			"Sii902x: read edid fail\n");
		/* Power on sii9022 */
		sii9022_poweron();
	} else {
		if (sii9022.fbi->monspecs.modedb_len > 0) {

			fb_destroy_modelist(&sii9022.fbi->modelist);
            
			for (i = 0; i < sii9022.fbi->monspecs.modedb_len; i++) {

				mode = &sii9022.fbi->monspecs.modedb[i];

				if (!(mode->vmode & FB_VMODE_INTERLACED)) {

					dev_dbg(&sii9022.client->dev, "Added mode %d:", i);
					dev_dbg(&sii9022.client->dev,
						"xres = %d, yres = %d, freq = %d, vmode = %d, flag = %d\n",
						mode->xres, mode->yres, mode->refresh,
						mode->vmode, mode->flag);

					fb_add_videomode(mode, &sii9022.fbi->modelist);
				}
			}

			/* Set the default mode only once. */
			if (!sii9022.dft_mode_set &&
					sii9022.mode_str && sii9022.bits_per_pixel) {

				dev_dbg(&sii9022.client->dev, "%s: setting to default=%s bpp=%d\n",
						__func__, sii9022.mode_str, sii9022.bits_per_pixel);

				fb_find_mode(&sii9022.fbi->var, sii9022.fbi,
						sii9022.mode_str, NULL, 0, NULL,
						sii9022.bits_per_pixel);

				sii9022.dft_mode_set = true;
			}

			fb_var_to_videomode(&m, &sii9022.fbi->var);
			dump_fb_videomode(&m);

			mode = fb_find_nearest_mode(&m,
					&sii9022.fbi->modelist);

			/* update fbi mode  */
			sii9022.fbi->mode = (struct fb_videomode *)mode;

			fb_videomode_to_var(&sii9022.fbi->var, mode);

			sii9022.fbi->var.activate |= FB_ACTIVATE_FORCE;
			console_lock();
			sii9022.fbi->flags |= FBINFO_MISC_USEREVENT;
			fb_set_var(sii9022.fbi, &sii9022.fbi->var);
			sii9022.fbi->flags &= ~FBINFO_MISC_USEREVENT;
			console_unlock();
		}
		/* Power on sii9022 */
		sii902x_poweron();
	}
	return ret;
}
#else
static int sii9022_cable_connected(void)
{
	/* Power on sii9022 */
	sii9022_poweron();
	
	return 0;
}
#endif /* CONFIG_SII902X_EDID_READING */

static void det_worker(struct work_struct *work)
{
	int dat;
	char event_string[16];
	char *envp[] = { event_string, NULL };

	dev_dbg(&sii9022.client->dev, "%s\n", __func__);

	dat = i2c_smbus_read_byte_data(sii9022.client, 0x3D);

	printk(KERN_ERR"status: %#X, sii9022.retries: %d\n", dat, sii9022.retries);
//	if ((dat & 0x1) || sii9022.retries > 0) {
	if (dat >= 0) {
		/* cable connection changes */
		if (dat & 0x4) {
			sii9022.cable_plugin = 1;
			dev_dbg(&sii9022.client->dev, "EVENT=plugin\n");
			sprintf(event_string, "EVENT=plugin");
			if (sii9022_cable_connected() < 0 && sii9022.retries > 0) {
				sii9022.retries --;
				schedule_delayed_work(&(sii9022.det_work), msecs_to_jiffies(500));
			} else {
				sii9022.retries = 0;
			}
		} else {
			sii9022.retries = 0;
			sii9022.cable_plugin = 0;
			dev_dbg(&sii9022.client->dev, "EVENT=plugout\n");
			sprintf(event_string, "EVENT=plugout");
			/* Power off sii9022 */
			sii9022_poweroff();
		}
		kobject_uevent_env(&sii9022.client->dev.kobj, KOBJ_CHANGE, envp);
	} else {
		dev_err(&sii9022.client->dev, "i2c bus error!!!\n");
		sii9022.retries = 0;
	}
	i2c_smbus_write_byte_data(sii9022.client, 0x3D, dat);

	dev_dbg(&sii9022.client->dev, "exit %s\n", __func__);
}

static irqreturn_t sii9022_detect_handler(int irq, void *data)
{
	if (sii9022.fbi) {
		if (sii9022.retries == 0) {/* no need to schedule workqueue if retries > 0 */
			sii9022.retries = DET_RETRY_CNT;
			schedule_delayed_work(&(sii9022.det_work), msecs_to_jiffies(100/*20*/));
		}
	} else {
		sii9022.waiting_for_fb = true;
	}

	return IRQ_HANDLED;
}

static int sii9022_fb_event(struct notifier_block *nb, unsigned long val, void *v)
{
	struct fb_event *event = v;
	struct fb_info *fbi = event->info;
	
	dev_dbg(&sii9022.client->dev, "%s event=0x%lx, \n", __func__, val);
		
	switch (val) {
	case FB_EVENT_FB_REGISTERED:
	if (sii9022.fbi == NULL) {
			sii9022.fbi = fbi;
			if (sii9022.waiting_for_fb) {
				sii9022.retries = DET_RETRY_CNT;
				sii9022.waiting_for_fb = false;
				sii9022_setup(fbi);
				schedule_delayed_work(&(sii9022.det_work), msecs_to_jiffies(20));
			}
		}
		fb_show_logo(fbi, 0);
		break;
	case FB_EVENT_MODE_CHANGE:
		printk(KERN_DEBUG"%s event=[%s]\n", __func__, "FB_EVENT_MODE_CHANGE");
		sii9022_setup(fbi);
		break;
	case FB_EVENT_BLANK:
		if (*((int *)event->data) == FB_BLANK_UNBLANK) {
			dev_dbg(&sii9022.client->dev, "FB_BLANK_UNBLANK\n");
			sii9022_poweron();
		} else {
			dev_dbg(&sii9022.client->dev, "FB_BLANK_BLANK\n");
			sii9022_poweroff();
		}
		break;
	}
	return 0;
}

static struct notifier_block nb = {
	.notifier_call = sii9022_fb_event,
};

static int get_of_property(void)
{
	struct device_node *np = sii9022.client->dev.of_node;
	const char *mode_str;
	int bits_per_pixel, ret;

	ret = of_property_read_string(np, "mode_str", &mode_str);
	if (ret < 0) {
		dev_warn(&sii9022.client->dev, "get of property mode_str fail\n");
		return ret;
	}
	ret = of_property_read_u32(np, "bits-per-pixel", &bits_per_pixel);
	if (ret) {
		dev_warn(&sii9022.client->dev, "get of property bpp fail\n");
		return ret;
	}

	sii9022.mode_str = mode_str;
	sii9022.bits_per_pixel = bits_per_pixel;

	return ret;
}

static int sii9022_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	int i, dat, ret;
#ifdef CONFIG_SII9022_EDID_READING
	struct fb_info edid_fbi;
#endif
	memset(&sii9022, 0, sizeof(sii9022));
    
	sii9022.client = client;

	dev_dbg(&sii9022.client->dev, "%s\n", __func__);;

	/* Recommend to reset sii9022 here, not yet implemented */
	
	/* Set 902x in hardware TPI mode on and jump out of D3 state */
	if (i2c_smbus_write_byte_data(sii9022.client, 0xc7, 0x00) < 0) {
		dev_err(&sii9022.client->dev,
			"Sii902x: cound not find device\n");
		return -ENODEV;
	}
    
	/* read device ID */
	for (i = 10; i > 0; i--) {
		dat = i2c_smbus_read_byte_data(sii9022.client, 0x1B);
		printk(KERN_DEBUG "Sii902x: read id = 0x%02X", dat);
		if (dat == 0xb0) {
			dat = i2c_smbus_read_byte_data(sii9022.client, 0x1C);
			printk(KERN_DEBUG "-0x%02X", dat);
			dat = i2c_smbus_read_byte_data(sii9022.client, 0x1D);
			printk(KERN_DEBUG "-0x%02X", dat);
			dat = i2c_smbus_read_byte_data(sii9022.client, 0x30);
			printk(KERN_DEBUG "-0x%02X\n", dat);
			break;
		}
	}
	if (i == 0) {
		dev_err(&sii9022.client->dev,
			"Sii902x: cound not find device\n");
		return -ENODEV;
	}

#ifdef CONFIG_SII9022_EDID_READING
	/* try to read edid */
	ret = sii9022_read_edid(&edid_fbi);
	if (ret < 0)
		dev_warn(&sii9022.client->dev, "Can not read edid\n");
#else
	sii9022.edid_cfg.hdmi_cap = 1;
#endif
		
	if (sii9022.client->irq) {
		ret = request_irq(sii9022.client->irq, sii9022_detect_handler,
				/*IRQF_TRIGGER_FALLING*/IRQF_TRIGGER_RISING,
				"SII902x_det", &sii9022);
		if (ret < 0)
			dev_warn(&sii9022.client->dev,
				"Sii902x: cound not request det irq %d\n",
				sii9022.client->irq);
		else {
			/*enable cable hot plug irq*/
			i2c_smbus_write_byte_data(sii9022.client, 0x3c, 0x01);
			INIT_DELAYED_WORK(&(sii9022.det_work), det_worker);
		}
		ret = device_create_file(&sii9022.client->dev, &dev_attr_fb_name);
		if (ret < 0)
			dev_warn(&sii9022.client->dev,
				"Sii902x: cound not create sys node for fb name\n");
		ret = device_create_file(&sii9022.client->dev, &dev_attr_cable_state);
		if (ret < 0)
			dev_warn(&sii9022.client->dev,
				"Sii902x: cound not create sys node for cable state\n");
		ret = device_create_file(&sii9022.client->dev, &dev_attr_edid);
		if (ret < 0)
			dev_warn(&sii9022.client->dev,
				"Sii902x: cound not create sys node for edid\n");

	}

	get_of_property();
	sii9022.waiting_for_fb = true;
	fb_register_client(&nb);

	return 0;
}

static int sii9022_remove(struct i2c_client *client)
{
	fb_unregister_client(&nb);
	sii9022_poweroff();
	
	device_remove_file(&client->dev, &dev_attr_fb_name);
	device_remove_file(&client->dev, &dev_attr_cable_state);
	device_remove_file(&client->dev, &dev_attr_edid);
	
	return 0;
}

static void sii9022_poweron(void)
{
	printk(KERN_INFO "%s\n", __func__);
	/* Turn on DVI or HDMI */
	if (sii9022.edid_cfg.hdmi_cap)
		i2c_smbus_write_byte_data(sii9022.client, 0x1A, 0x01);
	else
		i2c_smbus_write_byte_data(sii9022.client, 0x1A, 0x00);
	return;
}

static void sii9022_poweroff(void)
{
	printk(KERN_INFO "%s\n", __func__);
	/* disable tmds before changing resolution */
	if (sii9022.edid_cfg.hdmi_cap)
		i2c_smbus_write_byte_data(sii9022.client, 0x1A, 0x11);
	else
		i2c_smbus_write_byte_data(sii9022.client, 0x1A, 0x10);

	return;
}

static const struct i2c_device_id sii9022_id[] = {
	{ DRV_NAME, 0},
	{ },
};
MODULE_DEVICE_TABLE(i2c, sii9022_id);

static const struct of_device_id sii9022_dt_ids[] = {
	{ .compatible = DRV_NAME, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sii9022_dt_ids);

static struct i2c_driver sii9022_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = sii9022_dt_ids,
	},
	.probe		= sii9022_probe,
	.remove		= sii9022_remove,
	.id_table	= sii9022_id,
};

module_i2c_driver(sii9022_driver);

MODULE_AUTHOR("MYIR Tech, Inc.");
MODULE_DESCRIPTION("SII9022 DVI/HDMI driver");
MODULE_LICENSE("GPL");
