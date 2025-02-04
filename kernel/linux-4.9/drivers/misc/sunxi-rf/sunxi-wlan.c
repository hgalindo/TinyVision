/*
 * sunxi-wlan.c -- power on/off wlan part of SoC
 *
 * Copyright (c) 2019
 * Allwinner Technology Co., Ltd. <www.allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/rfkill.h>
#include <linux/regulator/consumer.h>
#include <linux/platform_device.h>
#include <linux/sunxi-gpio.h>
#include <linux/etherdevice.h>
#include <linux/crypto.h>
#include <linux/miscdevice.h>
#include <linux/capability.h>
#include <linux/pm_wakeirq.h>

#include "sunxi-rfkill.h"

#if defined(CONFIG_IO_EXPAND)
/*
 *for R6 scheme which use virtual extended gpio.
 *if the virtual extended gpio is not used,
 *just remove this macro
 */
#define VIRTUAL_EXTENDED_GPIO 1
#endif

static struct sunxi_wlan_platdata *wlan_data;

static int sunxi_wlan_on(struct sunxi_wlan_platdata *data, bool on_off);
static DEFINE_MUTEX(sunxi_wlan_mutex);

void sunxi_wl_chipen_set(int dev, int on_off)
{
	/* Only wifi and bt both close, chip_en goes down,
	 * otherwise, set chip_en up to keep module work.
	 * dev   : device to set power status. 0: wifi, 1: bt
	 * on_off: power status to set. 0: off, 1: on
	 */
	static int power_state;

	if (dev == 0) {  /* 0 for wifi */
		power_state &= ~(0x1);
		power_state |=  (on_off > 0);
	} else if (dev == 1) {  /* 1 for bt */
		power_state &= ~(1<<1);
		power_state |=  ((on_off > 0) << 1);
	}

	if (gpio_is_valid(wlan_data->gpio_chip_en)) {
		if (!wlan_data->gpio_chip_en_invert) {
			gpio_set_value(wlan_data->gpio_chip_en,
				       (power_state != 0));
		} else {
			gpio_set_value(wlan_data->gpio_chip_en,
				       (power_state == 0));
		}
	}
}
EXPORT_SYMBOL_GPL(sunxi_wl_chipen_set);

void sunxi_wlan_set_power(bool on_off)
{
	struct platform_device *pdev;
	int ret = 0;

	if (!wlan_data)
		return;

	pdev = wlan_data->pdev;
	mutex_lock(&sunxi_wlan_mutex);
	if (on_off != wlan_data->power_state) {
		ret = sunxi_wlan_on(wlan_data, on_off);
		if (ret)
			dev_err(&pdev->dev, "set power failed\n");
	}

	sunxi_wl_chipen_set(0, on_off);

	mutex_unlock(&sunxi_wlan_mutex);
}
EXPORT_SYMBOL_GPL(sunxi_wlan_set_power);


struct device *sunxi_wlan_get_dev(void)
{
	struct device *dev  = NULL;

	if (!wlan_data)
		return NULL;

	dev = &(wlan_data->pdev->dev);
	printk("%s->%d  device: %s\n", __func__, __LINE__, dev_name(dev));

	return dev;
}
EXPORT_SYMBOL_GPL(sunxi_wlan_get_dev);


int sunxi_wlan_get_bus_index(void)
{
	struct platform_device *pdev;

	if (!wlan_data)
		return -EINVAL;

	pdev = wlan_data->pdev;
	dev_info(&pdev->dev, "bus_index: %d\n", wlan_data->bus_index);
	return wlan_data->bus_index;
}
EXPORT_SYMBOL_GPL(sunxi_wlan_get_bus_index);

int sunxi_wlan_get_oob_irq(void)
{
	struct platform_device *pdev;
	int host_oob_irq = 0;

	if (!wlan_data || !gpio_is_valid(wlan_data->gpio_wlan_hostwake))
		return 0;

	pdev = wlan_data->pdev;

	host_oob_irq = gpio_to_irq(wlan_data->gpio_wlan_hostwake);
	if (IS_ERR_VALUE(host_oob_irq))
		dev_err(&pdev->dev, "map gpio [%d] to virq failed, errno = %d\n",
			wlan_data->gpio_wlan_hostwake, host_oob_irq);

	return host_oob_irq;
}
EXPORT_SYMBOL_GPL(sunxi_wlan_get_oob_irq);

