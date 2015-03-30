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
#include <asm/mach-types.h>
#include "star_edid.h"

#define SII_EDID_LEN	512
#define DRV_NAME 		"sii9134"
#define DET_RETRY_CNT	2

struct sii9134_data {
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
} sii9134;

static void sii9134_poweron(void);
static void sii9134_poweroff(void);


static __attribute__ ((unused)) void dump_regs(u8 reg, int len)
{
	u8 buf[50];
	int i;
    
	i2c_smbus_read_i2c_block_data(sii9134.client, reg, len, buf);
	for (i = 0; i < len; i++)
		dev_dbg(&sii9134.client->dev, "reg[0x%02X]: 0x%02X\n",
				i+reg, buf[i]);
}



static void sii9134_setup(struct fb_info *fbi)
{
    return;
}


static int sii9134_cable_connected(void)
{
    /* Power on sii902x */
    sii9134_poweron();

    return 0;
}

static void det_worker(struct work_struct *work)
{
    int dat;
    char event_string[16];
    char *envp[] = { event_string, NULL };

    dev_dbg(&sii9134.client->dev, "%s\n", __func__);

    dat = i2c_smbus_read_byte_data(sii9134.client, 0x3D);

    printk(KERN_ERR"status: %#X, sii9134.retries: %d\n", dat, sii9134.retries);
    if (dat >= 0) 
    {
        /* cable connection changes */
        if (dat & 0x4) 
        {
            sii9134.cable_plugin = 1;
            dev_dbg(&sii9134.client->dev, "EVENT=plugin\n");
            sprintf(event_string, "EVENT=plugin");
            if (sii9134_cable_connected() < 0 && sii9134.retries > 0)
            {
                sii9134.retries --;
                schedule_delayed_work(&(sii9134.det_work), msecs_to_jiffies(500));
            } 
            else 
            {
                sii9134.retries = 0;
            }
        } 
        else
        {
            sii9134.retries = 0;
            sii9134.cable_plugin = 0;
            dev_dbg(&sii9134.client->dev, "EVENT=plugout\n");
            sprintf(event_string, "EVENT=plugout");
            /* Power off sii9134 */
            sii9134_poweroff();
        }
        kobject_uevent_env(&sii9134.client->dev.kobj, KOBJ_CHANGE, envp);
    } 
    else 
    {
		dev_err(&sii9134.client->dev, "i2c bus error!!!\n");
		sii9134.retries = 0;
	}    
	dev_dbg(&sii9134.client->dev, "exit %s\n", __func__);
}

static irqreturn_t sii9134_detect_handler(int irq, void *data)
{
    if (sii9134.fbi) 
    {
        if (sii9134.retries == 0) 
        {
            /* no need to schedule workqueue if retries > 0 */
            sii9134.retries = DET_RETRY_CNT;
            schedule_delayed_work(&(sii9134.det_work), msecs_to_jiffies(100/*20*/));
        }
    } 
    else 
    {
        sii9134.waiting_for_fb = true;
    }
    
    return IRQ_HANDLED;
}

static int sii9134_fb_event(struct notifier_block *nb, unsigned long val, void *v)
{
	struct fb_event *event = v;
	struct fb_info *fbi = event->info;
	
	dev_dbg(&sii9134.client->dev, "%s event=0x%lx, \n", __func__, val);
	
	printk(KERN_INFO, "%s event=0x%lx, \n", __func__, val);
    
	switch (val) 
    {
	case FB_EVENT_FB_REGISTERED:
	if (sii9134.fbi == NULL) 
       {
			sii9134.fbi = fbi;
			if (sii9134.waiting_for_fb) 
            {
				sii9134.retries = DET_RETRY_CNT;
				sii9134.waiting_for_fb = false;
				sii9134_setup(fbi);
				schedule_delayed_work(&(sii9134.det_work), msecs_to_jiffies(20));
			}
		}
		fb_show_logo(fbi, 0);
		break;
	case FB_EVENT_MODE_CHANGE:
		printk(KERN_INFO"%s event=[%s]\n", __func__, "FB_EVENT_MODE_CHANGE");
		sii9134_setup(fbi);
		break;
	case FB_EVENT_BLANK:
		if (*((int *)event->data) == FB_BLANK_UNBLANK) 
        {
			dev_dbg(&sii9134.client->dev, "FB_BLANK_UNBLANK\n");
			sii9134_poweron();
		} 
        else
        {
			dev_dbg(&sii9134.client->dev, "FB_BLANK_BLANK\n");
			sii9134_poweroff();
		}
		break;
	}
	return 0;
}

static struct notifier_block nb = 
{
	.notifier_call = sii9134_fb_event,
};

