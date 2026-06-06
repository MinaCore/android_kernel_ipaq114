/**
 * Support for the PXA311 and PXA312 based Samsung SGH devices
 * 		m480, i780, i900, i904, i908, i910
 *
 * Copyright (C) 2009 Sacha Refshauge <xsacha@gmail.com>
 * Copyright (C) 2009 Stefan Schmidt <stefan@datenfreihafen.org>
 * Copyright (C) 2009 Mustafa Ozsakalli <ozsakalli@hotmail.com>
 *
 * Based on zylonite.c Copyright (C) 2006 Marvell International Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/pwm_backlight.h>
#include <linux/power_supply.h>
#include <linux/pda_power.h>
#include <linux/spi/spi.h>
#include <linux/spi/libertas_spi.h>
#include <../drivers/staging/android/timed_gpio.h>

#include <plat/i2c.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <mach/hardware.h>
#include <mach/pxafb.h>
#include <mach/audio.h>
#include <mach/mmc.h>
#include <mach/udc.h>
#include <mach/ohci.h>
#include <mach/pxa27x-udc.h>
#include <mach/pxa27x_keypad.h>
#include <mach/pxa2xx_spi.h>
#include <mach/pxa3xx-regs.h>
#include <mach/mfp-pxa300.h>
#include <linux/leds.h>
#include <linux/leds_pwm.h>
#include <linux/mfd/core.h>
#include <linux/mfd/ds1wm.h>
#include <linux/clk.h>
#if defined(CONFIG_PXA_DVFM)
#include <mach/dvfm.h>
#include <mach/pxa3xx_dvfm.h> 
#include <mach/pmu.h>
#endif

#include <mach/sgh_msm6k.h>

#include "devices.h"
#include "generic.h"

#define SGH_BATT_I2C_SLAVE_ADDRESS		0x34

#define GPIO09_SGH_LED_GREEN		  	  9
#define GPIO23_SGH_TOUCHSCREEN		 	 23
#define GPIO71_SGH_LED_BLUE		 	 71
#define GPIO79_SGH_LED_VIBRATE		 	 79
#define GPIO88_SGH_BATT_CHARGE		 	 88
#define GPIO104_SGH_WIFI_CMD		   	104
#define GPIO105_SGH_CARD_DETECT		   	105

#define GPIO18_SGH_I780_WIFI_CMD	 	 11
#define GPIO19_SGH_I780_SPK_AUDIO	 	 19
#define GPIO48_SGH_I780_LED_BACKLIGHT		 48
#define GPIO75_SGH_I780_LED_RED		 	 75
#define GPIO94_SGH_I780_WIFI_POWER	 	 94

#define GPIO03_SGH_I900_WIFI_POWER	  	  3
#define GPIO17_SGH_I900_SPK_AUDIO	 	 17
#define GPIO48_SGH_I900_LED_RED		 	 48
#define GPIO76_SGH_I900_BT_POWER	 	 76
#define GPIO118_SGH_I900_WIFI_CMD	   	118

#define GPIO16_SGH_SPI_CHIP_SEL	 	 	 16

/********************************************************
* DS1WM (1Wire Bus Master)                              *
********************************************************/
extern struct platform_device hpipaq114_device_ds1wm;

static int hpipaq114_ds1wm_enable(struct platform_device *pdev)
{
	printk("Enabling DS1WM (CLK on)\n");

	struct clk *clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk)) return PTR_ERR(clk);
	clk_enable(clk);

	return 0;
}

static int hpipaq114_ds1wm_disable(struct platform_device *pdev)
{
	printk("Disabling DS1WM (CLK off)\n");

	struct clk *clk = clk_get(&pdev->dev, NULL);
	if (!IS_ERR(clk)) clk_disable(clk);

	return 0;
}


static struct ds1wm_driver_data hpipaq114_ds1wm_info = {
	.clock_rate 	= 28000000, 
	.active_high	= 1,
};