int sunxi_wlan_get_oob_irq_flags(void)
{
	int oob_irq_flags;

	if (!wlan_data)
		return 0;

	oob_irq_flags = (IRQF_TRIGGER_HIGH | IRQF_SHARED | IRQF_NO_SUSPEND);

	return oob_irq_flags;
}
EXPORT_SYMBOL_GPL(sunxi_wlan_get_oob_irq_flags);

static int sunxi_wlan_on(struct sunxi_wlan_platdata *data, bool on_off)
{
	struct platform_device *pdev = data->pdev;
	struct device *dev = &pdev->dev;
	int ret = 0;
	int i = 0;

	if (!on_off && gpio_is_valid(data->gpio_wlan_regon))
#if defined(VIRTUAL_EXTENDED_GPIO)
		gpio_set_value_cansleep(data->gpio_wlan_regon, 0);
#else
		gpio_set_value(data->gpio_wlan_regon, 0);
#endif

	for (i = 0; i < (data->power_num); i++) {
		if (data->wlan_power_name[i]) {
			data->wlan_power[i] = regulator_get_optional(dev,
				data->wlan_power_name[i]);
			if (!IS_ERR(data->wlan_power[i])) {
				if (on_off) {
					ret =
						regulator_set_voltage(data->wlan_power[i],
							data->wlan_power_voltage,
							data->wlan_power_voltage);
					if (ret < 0) {
						dev_err(dev, "set wlan_power voltage failed!\n");
						regulator_put(data->wlan_power[i]);
						return ret;
					}

					ret = regulator_enable(data->wlan_power[i]);
					if (ret < 0) {
						dev_err(dev, "regulator wlan_power enable failed\n");
						regulator_put(data->wlan_power[i]);
						return ret;
					}

					ret = regulator_get_voltage(data->wlan_power[i]);
					if (ret < 0) {
						dev_err(dev, "regulator wlan_power get voltage failed\n");
						regulator_put(data->wlan_power[i]);
						return ret;
					}
					dev_info(dev, "check wlan wlan_power voltage: %d\n", ret);
				} else {
					ret = regulator_disable(data->wlan_power[i]);
					if (ret < 0) {
						dev_err(dev, "regulator wlan_power disable failed\n");
						regulator_put(data->wlan_power[i]);
						return ret;
					}
				}
				regulator_put(data->wlan_power[i]);
			}
		}
	}

	if (data->io_regulator_name) {
		data->io_regulator = regulator_get_optional(dev,
				data->io_regulator_name);
		if (!IS_ERR(data->io_regulator)) {
			if (on_off) {
				ret = regulator_set_voltage(data->io_regulator,
					data->wlan_io_voltage, data->wlan_io_voltage);
				if (ret < 0) {
					dev_err(dev, "set regulator io_regulator voltage failed!\n");
					regulator_put(data->io_regulator);
					return ret;
				}

				ret = regulator_enable(data->io_regulator);
				if (ret < 0) {
					dev_err(dev, "regulator io_regulator enable failed\n");
					regulator_put(data->io_regulator);
					return ret;
				}

				ret = regulator_get_voltage(data->io_regulator);
				if (ret < 0) {
					dev_err(dev, "regulator io_regulator get voltage failed\n");
					regulator_put(data->io_regulator);
					return ret;
				}
				dev_info(dev, "check wlan io_regulator voltage: %d\n", ret);
			} else {
				ret = regulator_disable(data->io_regulator);
				if (ret < 0) {
					dev_err(dev, "regulator io_regulator disable failed\n");
					regulator_put(data->io_regulator);
					return ret;
				}
			}
			regulator_put(data->io_regulator);
		}
	}

	if (on_off && gpio_is_valid(data->gpio_wlan_regon)) {
		mdelay(10);
#if defined(VIRTUAL_EXTENDED_GPIO)
		gpio_set_value_cansleep(data->gpio_wlan_regon, 1);
#else
		gpio_set_value(data->gpio_wlan_regon, 1);
#endif
	}
	wlan_data->power_state = on_off;

	return 0;
}

