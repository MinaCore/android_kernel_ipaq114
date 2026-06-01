/*
 * Samsung I780/I900 battery driver
 *
 * Copyright (C) 2009 Sacha Refshauge <xsacha@gmail.com>
 *
 * Based on DQ27x00 battery driver:
 * Copyright (C) 2008 Rodolfo Giometti <giometti@linux.it>
 * Copyright (C) 2008 Eurotech S.p.A. <info@eurotech.it>
 *
 * which was based on a previous work by Copyright (C) 2008 Texas Instruments, Inc.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */
#include <linux/module.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <linux/gpio.h>

#define DRIVER_VERSION			"1.0.1"

#define SGH_CHARGE_GPIO			88

#define SGH_BATT_REG_TEMP		0x25
#define SGH_BATT_REG_VDIFF		0x23
#define SGH_BATT_REG_VOLT		0x21

/* Reg Table
This is what is known of the register banks in PM6558 by
observation. Assumed that all registers are WORDs, so
address increases by 2. Also assumed that all registers
are 12-bit right justified (& 0xFFF).

Register	Task		Value
0x21		Voltage		The voltage
0x23		Charging	True if charging, False if not charging
0x25		Temperature	The temperature (which is used to determine charge)
0xC2		Shutdown	Write-only regisiter

*/

struct sgh_batt_device_info;
struct sgh_batt_access_methods {
	int (*read)(u8 reg, int *rt_value, int b_single,
		struct sgh_batt_device_info *di);
};
struct sgh_batt_device_info {
	struct device 		*dev;
	int			id;
	int			voltage_uV;
	int			current_uA;
	int			temp_C;
	int			charge_rsoc;
	struct sgh_batt_access_methods	*bus;
	struct power_supply	bat;
	struct power_supply	bat_ac;
	struct power_supply	bat_usb;
	
	struct i2c_client	*client;

	struct delayed_work 	work;
	struct workqueue_struct *wqueue;

	int			voltage;
	int			voltage_sum;
	int			voltage_count;
	int			capacity;
	int 			charging_status;
	int			poll_count;
};


static enum power_supply_property sgh_batt_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_BATT_VOL,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_BATT_TEMP,
};

static enum power_supply_property sgh_batt_power_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int sgh_batt_read(u8 reg, int *rt_value, struct sgh_batt_device_info *di)
{
	struct i2c_client *client = di->client;

	*rt_value = be16_to_cpu(i2c_smbus_read_word_data(client, reg));
	*rt_value = *rt_value & 0xFFF;

	return 0;
}

/*
 * Return the battery voltage in millivolts
 *
 */
static int sgh_batt_get_voltage(struct sgh_batt_device_info *di)
{
	int i;
	int voltages[5];
	int voltage = 0, largest = 0, smallest = 0;

	for(i = 0; i < 5; i++)
	{
		sgh_batt_read(SGH_BATT_REG_VOLT, &voltages[i], di);
		if (voltages[i] > voltages[largest])
			largest = i;

		if (voltages[i] < voltages[smallest])
			smallest = i;
	}
	for(i = 0; i < 5; i++)
	{
		if (i != smallest && i != largest)
			voltage += voltages[i];
	}

	voltage /= 3;

	if(di->voltage_count < 10) {
		di->voltage_sum += voltage;
		di->voltage_count++;
		voltage = di->voltage_sum / di->voltage_count;
	} else {
		di->voltage_sum = di->voltage_sum - di->voltage + voltage;
		voltage = di->voltage_sum / 10;
	}

	return voltage;
}

/*
 * Return the battery temperature in (10x) Celcius degrees.
 *
 * From Windows Mobile:
 * Temp Sample [ Min: 0x21B, 0x368, 0x89e : Max]
 *                    539    872    2206
 */
static int sgh_batt_get_temp(struct sgh_batt_device_info *di)
{
	int temp = 0;

	sgh_batt_read(SGH_BATT_REG_TEMP, &temp, di);

	return temp >> 2;
}

/*
 * Return the battery charge in percentage.
 */