static int sii9134_get_of_property(void)
{
    struct device_node *np = sii9134.client->dev.of_node;
    const char *mode_str;
    int bits_per_pixel, ret;

    ret = of_property_read_string(np, "mode_str", &mode_str);
    if (ret < 0) 
    {
        dev_warn(&sii9134.client->dev, "get of property mode_str fail\n");
        return ret;
    }
    ret = of_property_read_u32(np, "bits-per-pixel", &bits_per_pixel);
    if (ret)
    {
        dev_warn(&sii9134.client->dev, "get of property bpp fail\n");
        return ret;
    }
    
    sii9134.mode_str = mode_str;
    sii9134.bits_per_pixel = bits_per_pixel;

    return ret;
}

static int sii9134_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	int i, dat, ret;
    
	memset(&sii9134, 0, sizeof(sii9134));
    
    
	sii9134.client = client;
	printk(KERN_INFO "%s, addr:0x%x\n", __func__, sii9134.client->addr);
    
    i2c_smbus_write_byte_data(sii9134.client, 0x05, 0x01);  //soft reset    
    i2c_smbus_write_byte_data(sii9134.client, 0x05, 0x00);
    i2c_smbus_write_byte_data(sii9134.client, 0x08, 0xfd);  //PD#=1,power on mode
    #if 0
    i2c_smbus_write_byte_data(0x3d, 0x2f, 0x21);        //HDMI mode enable, 24bit per pixel(8 bits per channel; no packing)
    i2c_smbus_write_byte_data(0x3d, 0x3e, 0x03);        //Enable AVI infoFrame transmission, Enable(send in every VBLANK period)
    i2c_smbus_write_byte_data(0x3d, 0x40, 0x82);
    i2c_smbus_write_byte_data(0x3d, 0x41, 0x02);
    i2c_smbus_write_byte_data(0x3d, 0x42, 0x0d);
    i2c_smbus_write_byte_data(0x3d, 0x43, 0xf7);
    i2c_smbus_write_byte_data(0x3d, 0x44, 0x10);
    i2c_smbus_write_byte_data(0x3d, 0x45, 0x68);
    i2c_smbus_write_byte_data(0x3d, 0x46, 0x00);
    i2c_smbus_write_byte_data(0x3d, 0x47, 0x00);
    i2c_smbus_write_byte_data(0x3d, 0x3d, 0x07);
    #endif
    
    //display Device ID information
    dat = i2c_smbus_read_byte_data(sii9134.client, 0x02);
    
    printk(KERN_INFO "Sii9134: read device id1 = 0x%02X", dat);

    dat = i2c_smbus_read_byte_data(sii9134.client, 0x03);
    
    printk(KERN_INFO "Sii9134: read device id2 = 0x%02X", dat);
    
    if (sii9134.client->irq) 
    {
        ret = request_irq(sii9134.client->irq, sii9134_detect_handler, IRQF_TRIGGER_RISING, "SII9134_det", &sii9134);
        if (ret < 0)
            dev_warn(&sii9134.client->dev, "Sii9134: cound not request det irq %d\n", sii9134.client->irq);
        else 
        {
            INIT_DELAYED_WORK(&(sii9134.det_work), det_worker);
        }
    }
    
	sii9134_get_of_property();
	sii9134.waiting_for_fb = true;
	fb_register_client(&nb);
    
	return 0;
}

static int sii9134_remove(struct i2c_client *client)
{
	fb_unregister_client(&nb);
	sii9134_poweroff();
		
	return 0;
}

static void sii9134_poweron(void)
{
	printk(KERN_INFO "%s\n", __func__);
#if 0    
	/* Turn on DVI or HDMI */
	if (sii902x.edid_cfg.hdmi_cap)
		i2c_smbus_write_byte_data(sii902x.client, 0x1A, 0x01);
	else
		i2c_smbus_write_byte_data(sii902x.client, 0x1A, 0x00);
#endif
    return;
}

static void sii9134_poweroff(void)
{
	printk(KERN_INFO "%s\n", __func__);
	/* disable tmds before changing resolution */
#if 0
	if (sii902x.edid_cfg.hdmi_cap)
		i2c_smbus_write_byte_data(sii902x.client, 0x1A, 0x11);
	else
		i2c_smbus_write_byte_data(sii902x.client, 0x1A, 0x10);
#endif
	return;
}

static const struct i2c_device_id sii9134_id[] = {
	{ DRV_NAME, 0},
	{ },
};

MODULE_DEVICE_TABLE(i2c, sii9134_id);

static const struct of_device_id sii9134_dt_ids[] = {
	{ .compatible = DRV_NAME, },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, sii9134_dt_ids);

static struct i2c_driver sii9134_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = sii9134_dt_ids,
	},
	.probe		= sii9134_probe,
	.remove		= sii9134_remove,
	.id_table	= sii9134_id,
};

module_i2c_driver(sii9134_driver);

MODULE_AUTHOR("MYIR Tech, Inc.");
MODULE_DESCRIPTION("SII9134 DVI/HDMI driver");
MODULE_LICENSE("GPL");