static ssize_t power_state_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", wlan_data->power_state);
}

static ssize_t power_state_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long state;
	int err;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	err = kstrtoul(buf, 0, &state);
	if (err)
		return err;

	if (state > 1)
		return -EINVAL;

	mutex_lock(&sunxi_wlan_mutex);
	if (state != wlan_data->power_state) {
		err = sunxi_wlan_on(wlan_data, state);
		if (err)
			dev_err(dev, "set power failed\n");
	}
	mutex_unlock(&sunxi_wlan_mutex);

	return count;
}

static DEVICE_ATTR(power_state, S_IRUGO | S_IWUSR,
		power_state_show, power_state_store);

static ssize_t scan_device_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long state;
	int err;
	int bus = wlan_data->bus_index;

	err = kstrtoul(buf, 0, &state);
	if (err)
		return err;

	sunxi_wl_chipen_set(0, state);

	dev_info(dev, "start scan device on bus_index: %d\n",
			wlan_data->bus_index);
	if (bus < 0) {
		dev_err(dev, "scan device fail!\n");
		return -1;
	}
	sunxi_mmc_rescan_card(bus);

	return count;
}

static DEVICE_ATTR(scan_device, S_IRUGO | S_IWUSR,
		NULL, scan_device_store);

static struct attribute *misc_attributes[] = {
	&dev_attr_power_state.attr,
	&dev_attr_scan_device.attr,
	NULL,
};

static struct attribute_group misc_attribute_group = {
	.name  = "rf-ctrl",
	.attrs = misc_attributes,
};

static struct miscdevice sunxi_wlan_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "sunxi-wlan",
};

static char wifi_mac_str[18] = {0};

void sunxi_wlan_chipid_mac_address(u8 *mac)
{
#define MD5_SIZE	16
#define CHIP_SIZE	16

	struct crypto_hash *tfm;
	struct hash_desc desc;
	struct scatterlist sg;
	u8 result[MD5_SIZE];
	u8 chipid[CHIP_SIZE];
	int i = 0;
	int ret = -1;

	memset(chipid, 0, sizeof(chipid));
	memset(result, 0, sizeof(result));

	sunxi_get_soc_chipid((u8 *)chipid);

	tfm = crypto_alloc_hash("md5", 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm)) {
		pr_err("Failed to alloc md5\n");
		return;
	}
	desc.tfm = tfm;
	desc.flags = 0;

	ret = crypto_hash_init(&desc);
	if (ret < 0) {
		pr_err("crypto_hash_init() failed\n");
		goto out;
	}

	sg_init_one(&sg, chipid, sizeof(chipid) - 1);
	ret = crypto_hash_update(&desc, &sg, sizeof(chipid) - 1);
	if (ret < 0) {
		pr_err("crypto_hash_update() failed for id\n");
		goto out;
	}

	crypto_hash_final(&desc, result);
	if (ret < 0) {
		pr_err("crypto_hash_final() failed for result\n");
		goto out;
	}

	/* Choose md5 result's [0][2][4][6][8][10] byte as mac address */
	for (i = 0; i < 6; i++)
		mac[i] = result[2*i];
	mac[0] &= 0xfe;     /* clear multicast bit */
	mac[0] &= 0xfd;     /* clear local assignment bit (IEEE802) */

out:
	crypto_free_hash(tfm);
}
EXPORT_SYMBOL(sunxi_wlan_chipid_mac_address);

void sunxi_wlan_custom_mac_address(u8 *mac)
{
	int i;
	char *p = wifi_mac_str;
	u8 mac_addr[ETH_ALEN] = {0};

	if (0 == strlen(p))
		return;

	for (i = 0; i < ETH_ALEN; i++, p++)
		mac_addr[i] = simple_strtoul(p, &p, 16);

	memcpy(mac, mac_addr, sizeof(mac_addr));
}
EXPORT_SYMBOL(sunxi_wlan_custom_mac_address);