static int sgh_batt_get_charge(struct sgh_batt_device_info *di)
{
	int volt = di->voltage;
	int i, k = 0, d = 10;
	int ndist, tdist;
	int vsamp[] = {0xe38, 0xdb6, 0xd66, 0xd25, 0xce4, 0xc94, 0xb79};
	// Charging applies a greater voltage. USB: ~0x30 AC: ~0x60
        // volt -=  be16_to_cpu(i2c_smbus_read_word_data(di->client, SGH_BATT_REG_VDIFF)) & 0xFFF;	// FIXME

	/* Use voltage to work out charge.
	   Closer to 100%, the voltage has less impact on gradient (linear).
	   Whereas closer to 0%, it is purely the gradient.
	 */
	for (i = 6; i >= 0; i--)
	{
		if (volt < vsamp[i])
		{
			switch (i) {
			case 0:
				k = k + 1;
			case 1:
				k = k + 1;
			case 2:
				k = k + 1;
			case 3:
				d = d >> 1;
			case 4:
				k = k + 1;
			case 5:
				ndist = 100 * (volt - vsamp[i+1]);
				tdist = (vsamp[i] - vsamp[i+1]);
				volt = (k * 100) + (ndist / tdist);
				return volt / d;
			default:
				return 0;
			}
		}
	}
	return 100;
}

#define to_sgh_batt_device_info(x) container_of((x), \
				struct sgh_batt_device_info, bat);

static int sgh_batt_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct sgh_batt_device_info *di = to_sgh_batt_device_info(psy);
	switch (psp) {

	case POWER_SUPPLY_PROP_BATT_VOL:
		val->intval = sgh_batt_get_voltage(di);
		break;

	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = true; // Device can't run without it
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = di->capacity;
		break;
	case POWER_SUPPLY_PROP_BATT_TEMP:
		val->intval = sgh_batt_get_temp(di);
		break;

	case POWER_SUPPLY_PROP_STATUS:
		val->intval = di->charging_status ? POWER_SUPPLY_STATUS_CHARGING : POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
 	case POWER_SUPPLY_PROP_HEALTH:
                val->intval = POWER_SUPPLY_HEALTH_GOOD;
                break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int sgh_batt_power_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct sgh_batt_device_info *di = to_sgh_batt_device_info(psy);
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		if (psy->type == POWER_SUPPLY_TYPE_MAINS)
			val->intval = (di->charging_status == 1) ? 1 : 0;
		else if (psy->type == POWER_SUPPLY_TYPE_USB)
			val->intval = (di->charging_status == 2) ? 1 : 0;
		else	val->intval = 0;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void sgh_batt_battery_update(struct power_supply *psy)
{
	int charging_status;
	struct sgh_batt_device_info *di = to_sgh_batt_device_info(psy);
	charging_status = di->charging_status;

	di->poll_count++;
	if(gpio_get_value(SGH_CHARGE_GPIO)) {
		di->charging_status = 0; //not charging
	} else {
		di->charging_status = 1; //ac
		//TODO: Detect usb
	}

	if(di->charging_status != charging_status || di->poll_count >= 5) {
		if(di->charging_status != charging_status)
			di->voltage_sum = di->voltage_count = 0;

		di->voltage = sgh_batt_get_voltage(di);
		di->capacity = sgh_batt_get_charge(di);

		printk("pwr: V:%x C:%d\n",di->voltage,di->capacity);

		power_supply_changed(psy);
		di->poll_count = 0;
	}

	/*
	di->charging_status = gpio_get_value(SGH_CHARGE_GPIO) ?
                        POWER_SUPPLY_STATUS_NOT_CHARGING :
                        POWER_SUPPLY_STATUS_CHARGING;
	*/
/*
	if (di->charging_status != charging_status)
	{
		di->reset_avg = 1;
		di->poll_count = 0xff;
	}

	if(di->poll_count >= 10) {
		di->poll_count = 0;
		power_supply_changed(psy);
	}
*/
}

static void sgh_batt_battery_work(struct work_struct *work)
{
	struct sgh_batt_device_info *di = container_of(work, struct sgh_batt_device_info, work.work);

	sgh_batt_battery_update(&di->bat);
	queue_delayed_work(di->wqueue, &di->work, HZ*5);
}

static char *supply_list[] = {
	"battery",
};
 
static void sgh_powersupply_init(struct sgh_batt_device_info *di) {
	di->bat.type = POWER_SUPPLY_TYPE_BATTERY;
        di->bat.properties = sgh_batt_battery_props;
        di->bat.num_properties = ARRAY_SIZE(sgh_batt_battery_props);
        di->bat.get_property = sgh_batt_battery_get_property;
        di->bat.external_power_changed = NULL;
}

static void sgh_powersupply_power_init(struct power_supply *bat,int is_usb) {
	bat->name = is_usb ? "usb" : "ac";
	bat->type = is_usb ? POWER_SUPPLY_TYPE_USB : POWER_SUPPLY_TYPE_MAINS;
	bat->supplied_to = supply_list;
	bat->num_supplicants = ARRAY_SIZE(supply_list);
	bat->properties = sgh_batt_power_props;
	bat->num_properties = ARRAY_SIZE(sgh_batt_power_props);
	bat->get_property = sgh_batt_power_get_property;
}

static int sgh_batt_battery_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	struct sgh_batt_device_info *di;
	struct sgh_batt_access_methods *bus;
	int retval = 0;

        retval = gpio_request(SGH_CHARGE_GPIO, "BATT CHRG");
	if (retval)
		goto batt_failed_0;

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		dev_err(&client->dev, "failed to allocate device info data\n");
		retval = -ENOMEM;
		return retval;
	}

	bus = kzalloc(sizeof(*bus), GFP_KERNEL);
	if (!bus) {
		dev_err(&client->dev, "failed to allocate access method "
					"data\n");
		retval = -ENOMEM;
		goto batt_failed_1;
	}

	i2c_set_clientdata(client, di);
	di->dev = &client->dev;
	di->bat.name = "battery";	// Android only looks for this
	di->bus = bus;
	di->client = client;
	di->poll_count = 0;
	sgh_powersupply_init(di);
	retval = power_supply_register(&client->dev, &di->bat);
	if (retval) {
		dev_err(&client->dev, "failed to register battery\n");
		goto batt_failed_2;
	}

	sgh_powersupply_power_init(&di->bat_ac,0);
	retval = power_supply_register(&client->dev, &di->bat_ac);
	if (retval) {
		dev_err(&client->dev, "failed to register battery (ac)\n");
		goto batt_failed_2;
	}

	sgh_powersupply_power_init(&di->bat_usb,1);
	retval = power_supply_register(&client->dev, &di->bat_usb);
	if (retval) {
		dev_err(&client->dev, "failed to register battery (usb)\n");
		goto batt_failed_2;
	}

	INIT_DELAYED_WORK(&di->work, sgh_batt_battery_work);
	di->wqueue = create_singlethread_workqueue("battery");
	queue_delayed_work(di->wqueue, &di->work, 1);

	dev_info(&client->dev, "support ver. %s enabled\n", DRIVER_VERSION);

	return 0;