static struct resource hpipaq114_resource_ds1wm[] = {
	[0] = {
		.start	= 0x41b00000,
		.end	= 0x41b00014,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start	= IRQ_1WIRE,
		.end	= IRQ_1WIRE,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct mfd_cell hpipaq114_ds1wm_cell = {
	.name          = "ds1wm",
	.enable        = hpipaq114_ds1wm_enable,
	.disable       = hpipaq114_ds1wm_disable,
//	.suspend)(struct platform_device *dev);
//	.resume)(struct platform_device *dev);
	.driver_data   = &hpipaq114_ds1wm_info,
	.resources	= hpipaq114_resource_ds1wm,
	.num_resources	= ARRAY_SIZE(hpipaq114_resource_ds1wm),
};

struct platform_device hpipaq114_device_ds1wm = {
	.name		= "ds1wm",
	.id		= -1,
	.resource	= hpipaq114_resource_ds1wm,
	.num_resources	= ARRAY_SIZE(hpipaq114_resource_ds1wm),
	.dev		=  {
	.platform_data  = &hpipaq114_ds1wm_cell,
	}
};

static void __init hpipaq114_init_ds1wm(void)
{
	mdelay(200);
	platform_device_register(&hpipaq114_device_ds1wm);
}

/****************************
* AC Power Supply           *
****************************/
#if defined(CONFIG_PDA_POWER) || defined(CONFIG_PDA_POWER_MODULES)
#define GPIO_DOK 	91	/* ! DC-In OK (MAX8677) */
#define GPIO_CEN 	120	/* ! Charge Enable (MAX8677) */
#define GPIO_CHG 	105	/* Charging (MAX8677) */
#define GPIO_PREQ 	106	/* Prequal (MAX8677) */
#define GPIO_BATT_CONN 	107	/* Battery Connected (??) */
//NOTE: CEN pulled low in LPM by MFP CFG.
static int hpipaq114_power_init(struct device *dev)
{
	int ret;
	ret = gpio_request(GPIO_DOK, "!DC-in OK");
	ret += gpio_request(GPIO_CEN, "!Charge Enable");
/*	ret += gpio_request(GPIO_CHG, "Charging");
	ret += gpio_request(GPIO_PREQ, "Prequal charge");
	ret += gpio_request(GPIO_BATT_CONN, "Battery Connected"); */
	
	ret += gpio_direction_input(GPIO_DOK);
	ret += gpio_direction_output(GPIO_CEN, 0); //leave ther charger always on
/*	ret += gpio_direction_input(GPIO_CHG);
	ret += gpio_direction_input(GPIO_PREQ);
	ret += gpio_direction_input(GPIO_BATT_CONN);*/
	printk(KERN_ERR "power_init: ret=%d CEN=%d DOK=%d\n", ret, gpio_get_value(GPIO_CEN), gpio_get_value(GPIO_DOK));
	return ret;
}
static void hpipaq114_power_exit(struct device *dev)
{
	gpio_free(GPIO_DOK);
	gpio_free(GPIO_CEN);
/*	gpio_free(GPIO_CHG);
	gpio_free(GPIO_PREQ);
	gpio_free(GPIO_BATT_CONN);	*/
}
static int hpipaq114_power_ac_online(void)
{
	return (gpio_get_value(GPIO_DOK) == 0);
}
static char *hpipaq114_ac_supplied_to[] = {
	"battery",
};
static struct pda_power_pdata hpipaq114_power_data = {
	.init			= hpipaq114_power_init,
	.is_ac_online		= hpipaq114_power_ac_online,
	.exit			= hpipaq114_power_exit,
	.supplied_to		= hpipaq114_ac_supplied_to,
	.num_supplicants	= ARRAY_SIZE(hpipaq114_ac_supplied_to),
};
static struct resource hpipaq114_power_resource[] = {
	{
		.name		= "ac",
		.start		= gpio_to_irq(GPIO_DOK),
		.end		= gpio_to_irq(GPIO_DOK),
		.flags		= IORESOURCE_IRQ |
				  IORESOURCE_IRQ_HIGHEDGE |
				  IORESOURCE_IRQ_LOWEDGE,
	},
};
static struct platform_device hpipaq114_power_device = {
	.name			= "pda-power",
	.id			= -1,
	.dev.platform_data	= &hpipaq114_power_data,
	.resource		= hpipaq114_power_resource,
	.num_resources		= ARRAY_SIZE(hpipaq114_power_resource),
};
static void __init hpipaq114_init_power(void)
{
	int ret;
	hpipaq114_power_resource[0].start = gpio_to_irq(GPIO_DOK);
	hpipaq114_power_resource[0].end   = gpio_to_irq(GPIO_DOK);
	ret = platform_device_register(&hpipaq114_power_device);	
	if (ret)
		printk(KERN_ERR "unable to register pda_power device\n");
}
#else
static inline void hpipaq214_init_power(void) {}
#endif /* CONFIG_PDA_POWER || CONFIG_PDA_POWER_MODULES */


#if defined(CONFIG_LEDS_GPIO) || defined(CONFIG_LEDS_GPIO_MODULE)
static struct gpio_led sgh_leds[] = {
	[0] = {
		.name			= "red",
	},
	[1] = {
		.name			= "green",
		.default_trigger	= "mmc0",
		.gpio			= GPIO09_SGH_LED_GREEN,
	},
	[2] = {
		.name			= "blue",
		.default_trigger	= "mmc0",
		.gpio			= GPIO71_SGH_LED_BLUE,
	},
	[3] = {
		.name			= "keyboard",
	},

};

static struct gpio_led_platform_data sgh_leds_info = {
	.leds		= sgh_leds,
	.num_leds	= ARRAY_SIZE(sgh_leds),
};

static struct platform_device sgh_device_leds = {
	.name		= "leds-gpio",
	.id		= -1,
	.dev		= {
		.platform_data = &sgh_leds_info,
	}
};

static struct timed_gpio sgh_vibrator = {
         .name                   = "vibrator",
         .gpio                   = GPIO79_SGH_LED_VIBRATE,
         .max_timeout            = 1000,
};

static struct timed_gpio_platform_data sgh_vibrator_info = {
        .gpios          = &sgh_vibrator,
        .num_gpios      = 1,
};

static struct platform_device sgh_device_vibrator = {
        .name           = "timed-gpio",
        .id             = -1,
        .dev            = {
                .platform_data = &sgh_vibrator_info,
        }
};

static void __init sgh_init_leds(void)
{

	sgh_leds[0].gpio = (machine_is_sgh_i780()) ? GPIO75_SGH_I780_LED_RED : GPIO48_SGH_I900_LED_RED;
	if(machine_is_sgh_i780())
		sgh_leds[3].gpio = GPIO48_SGH_I780_LED_BACKLIGHT;

	platform_device_register(&sgh_device_leds);
	//timed_gpio doesnt request gpio
	gpio_request(GPIO79_SGH_LED_VIBRATE, "SGH-VIBRATOR");
	platform_device_register(&sgh_device_vibrator);

}
#else
static inline void sgh_init_leds(void) {}
#endif

static struct led_pwm sgh_pwm_leds[] = {
    {
        .name          = "lcd-backlight",  // ← именно это ищет Android
        .pwm_id        = 2,
        .max_brightness = 255,
        .pwm_period_ns = 10000,
    },
};
static struct led_pwm_platform_data sgh_pwm_data = {
    .num_leds = ARRAY_SIZE(sgh_pwm_leds),
    .leds     = sgh_pwm_leds,
};

static struct platform_device sgh_backlight_device = {
    .name = "leds_pwm",
    .id   = -1,
    .dev  = {
        .parent        = &pxa27x_device_pwm0.dev,
        .platform_data = &sgh_pwm_data,
    },
};
extern void pwm_backlight_update_status(struct backlight_device *bl); 

/* Pixclock Calculation
 Calculated from reviewing HaRET source: http://xanadux.cvs.sourceforge.net/viewvc/xanadux/haret/haret-gnu/src/script.cpp?view=markup
 pixclock = K * 8MHz / CLK ;   where CLK is 312MHz and K is last 8 bits of lccr3

 New: pixclock = (K * 200000000) / 15600
*/
static struct pxafb_mode_info sgh_i780_mode = {
        .pixclock		= 256500,	// K = 19
        .xres			= 240,		// HACK: Android does not like square resolutions
        .yres			= 320,
        .bpp			= 16,
        .hsync_len		= 4,
        .left_margin		= 20,
        .right_margin		= 20,
        .vsync_len		= 1,
        .upper_margin		= 7,
        .lower_margin   	= 6,
        .sync          	 = 0,
};
static struct pxafb_mode_info sgh_i900_mode = {
	.pixclock		= 256500,	// K = 20
	.xres			= 240,
	.yres			= 320,
	.bpp			= 16,
	.hsync_len		= 4,
	.left_margin		= 20,
	.right_margin		= 20,
	.vsync_len		= 1,
	.upper_margin		= 7,
	.lower_margin		= 6,
	.sync			= 0, //FB_SYNC_VERT_HIGH_ACT,
};

static struct pxafb_mach_info sgh_lcd_info = {
	.num_modes		= 1,
	.lcd_conn		= LCD_COLOR_TFT_16BPP | LCD_PCLK_EDGE_FALL,
	.lccr4			= 9,
};

static void __init sgh_init_lcd(void)
{
	platform_device_register(&sgh_backlight_device);
	sgh_lcd_info.modes = (machine_is_sgh_i780()) ? &sgh_i780_mode : &sgh_i900_mode;
	set_pxa_fb_info(&sgh_lcd_info);
}

/****************************
* Keypad                    *
****************************/

/*Android (i.e. non-linux) keys:
Name:            defined as:   function:
KEY_SEND         231           Send key
KEY_END          107           End key
KEY_BACK         158           Go back a page
KEY_MENU         139           Open a special menu
KEY_HOME         102           Return to the home screen
KEY_SEARCH       217           Open the Android search
KEY_VOLUMEUP     115           Increase volume
KEY_VOLUMEDOWN   114           Decrease volume
KEY_CAMERA       212           Opens camera
KEY_CAMERAFOCUS  211           Focuses camera (Omnia only, replaces KEY_HP in kernel/include/linux/input.h)
*/

#if defined(CONFIG_KEYBOARD_PXA27x) || defined(CONFIG_KEYBOARD_PXA27x_MODULE)
/* KEY(row, col, key_code) */
static unsigned int sgh_i780_matrix_key_map[] = {
/* QWERTY Keyboard */
/* 1st row */
/* 2nd row */
KEY(0, 1, KEY_A), KEY(7, 2, KEY_S), KEY(2, 1, KEY_D), KEY(3, 1, KEY_F), KEY(4, 1, KEY_G),
KEY(0, 5, KEY_H), KEY(1, 5, KEY_J), KEY(2, 5, KEY_K), KEY(3, 5, KEY_L), KEY(4, 5, KEY_BACKSPACE),
/* 3rd row */
KEY(0, 2, KEY_LEFTALT), KEY(1, 2, KEY_Z), KEY(2, 2, KEY_X), KEY(3, 2, KEY_C), KEY(4, 2, KEY_V),
KEY(0, 6, KEY_B), KEY(1, 6, KEY_N), KEY(2, 6, KEY_M), KEY(3, 6, KEY_DOT), KEY(4, 6, KEY_ENTER),
/* 4th row */
KEY(0, 3, KEY_LEFTSHIFT), KEY(1, 3, KEY_RIGHTALT), KEY(2, 3, KEY_0), KEY(3, 3, KEY_SPACE),
KEY(4, 3, KEY_COMMA), KEY(7, 6, KEY_SLASH), /* Message */ KEY(5, 1, KEY_TAB), /* GPS */

/* Volume Keys */
KEY(1, 0, KEY_VOLUMEUP),
KEY(1, 1, KEY_VOLUMEDOWN),

/* Left Softkey */      /* Windows Key */       /* OK */        /* Right Softkey */
KEY(5, 4, KEY_MINUS), KEY(5, 2, KEY_MENU), KEY(5, 3, KEY_EXIT),  KEY(5, 6, KEY_F2),
KEY(5, 5, KEY_SEND),            KEY(6, 4, KEY_REPLY),           KEY(7, 0, KEY_END),
/* Green Key */                  /* Center */                   /* Red Key */

/* Camera */
KEY(7, 3, KEY_CAMERA),
};

static unsigned int sgh_i900_matrix_key_map[] = {
	/* KEY(row, col, key_code) */
		//Camera half-press
	KEY(1, 1, KEY_VOLUMEUP),	//Volume up
	KEY(3, 0, KEY_END),
	KEY(2, 0, KEY_VOLUMEDOWN),	//Volume down
	KEY(0, 2, KEY_MENU),		//Top right key (Main Menu button)		//???
	KEY(1, 0, KEY_BACK),		//End key (Back button)
	KEY(1, 2, KEY_LEFT),
        KEY(2, 1, KEY_UP),
        KEY(2, 2, KEY_RIGHT),
        KEY(3, 1, KEY_DOWN),
        KEY(3, 2, KEY_ENTER),
       
};

static struct pxa27x_keypad_platform_data sgh_keypad_info = {
	.enable_rotary0		= 0,

	.debounce_interval	= 30,
};

static void __init sgh_init_keypad(void)
{
	if(machine_is_sgh_i780())
	{
		sgh_keypad_info.matrix_key_rows = 8;
		sgh_keypad_info.matrix_key_cols = 7;
		sgh_keypad_info.matrix_key_map = sgh_i780_matrix_key_map;
		sgh_keypad_info.matrix_key_map_size = ARRAY_SIZE(sgh_i780_matrix_key_map);
	}
	else
	{
		sgh_keypad_info.matrix_key_rows = 4;
		sgh_keypad_info.matrix_key_cols = 3;
		sgh_keypad_info.matrix_key_map = sgh_i900_matrix_key_map;
		sgh_keypad_info.matrix_key_map_size = ARRAY_SIZE(sgh_i900_matrix_key_map);
	}

	pxa_set_keypad_info(&sgh_keypad_info);
}
#else
static inline void sgh_init_keypad(void) {}
#endif

#if defined(CONFIG_MMC)
static int sgh_mci_sdcard_init(struct device *dev,
			     irq_handler_t sgh_detect_int,
			     void *data)
{
	int err, cd_irq;
	int gpio_cd = GPIO105_SGH_CARD_DETECT;

	cd_irq = gpio_to_irq(gpio_cd);

	/*
	 * setup GPIO for MMC controller
	 */
	err = gpio_request(gpio_cd, "microSD card detect");
	if (err)
		goto err_request_cd;
	gpio_direction_input(gpio_cd);

	err = request_irq(cd_irq, sgh_detect_int,
			  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			  "microSD card detect", data);
	if (err) {
		printk(KERN_ERR "%s: MicroSD: "
				"can't request card detect IRQ\n", __func__);
		goto err_request_cd;
	}

	return 0;

err_request_cd:
	return err;
}

static void sgh_mci_sdcard_exit(struct device *dev, void *data)
{
	int cd_irq, gpio_cd;

	cd_irq = gpio_to_irq(105);
	gpio_cd = 105;

	free_irq(cd_irq, data);
	gpio_free(gpio_cd);
}

static struct pxamci_platform_data sgh_mci_sdcard_platform_data = {
	.detect_delay	= 20,
	.ocr_mask	= MMC_VDD_32_33|MMC_VDD_33_34,
	.init		= sgh_mci_sdcard_init,
	.exit		= sgh_mci_sdcard_exit,
	.gpio_card_detect	= -1,
	.gpio_card_ro		= -1,
	.gpio_power		= -1,
};


static void __init sgh_init_mmc(void)
{
	pxa_set_mci_info(&sgh_mci_sdcard_platform_data);		// External MicroSD
	if(machine_is_sgh_i900())
		pxa3xx_set_mci2_info(&sgh_mci_sdcard_platform_data);	// Internal MicroSD
}
#else
static inline void sgh_init_mmc(void) {}
#endif
static void sgh_udc_command(int cmd)
{
        switch (cmd) {
        case PXA2XX_UDC_CMD_CONNECT:
                //UP2OCR |=  UP2OCR_HXOE | UP2OCR_DPPUE | UP2OCR_DPPUBE;
		UP2OCR |= 0xf024;	// USB Port 2 Output Control Register
                break;
        case PXA2XX_UDC_CMD_DISCONNECT:
                //UP2OCR &= ~(UP2OCR_HXOE | UP2OCR_DPPUE | UP2OCR_DPPUBE);
		UP2OCR &= 0xf024;
                break;
        }
}
static struct pxa2xx_udc_mach_info sgh_udc_info __initdata = {
        .udc_command            = sgh_udc_command,
};
	/* WinMo: UHCHR_SSEP2 | UHCHR_SSEP1 | UHCHR_SSE | UHCHR_CGR | UHCHR_FHR
           Set the Power Control Polarity Low */
/*        UHCHR = (UHCHR | UHCHR_PCPL) &
                ~(UHCHR_SSEP1 | UHCHR_SSEP2 | UHCHR_SSE);
*/
static int sgh_init_udc(void)
{
        pxa_set_udc_info(&sgh_udc_info);
        return 0;
}

#if defined(CONFIG_USB_OHCI_HCD) || defined(CONFIG_USB_OHCI_HCD_MODULE)
static int sgh_ohci_init(struct device *dev)
{
	return 0;
}
static struct pxaohci_platform_data sgh_ohci_platform_data = {
	.port_mode	= PMM_PERPORT_MODE,
	.init		= sgh_ohci_init
};

static void __init sgh_init_ohci(void)
{
	pxa_set_ohci_info(&sgh_ohci_platform_data);
}
#else
static inline void sgh_init_ohci(void) {}
#endif /* CONFIG_USB_OHCI_HCD || CONFIG_USB_OHCI_HCD_MODULE */

#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
static struct i2c_board_info __initdata sgh_i2c_board_info[] = {
        { /* PM6558 Battery */
                .type = "sgh_battery",
                .addr = SGH_BATT_I2C_SLAVE_ADDRESS,
	},
};
static void __init sgh_init_i2c(void)
{
	i2c_register_board_info(0, sgh_i2c_board_info,
                                ARRAY_SIZE(sgh_i2c_board_info));
        pxa_set_i2c_info(NULL);
}
#else
static inline void sgh_init_i2c(void) {}
#endif

#if defined(CONFIG_SPI_PXA2XX) || defined(CONFIG_SPI_PXA2XX_MASTER)
static void sgh_spi_wifi_cs(u32 command)
{
	gpio_set_value(GPIO16_SGH_SPI_CHIP_SEL, !(command == PXA2XX_CS_ASSERT));
}

static int sgh_libertas_setup(struct spi_device *spi)
{
	int WifiPwr = 0;
	int WifiCmd = 0;
	if(machine_is_sgh_i780())
	{
		WifiPwr = GPIO94_SGH_I780_WIFI_POWER;
		WifiCmd = GPIO18_SGH_I780_WIFI_CMD;
	}
	else if(machine_is_sgh_i900())
	{
		WifiPwr = GPIO03_SGH_I900_WIFI_POWER;
		WifiCmd = GPIO118_SGH_I900_WIFI_CMD;
	}
	gpio_request(WifiPwr,"WLAN");
	gpio_request(0x10,"WLAN");
	gpio_request(0x68,"WLAN");
	gpio_request(WifiCmd,"WLAN");

	//pxa_init_hw
	gpio_direction_output(0x68,1);
	gpio_direction_output(WifiCmd,1);
	gpio_direction_output(WifiPwr,1);
	gpio_direction_output(0x10,1);
	mdelay(60);

	gpio_set_value(WifiPwr,1);
	mdelay(60);
	gpio_set_value(WifiCmd,1);
	gpio_set_value(0x10,1);
	gpio_set_value(0x68,1);
	mdelay(60);


	//gspx_power_up
	gpio_set_value(WifiPwr,1);
	mdelay(60);
	gpio_set_value(WifiCmd,1);
	gpio_set_value(0x68,1);
	mdelay(150);

	//gspx_reset_module
	gpio_set_value(0x68,1);
	mdelay(60);
	gpio_set_value(0x68,0);
	mdelay(60);
	gpio_set_value(0x68,1);
	mdelay(100);

	spi->bits_per_word = 16;
	spi_setup(spi);

	return 0;
}

static struct pxa2xx_spi_chip sgh_wifi_chip = {
	.rx_threshold	= 8,
	.tx_threshold	= 8,
	.timeout	= 235,
	.dma_burst_size = 16,
	.cs_control	= sgh_spi_wifi_cs,
};

static struct pxa2xx_spi_master sgh_spi_info = {
	.clock_enable 	= CKEN_SSP1,
	.num_chipselect	= 1,
	.enable_dma	= 1,
};

struct libertas_spi_platform_data sgh_wifi_pdata = {
	.use_dummy_writes	= 0,
	.setup			= sgh_libertas_setup,
};

static struct spi_board_info sgh_spi_devices[] __initdata = {
	{	//wireless
		.modalias		= "libertas_spi",
		.max_speed_hz		= 13000000,
		.bus_num		= 1,
		.irq			= IRQ_GPIO(8),
		.chip_select		= 0,
		.controller_data	= &sgh_wifi_chip,
		.platform_data		= &sgh_wifi_pdata,
	},
};

static void __init sgh_init_spi(void)
{
	sgh_spi_devices[0].irq = IRQ_GPIO(machine_is_sgh_i780() ? 11 : 8);
	pxa2xx_set_spi_info(1, &sgh_spi_info);
	spi_register_board_info(ARRAY_AND_SIZE(sgh_spi_devices));
}
#else
static inline void sgh_init_spi(void){}
#endif

#if defined(CONFIG_PXA_DVFM)
struct pxa3xx_freq_mach_info sgh_freq_mach_info = {
	.flags = 0,
}; 

static void __init sgh_init_dvfm() {
	set_pxa3xx_freq_info(&sgh_freq_mach_info);
	pxa3xx_set_pmu_info(NULL);
}
#else
static inline void sgh_init_dvfm(void){}
#endif

static mfp_cfg_t sgh_mfp_cfg[] __initdata = {
        /* AC97 */
	//GPIO23_AC97_nACRESET,
        GPIO25_AC97_SDATA_IN_0,
        GPIO27_AC97_SDATA_OUT,
        GPIO28_AC97_SYNC,
        GPIO29_AC97_BITCLK,

	/* KEYPAD */
	GPIO115_KP_MKIN_0 | MFP_LPM_EDGE_BOTH,
	GPIO116_KP_MKIN_1 | MFP_LPM_EDGE_BOTH,
	GPIO117_KP_MKIN_2 | MFP_LPM_EDGE_BOTH,
	GPIO121_KP_MKOUT_0,
	GPIO122_KP_MKOUT_1,
	GPIO123_KP_MKOUT_2,
	GPIO124_KP_MKOUT_3,

	/* BACKLIGHT */
	GPIO19_PWM2_OUT,
	
	// Charging GPIOs (MAX8677)
	GPIO91_GPIO,                           // !DOK - DC/USB power ok
	GPIO120_GPIO | MFP_LPM_DRIVE_LOW,     // !CEN - charge enable, low in LPM
	GPIO107_GPIO,                          // Battery present
	GPIO126_OW_DQ | MFP_LPM_FLOAT,        // 1-Wire DS2760
	
};

static struct platform_device sgh_audio = {
        .name           = "sgh-asoc",
        .id             = -1,
};

static struct platform_device *devices[] __initdata = {
	&sgh_audio,
};

int wm9713_power_gpio = 104;

void hpipaq114_audio_suspend(void *priv){
        if(wm9713_power_gpio >= 0){ //didn't get the GPIO, can't do anything
                printk(KERN_INFO "hpipaq114_audio_suspend(), gpio%i -> off",wm9713_power_gpio);
                gpio_set_value(wm9713_power_gpio, 0);
        }
}

void hpipaq114_audio_resume(void *priv){
        if(wm9713_power_gpio >= 0){ //didn't get the GPIO, can't do anything
                printk(KERN_INFO "hpipaq114_audio_resume, gpio%i -> on",wm9713_power_gpio);
                gpio_set_value(wm9713_power_gpio, 1);
        }
}

static pxa2xx_audio_ops_t hpipaq114_audio_ops = {
        .suspend = hpipaq114_audio_suspend,
        .resume = hpipaq114_audio_resume,
};

static void __init hpipaq114_init_audio(void)
{
        int ret;

        ret = gpio_request(wm9713_power_gpio, "WM9713 power?"); 
        if(ret){
                printk(KERN_ERR "Unable to register WM9713/audio gpio (%i)\n",wm9713_power_gpio);
                wm9713_power_gpio = -1;
        }else
                gpio_direction_output(wm9713_power_gpio, 1); //keep it on for now!

        printk("hpipaq114_init_audio: %x\n",(unsigned int)&hpipaq114_audio_ops);

        pxa_set_ac97_info(&hpipaq114_audio_ops);

}

static void __init sgh_init(void)
{
	static int dvfm = 0;

	pxa3xx_mfp_config(ARRAY_AND_SIZE(sgh_mfp_cfg));
	sgh_init_dvfm();

	rpc_init();

	sgh_init_lcd();
	sgh_init_mmc();
	sgh_init_leds();
	sgh_init_keypad();
	hpipaq114_init_ds1wm();
	hpipaq114_init_power();
	hpipaq114_init_audio();
	platform_add_devices(devices, ARRAY_SIZE(devices));

	sgh_init_ohci();
	sgh_init_udc();
	sgh_init_i2c();
	sgh_init_spi();
	gpio_request(83, "backlight-autobrightness");
	gpio_direction_output(83, 0);
	/*
	dvfm_register("Test", &dvfm);
	dvfm_disable_op_name("D1", dvfm);
	dvfm_disable_op_name("D2", dvfm);
	*/
}

MACHINE_START(SGH_I780, "Samsung SGH-i780 (Mirage) phone")
        .phys_io        = 0x40000000,
        .boot_params    = 0xa0000100,
        .io_pg_offst    = (io_p2v(0x40000000) >> 18) & 0xfffc,
        .map_io         = pxa_map_io,
        .init_irq       = pxa3xx_init_irq,
        .timer          = &pxa_timer,
        .init_machine   = sgh_init,
MACHINE_END

MACHINE_START(SGH_I900, "Samsung SGH-i900 (Omnia) phone")
	.phys_io	= 0x40000000,
	.boot_params	= 0xa0000100,
	.io_pg_offst	= (io_p2v(0x40000000) >> 18) & 0xfffc,
	.map_io		= pxa_map_io,
	.init_irq	= pxa3xx_init_irq,
	.timer		= &pxa_timer,
	.init_machine	= sgh_init,
MACHINE_END