#ifndef MODULE
static int __init set_wlan_mac_addr(char *str)
{
	char *p = str;

	if (str != NULL && *str)
		strlcpy(wifi_mac_str, p, 18);

	return 0;
}
__setup("wifi_mac=", set_wlan_mac_addr);
#endif

static int sunxi_wlan_probe(struct platform_device *pdev)
{
	struct pinctrl *pinctrl;
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct sunxi_wlan_platdata *data;
	struct gpio_config config;

	const char *power, *io_regulator, *clocks;

	int ret = 0;
	u32 val;
	int i = 0, j = 0;
	char wlan_name_buf[64] = {0}, s[64] = {0};

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	data->pdev = pdev;
	wlan_data = data;

	data->bus_index = -1;
	if (!of_property_read_u32(np, "wlan_busnum", &val)) {
		switch (val) {
		case 0:
		case 1:
		case 2:
			data->bus_index = val;
			break;
		default:
			dev_err(dev, "unsupported wlan_busnum (%u)\n", val);
			return -EINVAL;
		}
	}
	dev_info(dev, "wlan_busnum (%u)\n", data->bus_index);

	data->wlan_power_voltage = 3300000;
	if (!of_property_read_u32(np, "wlan_power_voltage", &val)) {
		data->wlan_power_voltage = val;
		dev_err(dev, "wlan power voltage (%u)\n", val);
	}

	data->wlan_io_voltage = 1800000;
	if (!of_property_read_u32(np, "wlan_io_voltage", &val)) {
		data->wlan_io_voltage = val;
		dev_err(dev, "wlan io voltage (%u)\n", val);
	}

	data->power_num = -1;
	if (!of_property_read_u32(np, "wlan_power_num", &val)) {
		switch (val) {
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
			data->power_num = val;
			break;
		default:
			dev_err(dev, "unsupported wlan_power_num (%u)\n", val);
			return -EINVAL;
		}
	}
	dev_info(dev, "wlan_power_num (%d)\n", data->power_num);

	if (data->power_num > 0) {
		data->wlan_power_name =
		    devm_kzalloc(dev, (data->power_num) * sizeof(char *),
				 GFP_KERNEL);
		for (i = 0; i < (data->power_num); i++) {
			sprintf(s, "wlan_power%d", i + 1);
			strcpy(wlan_name_buf, s);
			if (of_property_read_string(np, wlan_name_buf, &power)) {
				dev_warn(dev, "Missing wlan_power.\n");
			} else {
				data->wlan_power_name[i] =
				    devm_kzalloc(dev, 64, GFP_KERNEL);
				if (data->wlan_power_name[i]) {
					strcpy(data->wlan_power_name[i], power);
				} else {
					for (j = 0; j < i; j++) {
						devm_kfree(dev,
							   data->
							   wlan_power_name[j]);
					}
					devm_kfree(dev, data->wlan_power_name);
					devm_kfree(dev, data);
					return -ENOMEM;
				}
			}
			dev_info(dev, "wlan_power_name (%s)\n",
				 data->wlan_power_name[i]);
		}
	}

	if (of_property_read_string(np, "wlan_io_regulator", &io_regulator)) {
		dev_warn(dev, "Missing wlan_io_regulator.\n");
	} else {
		data->io_regulator_name = devm_kzalloc(dev, 64, GFP_KERNEL);
		if (!data->io_regulator_name) {
			ret = -ENOMEM;
			goto end;
		} else
			strcpy(data->io_regulator_name, io_regulator);
	}
	dev_info(dev, "io_regulator_name (%s)\n", data->io_regulator_name);

	/* request device pinctrl, set as default state */
	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
	if (IS_ERR_OR_NULL(pinctrl)) {
		dev_err(dev, "request pincrtl handle for device [%s] failed\n",
			dev_name(&pdev->dev));
	}
#if defined(VIRTUAL_EXTENDED_GPIO)
	dev_info(dev, "------SUNXI_RF: Set regon for SUN3IW1P1_R6!----\n");
	val = 0;
	of_property_read_u32(np, "wlan_board_sel", &val);
	if (val) {
		if (!of_property_read_u32(np, "wlan_regon", &val)) {
			data->gpio_wlan_regon = (int)val;
		} else {
			dev_err(dev, "unsupported wlan_regon(%u)\n", val);
			goto end;
		}
	} else {
		data->gpio_wlan_regon =
		    of_get_named_gpio_flags(np, "wlan_regon", 0,
					    (enum of_gpio_flags *)&config);
	}
#else
	data->gpio_wlan_regon = of_get_named_gpio_flags(np, "wlan_regon", 0,
							(enum of_gpio_flags *)
							&config);
#endif
	if (!gpio_is_valid(data->gpio_wlan_regon)) {
		dev_err(dev, "get gpio wlan_regon failed\n");
	} else {
		dev_info(dev, "wlan_regon gpio=%d  mul-sel=%d  pull=%d  drv_level=%d  data=%d\n",
				config.gpio,
				config.mul_sel,
				config.pull,
				config.drv_level,
				config.data);

		ret = devm_gpio_request(dev, data->gpio_wlan_regon,
				"wlan_regon");
		if (ret < 0) {
			dev_err(dev, "can't request wlan_regon gpio %d\n",
				data->gpio_wlan_regon);
			goto end;
		}

		ret = gpio_direction_output(data->gpio_wlan_regon, 0);
		if (ret < 0) {
			dev_err(dev, "can't request output direction wlan_regon gpio %d\n",
				data->gpio_wlan_regon);
			goto end;
		}
	}

	data->gpio_chip_en = of_get_named_gpio_flags(np, "chip_en",
			0, (enum of_gpio_flags *)&config);
	if (!gpio_is_valid(data->gpio_chip_en)) {
		dev_err(dev, "get gpio chip_en failed\n");
	} else {
		dev_info(dev, "chip_en gpio=%d  mul-sel=%d  pull=%d  drv_level=%d  data=%d\n",
				config.gpio,
				config.mul_sel,
				config.pull,
				config.drv_level,
				config.data);

		ret = devm_gpio_request(dev, data->gpio_chip_en, "chip_en");
		if (ret < 0) {
			dev_err(dev, "can't request chip_en gpio %d\n",
				data->gpio_chip_en);
			goto end;
		}

		ret = gpio_direction_output(data->gpio_chip_en, 0);
		if (ret < 0) {
			dev_err(dev, "can't request output direction chip_en gpio %d\n",
				data->gpio_chip_en);
			goto end;
		}
	}

	if (!of_property_read_u32(np, "chip_en_invert", &val)) {
		if (val > 0)
			data->gpio_chip_en_invert = 1;
		else
			data->gpio_chip_en_invert = 0;
	} else {
		data->gpio_chip_en_invert = 0;
	}

	data->gpio_wlan_hostwake = of_get_named_gpio_flags(np, "wlan_hostwake",
			0, (enum of_gpio_flags *)&config);
	if (!gpio_is_valid(data->gpio_wlan_hostwake)) {
		dev_err(dev, "get gpio wlan_hostwake failed\n");
	} else {
		dev_info(dev,
				"wlan_hostwake gpio=%d  mul-sel=%d  pull=%d  drv_level=%d  data=%d\n",
				config.gpio,
				config.mul_sel,
				config.pull,
				config.drv_level,
				config.data);

		ret = devm_gpio_request(dev, data->gpio_wlan_hostwake,
				"wlan_hostwake");
		if (ret < 0) {
			dev_err(dev, "can't request wlan_hostwake gpio %d\n",
				data->gpio_wlan_hostwake);
			goto end;
		}

		ret = gpio_direction_input(data->gpio_wlan_hostwake);
		if (ret < 0) {
			dev_err(dev,
				"can't request input direction wlan_hostwake gpio %d\n",
				data->gpio_wlan_hostwake);
			goto end;
		}

		/*
		 * wakeup_source relys on wlan_hostwake, if wlan_hostwake gpio
		 * isn't configured, then whether wakeup_source is configured
		 * or not is unmeaningful.
		 */
		/* Please use "wakeup-source" property to configure wake-up source
		 * as much as possible, and without parameters.*/
		if (!of_property_read_bool(np, "wakeup-source")) {
			data->wakeup_enable = 0;
			dev_warn(dev, "wakeup source is disabled!\n");
		} else {
			ret = device_init_wakeup(dev, true);
			if (ret < 0) {
				dev_err(dev, "device init wakeup failed!\n");
				return ret;
			}

			ret = dev_pm_set_wake_irq(dev, gpio_to_irq(data->gpio_wlan_hostwake));
			if (ret < 0) {
				dev_err(dev, "can't enable wakeup src for wlan_hostwake %d\n",
				data->gpio_wlan_hostwake);
				return ret;
			}
			data->wakeup_enable = 1;
		}
	}

	if (of_property_read_string(np, "clocks", &clocks)) {
		dev_warn(dev, "Missing clocks.\n");
	} else {
		data->clk_name = devm_kzalloc(dev, 64, GFP_KERNEL);
		if (!data->clk_name) {
			ret = -ENOMEM;
			goto end;
		} else
			strcpy(data->clk_name, clocks);
	}
	dev_info(dev, "clk_name (%s)\n", data->clk_name);

	data->lpo = devm_clk_get(dev, NULL);
	if (IS_ERR_OR_NULL(data->lpo)) {
		dev_warn(dev, "clk not config\n");
	} else {
		ret = clk_prepare_enable(data->lpo);
		if (ret < 0)
			dev_warn(dev, "can't enable clk\n");
	}

	ret = misc_register(&sunxi_wlan_dev);
	if (ret) {
		dev_err(dev, "sunxi-wlan register driver as misc device error!\n");
		goto end;
	}

	ret = sysfs_create_group(&sunxi_wlan_dev.this_device->kobj,
			&misc_attribute_group);
	if (ret) {
		dev_err(dev, "sunxi-wlan register sysfs create group failed!\n");
		goto end;
	}

	data->power_state = 0;
end:
	if (ret != 0) {
		for (i = 0; i < (wlan_data->power_num); i++)
			devm_kfree(dev, data->wlan_power_name[i]);
		devm_kfree(dev, data->wlan_power_name);
		devm_kfree(dev, data);
		return ret;
	}

	data->wlan_power =
	    devm_kzalloc(dev, (data->power_num) * sizeof(struct regulator *),
			 GFP_KERNEL);
	return 0;
}