batt_failed_2:
	kfree(bus);
batt_failed_1:
	kfree(di);
batt_failed_0:

	return retval;
}

static int sgh_batt_battery_remove(struct i2c_client *client)
{
	struct sgh_batt_device_info *di = i2c_get_clientdata(client);

	cancel_rearming_delayed_workqueue(di->wqueue,
					  &di->work);
	destroy_workqueue(di->wqueue);

	gpio_free(SGH_CHARGE_GPIO);

	power_supply_unregister(&di->bat);

	kfree(di->bat.name);

	kfree(di);

	return 0;
}


/*
 * Module stuff
 */

static const struct i2c_device_id sgh_batt_id[] = {
	{ "sgh_battery", 0 },
	{},
};

static struct i2c_driver sgh_batt_battery_driver = {
	.driver = {
		.name = "battery",
	},
	.probe = sgh_batt_battery_probe,
	.remove = sgh_batt_battery_remove,
	.suspend = NULL, 
	.resume = NULL,   //todo: power management
	.id_table = sgh_batt_id,
};

static int __init sgh_batt_battery_init(void)
{
	int ret;

	ret = i2c_add_driver(&sgh_batt_battery_driver);
	if (ret)
		printk(KERN_ERR "Unable to register Samsung I780/I900 driver\n");

	return ret;
}
module_init(sgh_batt_battery_init);

static void __exit sgh_batt_battery_exit(void)
{
	i2c_del_driver(&sgh_batt_battery_driver);
}
module_exit(sgh_batt_battery_exit);

MODULE_AUTHOR("Sacha Refshauge <xsacha@gmail.com>");
MODULE_DESCRIPTION("Samsung I780/I900 battery monitor driver");
MODULE_LICENSE("GPL");