static int sunxi_wlan_remove(struct platform_device *pdev)
{
	int i = 0;

	devm_kfree(&pdev->dev, wlan_data->wlan_power);
	for (i = 0; i < (wlan_data->power_num); i++)
		devm_kfree(&pdev->dev, wlan_data->wlan_power_name[i]);
	devm_kfree(&pdev->dev, wlan_data->wlan_power_name);
	devm_kfree(&pdev->dev, wlan_data);

	sysfs_remove_group(&(sunxi_wlan_dev.this_device->kobj),
			&misc_attribute_group);
	misc_deregister(&sunxi_wlan_dev);

	if (!IS_ERR_OR_NULL(wlan_data->lpo))
		clk_disable_unprepare(wlan_data->lpo);

	if (wlan_data->wakeup_enable) {
		dev_info(&pdev->dev, "Deinit wakeup source");
		device_init_wakeup(&pdev->dev, false);
		dev_pm_clear_wake_irq(&pdev->dev);
	}

	return 0;
}

static const struct of_device_id sunxi_wlan_ids[] = {
	{ .compatible = "allwinner,sunxi-wlan" },
	{ /* Sentinel */ }
};

static struct platform_driver sunxi_wlan_driver = {
	.probe		= sunxi_wlan_probe,
	.remove	= sunxi_wlan_remove,
	.driver	= {
		.owner	= THIS_MODULE,
		.name	= "sunxi-wlan",
		.of_match_table	= sunxi_wlan_ids,
	},
};

module_platform_driver(sunxi_wlan_driver);

MODULE_DESCRIPTION("sunxi wlan driver");
MODULE_LICENSE("GPL");
