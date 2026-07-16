/*
 * Copyright (C) 2010 Trusted Logic S.A.
 * modifications copyright (C) 2015 NXP B.V.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/regulator/consumer.h>
#include <linux/printk.h>
#include <linux/version.h>

#include <linux/of.h>
#include <linux/acpi.h>

#include "pn5xx_i2c.h"


#define MAX_BUFFER_SIZE	512

/* Only pn548, pn547 and pn544 are supported */
#define CHIP "pn544"
#define DRIVER_DESC "NFC driver for PN54x Family"

struct pn54x_dev	{
	wait_queue_head_t read_wq;
	struct mutex read_mutex;
	struct i2c_client *client;
	struct miscdevice pn54x_device;
	struct gpio_desc *ven_gpio;
	struct gpio_desc *firm_gpio;
	struct gpio_desc *irq_gpio;
	struct gpio_desc *clkreq_gpio;
	struct regulator *pvdd_reg;
	struct regulator *vbat_reg;
	struct regulator *pmuvcc_reg;
	struct regulator *sevdd_reg;
	bool irq_enabled;
	spinlock_t irq_enabled_lock;
};

/**********************************************************
 * Interrupt control and handler
 **********************************************************/
static void pn54x_disable_irq(struct pn54x_dev *pn54x_dev)
{
	unsigned long flags;

	spin_lock_irqsave(&pn54x_dev->irq_enabled_lock, flags);
	if (pn54x_dev->irq_enabled) {
		disable_irq_nosync(pn54x_dev->client->irq);
		pn54x_dev->irq_enabled = false;
	}
	spin_unlock_irqrestore(&pn54x_dev->irq_enabled_lock, flags);
}

static irqreturn_t pn54x_dev_irq_handler(int irq, void *dev_id)
{
	struct pn54x_dev *pn54x_dev = dev_id;

	pn54x_disable_irq(pn54x_dev);

	/* Wake up waiting readers */
	wake_up(&pn54x_dev->read_wq);

	return IRQ_HANDLED;
}

/**********************************************************
 * private functions
 **********************************************************/
static int pn544_enable(struct pn54x_dev *dev, int mode)
{
	int r;

	/* turn on the regulators */
	/* -- if the regulators were specified, they're required */
	if(dev->pvdd_reg != NULL)
	{
		r = regulator_enable(dev->pvdd_reg);
		if (r < 0){
			pr_err("%s: not able to enable pvdd\n", __func__);
			return r;
		}
	}
	if(dev->vbat_reg != NULL)
	{
		r = regulator_enable(dev->vbat_reg);
		if (r < 0){
			pr_err("%s: not able to enable vbat\n", __func__);
			goto err_vbat;
		}
	}
	if(dev->pmuvcc_reg != NULL)
	{
		r = regulator_enable(dev->pmuvcc_reg);
		if (r < 0){
			pr_err("%s: not able to enable pmuvcc\n", __func__);
			goto err_pmuvcc;
		}
	}
	if(dev->sevdd_reg != NULL)
	{
		r = regulator_enable(dev->sevdd_reg);
		if (r < 0){
			pr_err("%s: not able to enable sevdd\n", __func__);
			goto err_sevdd;
		}
	}

	if (PWR_ON == mode) {
		pr_info("%s power on\n", __func__);
		if (dev->firm_gpio)
			gpiod_set_value_cansleep(dev->firm_gpio, 0);
		gpiod_set_value_cansleep(dev->ven_gpio, 1);
		msleep(100);
	}
	else if (PWR_FW == mode) {
		/* power on with firmware download (requires hw reset)
		 */
		pr_info("%s power on with firmware\n", __func__);
		if (!dev->firm_gpio) {
			pr_err("%s Unused Firm GPIO %d\n", __func__, mode);
			r = -EOPNOTSUPP;
			goto err_gpio;
		}
		gpiod_set_value_cansleep(dev->ven_gpio, 1);
		msleep(20);
		gpiod_set_value_cansleep(dev->firm_gpio, 1);
		msleep(20);
		gpiod_set_value_cansleep(dev->ven_gpio, 0);
		msleep(100);
		gpiod_set_value_cansleep(dev->ven_gpio, 1);
		msleep(20);
	}
	else {
		pr_err("%s bad arg %d\n", __func__, mode);
		r = -EINVAL;
		goto err_gpio;
	}

	return 0;

err_gpio:
	if(dev->sevdd_reg) regulator_disable(dev->sevdd_reg);
err_sevdd:
	if(dev->pmuvcc_reg) regulator_disable(dev->pmuvcc_reg);
err_pmuvcc:
	if(dev->vbat_reg) regulator_disable(dev->vbat_reg);
err_vbat:
	if(dev->pvdd_reg) regulator_disable(dev->pvdd_reg);

	return r;
}

static void pn544_disable(struct pn54x_dev *dev)
{
	/* power off */
	pr_info("%s power off\n", __func__);
	if (dev->firm_gpio)
		gpiod_set_value_cansleep(dev->firm_gpio, 0);
	gpiod_set_value_cansleep(dev->ven_gpio, 0);
	msleep(100);

	if(dev->sevdd_reg) regulator_disable(dev->sevdd_reg);
	if(dev->pmuvcc_reg) regulator_disable(dev->pmuvcc_reg);
	if(dev->vbat_reg) regulator_disable(dev->vbat_reg);
	if(dev->pvdd_reg) regulator_disable(dev->pvdd_reg);

}

/**********************************************************
 * driver functions
 **********************************************************/
static ssize_t pn54x_dev_read(struct file *filp, char __user *buf,
		size_t count, loff_t *offset)
{
	struct pn54x_dev *pn54x_dev = filp->private_data;
	char tmp[MAX_BUFFER_SIZE];
	int ret;

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	pr_debug("%s : reading %zu bytes.\n", __func__, count);

	mutex_lock(&pn54x_dev->read_mutex);

	if (!gpiod_get_value_cansleep(pn54x_dev->irq_gpio)) {
		if (filp->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto fail;
		}

		while (1) {
			pn54x_dev->irq_enabled = true;
			enable_irq(pn54x_dev->client->irq);
			ret = wait_event_interruptible(
					pn54x_dev->read_wq,
					!pn54x_dev->irq_enabled);

			pn54x_disable_irq(pn54x_dev);

			if (ret)
				goto fail;

			if (gpiod_get_value_cansleep(pn54x_dev->irq_gpio))
				break;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,5,0)
			pr_warning("%s: spurious interrupt detected\n", __func__);
#else
			pr_warn("%s: spurious interrupt detected\n", __func__);
#endif
		}
	}

	/* Read data */
	ret = i2c_master_recv(pn54x_dev->client, tmp, count);

	mutex_unlock(&pn54x_dev->read_mutex);

	/* pn54x seems to be slow in handling I2C read requests
	 * so add 1ms delay after recv operation */
	udelay(1000);

	if (ret < 0) {
		pr_err("%s: i2c_master_recv returned %d\n", __func__, ret);
		return ret;
	}
	if (ret > count) {
		pr_err("%s: received too many bytes from i2c (%d)\n",
			__func__, ret);
		return -EIO;
	}
	if (copy_to_user(buf, tmp, ret)) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,5,0)
		pr_warning("%s : failed to copy to user space\n", __func__);
#else
		pr_warn("%s : failed to copy to user space\n", __func__);
#endif
		return -EFAULT;
	}
	return ret;

fail:
	mutex_unlock(&pn54x_dev->read_mutex);
	return ret;
}

static ssize_t pn54x_dev_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *offset)
{
	struct pn54x_dev  *pn54x_dev;
	char tmp[MAX_BUFFER_SIZE];
	int ret;

	pn54x_dev = filp->private_data;

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	if (copy_from_user(tmp, buf, count)) {
		pr_err("%s : failed to copy from user space\n", __func__);
		return -EFAULT;
	}

	pr_debug("%s : writing %zu bytes.\n", __func__, count);
	/* Write data */
	ret = i2c_master_send(pn54x_dev->client, tmp, count);
	if (ret != count) {
		pr_err("%s : i2c_master_send returned %d\n", __func__, ret);
		ret = -EIO;
	}

	/* pn54x seems to be slow in handling I2C write requests
	 * so add 1ms delay after I2C send operation */
	udelay(1000);

	return ret;
}

static int pn54x_dev_open(struct inode *inode, struct file *filp)
{
	struct pn54x_dev *pn54x_dev = container_of(filp->private_data,
											   struct pn54x_dev,
											   pn54x_device);

	filp->private_data = pn54x_dev;

	pr_info("%s : %d,%d\n", __func__, imajor(inode), iminor(inode));

	return 0;
}

static int pn54x_dev_release(struct inode *inode, struct file *filp)
{
	pr_info("%s : closing %d,%d\n", __func__, imajor(inode), iminor(inode));
	return 0;
}

static long  pn54x_dev_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	struct pn54x_dev *pn54x_dev = filp->private_data;
	int ret;

	pr_info("%s, cmd=%d, arg=%lu\n", __func__, cmd, arg);
	switch (cmd) {
	case PN544_SET_PWR:
		if (arg == PWR_ON || arg == PWR_FW) {
			ret = pn544_enable(pn54x_dev, arg);
			if (ret)
				return ret;
		} else if (arg == PWR_OFF) {
			pn544_disable(pn54x_dev);
		} else {
			pr_err("%s bad SET_PWR arg %lu\n", __func__, arg);
			return -EINVAL;
		}
		break;
	case PN54X_CLK_REQ:
		if(CLK_ON == arg){
			if(pn54x_dev->clkreq_gpio){
				gpiod_set_value_cansleep(pn54x_dev->clkreq_gpio, 1);
			}
			else {
				pr_err("%s Unused Clkreq GPIO %lu\n", __func__, arg);
				return -EOPNOTSUPP;
			}
		}
		else if(CLK_OFF == arg) {
			if(pn54x_dev->clkreq_gpio){
				gpiod_set_value_cansleep(pn54x_dev->clkreq_gpio, 0);
			}
			else {
				pr_err("%s Unused Clkreq GPIO %lu\n", __func__, arg);
				return -EOPNOTSUPP;
			}
		} else {
			pr_err("%s bad CLK_REQ arg %lu\n", __func__, arg);
			return -EINVAL;
		}
		break;
	default:
		pr_err("%s bad ioctl %u\n", __func__, cmd);
		return -EINVAL;
	}

	return 0;
}

static const struct file_operations pn54x_dev_fops = {
	.owner		= THIS_MODULE,
	.read		= pn54x_dev_read,
	.write		= pn54x_dev_write,
	.open		= pn54x_dev_open,
	.release	= pn54x_dev_release,
	.unlocked_ioctl	= pn54x_dev_ioctl,
	.compat_ioctl	= pn54x_dev_ioctl,
};


/*
 * Handlers for alternative sources of platform_data
 */

/*
 * Translate OpenFirmware node properties into platform_data
 */
#ifdef CONFIG_OF
static int pn54x_get_pdata_of(struct device *dev,
							struct pn544_i2c_platform_data *pdata)
{
	struct gpio_desc *desc;

	if (!dev->of_node)
		return -ENODEV;

	memset(pdata, 0, sizeof(*pdata));

	/* ven pin - enable's power to the chip - REQUIRED */
	desc = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(desc)) {
		dev_err(dev, "VEN GPIO error getting from OF node\n");
		return PTR_ERR(desc);
	}
	pdata->ven_gpio = desc;

	/* firm pin - controls firmware download - OPTIONAL */
	desc = devm_gpiod_get_optional(dev, "firmware", GPIOD_OUT_LOW);
	if (IS_ERR(desc)) {
		dev_err(dev, "FIRM GPIO error getting from OF node\n");
		return PTR_ERR(desc);
	}
	pdata->firm_gpio = desc;

	/* irq pin - data available irq - REQUIRED */
	desc = devm_gpiod_get(dev, "interrupt", GPIOD_IN);
	if (IS_ERR(desc)) {
		dev_err(dev, "IRQ GPIO error getting from OF node\n");
		return PTR_ERR(desc);
	}
	pdata->irq_gpio = desc;

	/* clkreq pin - controls the clock to the PN547 - OPTIONAL */
	desc = devm_gpiod_get_optional(dev, "clkreq", GPIOD_OUT_LOW);
	if (IS_ERR(desc)) {
		dev_err(dev, "CLKREQ GPIO error getting from OF node\n");
		return PTR_ERR(desc);
	}
	pdata->clkreq_gpio = desc;

	/* handle the regulator lines - these are optional
	 * PVdd - pad Vdd (544, 547)
	 * Vbat - Battery (544, 547)
	 * PMUVcc - UICC Power (544, 547)
	 * SEVdd - SE Power (544)
	 *
	 * Will attempt to load a matching Regulator Resource for each
	 * If no resource is provided, then the input will not be controlled
	 * Example: if only PVdd is provided, it is the only one that will be
	 *  turned on/off.
	 */
	pdata->pvdd_reg = devm_regulator_get_optional(dev, "nxp,pn54x-pvdd");
	if (IS_ERR(pdata->pvdd_reg))
		pdata->pvdd_reg = NULL;

	pdata->vbat_reg = devm_regulator_get_optional(dev, "nxp,pn54x-vbat");
	if (IS_ERR(pdata->vbat_reg))
		pdata->vbat_reg = NULL;

	pdata->pmuvcc_reg = devm_regulator_get_optional(dev, "nxp,pn54x-pmuvcc");
	if (IS_ERR(pdata->pmuvcc_reg))
		pdata->pmuvcc_reg = NULL;

	pdata->sevdd_reg = devm_regulator_get_optional(dev, "nxp,pn54x-sevdd");
	if (IS_ERR(pdata->sevdd_reg))
		pdata->sevdd_reg = NULL;

	return 0;
}
#endif

#ifdef CONFIG_ACPI
static int pn54x_get_pdata_acpi(struct device *dev,
							struct pn544_i2c_platform_data *pdata)
{
	struct gpio_desc *desc;

	/* irq pin - data available irq - REQUIRED (ACPI index 0: GpioInt) */
	desc = devm_gpiod_get_index(dev, NULL, 0, GPIOD_IN);
	if (IS_ERR(desc)) {
		dev_err(dev, "IRQ GPIO error getting from ACPI\n");
		return PTR_ERR(desc);
	}
	pdata->irq_gpio = desc;

	/* firm pin - controls firmware download - REQUIRED (ACPI index 1: GpioIo) */
	desc = devm_gpiod_get_index(dev, NULL, 1, GPIOD_OUT_LOW);
	if (IS_ERR(desc)) {
		dev_err(dev, "FIRM GPIO error getting from ACPI\n");
		return PTR_ERR(desc);
	}
	pdata->firm_gpio = desc;

	/* ven pin - enable's power to the chip - REQUIRED (ACPI index 2: GpioIo) */
	desc = devm_gpiod_get_index(dev, NULL, 2, GPIOD_OUT_LOW);
	if (IS_ERR(desc)) {
		dev_err(dev, "VEN GPIO error getting from ACPI\n");
		return PTR_ERR(desc);
	}
	pdata->ven_gpio = desc;

	/* clkreq pin - controls the clock to the PN547 - OPTIONAL */
	pdata->clkreq_gpio = NULL;

	pdata->pvdd_reg = NULL;
	pdata->vbat_reg = NULL;
	pdata->pmuvcc_reg = NULL;
	pdata->sevdd_reg = NULL;

	return 0;
}
#endif

/*
 * pn54x_probe
 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,0)
static int __devinit pn54x_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6,3,0)
static int pn54x_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
#else
static int pn54x_probe(struct i2c_client *client)
#endif
{
	int ret = 0;
	struct pn544_i2c_platform_data *pdata;
	struct pn544_i2c_platform_data tmp_pdata;
	struct pn54x_dev *pn54x_dev;
	bool is_acpi = false;

	pr_info("%s\n", __func__);

	pdata = client->dev.platform_data;

	if(!pdata)
	{
#ifdef CONFIG_ACPI
		if (ACPI_HANDLE(&client->dev)) {
			is_acpi = true;
			pr_info("%s: configuring via ACPI\n", __func__);
			ret = pn54x_get_pdata_acpi(&(client->dev), &tmp_pdata);
		}
#endif
#ifdef CONFIG_OF
		/* if device was not configured via ACPI, or ACPI configuration failed */
		if ((!is_acpi || ret) && client->dev.of_node) {
			pr_info("%s: configuring via device tree\n", __func__);
			ret = pn54x_get_pdata_of(&(client->dev), &tmp_pdata);
		}
#endif

		if(ret) {
			return ret;
		}

		pdata = &tmp_pdata;
	}

	if (pdata == NULL) {
		pr_err("%s: nfc probe fail\n", __func__);
		return  -ENODEV;
	}

	/* validate the the adapter has basic I2C functionality */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s: need I2C_FUNC_I2C\n", __func__);
		return  -ENODEV;
	}

	/* map the IRQ GPIO to an IRQ number */
	ret = gpiod_to_irq(pdata->irq_gpio);
	if (ret < 0) {
		pr_err("%s: not able to map irq_gpio to an IRQ\n", __func__);
		return ret;
	}
	client->irq = ret;

	/* allocate the pn54x driver information structure */
	pn54x_dev = kzalloc(sizeof(*pn54x_dev), GFP_KERNEL);
	if (pn54x_dev == NULL) {
		dev_err(&client->dev, "failed to allocate memory for module data\n");
		return -ENOMEM;
	}

	/* store the platform data in the driver info struct */
	pn54x_dev->irq_gpio = pdata->irq_gpio;
	pn54x_dev->ven_gpio = pdata->ven_gpio;
	pn54x_dev->firm_gpio = pdata->firm_gpio;
	pn54x_dev->clkreq_gpio = pdata->clkreq_gpio;
	pn54x_dev->pvdd_reg = pdata->pvdd_reg;
	pn54x_dev->vbat_reg = pdata->vbat_reg;
	pn54x_dev->pmuvcc_reg = pdata->pmuvcc_reg;
	pn54x_dev->sevdd_reg = pdata->sevdd_reg;

	pn54x_dev->client = client;

	/* init mutex and queues */
	init_waitqueue_head(&pn54x_dev->read_wq);
	mutex_init(&pn54x_dev->read_mutex);
	spin_lock_init(&pn54x_dev->irq_enabled_lock);

	/* register as a misc device - character based with one entry point */
	pn54x_dev->pn54x_device.minor = MISC_DYNAMIC_MINOR;
	pn54x_dev->pn54x_device.name = CHIP;
	pn54x_dev->pn54x_device.fops = &pn54x_dev_fops;
	ret = misc_register(&pn54x_dev->pn54x_device);
	if (ret) {
		pr_err("%s : misc_register failed\n", __FILE__);
		goto err_misc_register;
	}

	/* request irq.  the irq is set whenever the chip has data available
	 * for reading.  it is cleared when all data has been read.
	 */
	pr_info("%s : requesting IRQ %d\n", __func__, client->irq);
	pn54x_dev->irq_enabled = true;
	ret = request_irq(client->irq, pn54x_dev_irq_handler,
				IRQF_TRIGGER_HIGH, client->name, pn54x_dev);
	if (ret) {
		dev_err(&client->dev, "request_irq failed\n");
		goto err_request_irq_failed;
	}
	pn54x_disable_irq(pn54x_dev);

	i2c_set_clientdata(client, pn54x_dev);

	return 0;

err_request_irq_failed:
	misc_deregister(&pn54x_dev->pn54x_device);
err_misc_register:
	kfree(pn54x_dev);

	return ret;
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,0)
static int __devexit pn54x_remove(struct i2c_client *client)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6,1,0)
static int pn54x_remove(struct i2c_client *client)
#else
static void pn54x_remove(struct i2c_client *client)
#endif
{
	struct pn54x_dev *pn54x_dev;

	pr_info("%s\n", __func__);

	pn54x_dev = i2c_get_clientdata(client);
	free_irq(client->irq, pn54x_dev);
	misc_deregister(&pn54x_dev->pn54x_device);
	mutex_destroy(&pn54x_dev->read_mutex);
	kfree(pn54x_dev);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,1,0)
	return 0;
#endif
}

#ifdef CONFIG_OF
static const struct of_device_id pn54x_dt_match[] = {
	{ .compatible = "nxp,pn547", },
	{ .compatible = "nxp,pn544", },
	{},
};
MODULE_DEVICE_TABLE(of, pn54x_dt_match);
#endif

static const struct i2c_device_id pn54x_id[] = {
	{ "pn547", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, pn54x_id);

#ifdef CONFIG_ACPI
static const struct acpi_device_id pn54x_acpi_match[] = {
        { "NXP1001", 0},
        { },
};
MODULE_DEVICE_TABLE(acpi, pn54x_acpi_match);
#endif


static struct i2c_driver pn54x_driver = {
	.id_table	= pn54x_id,
	.probe		= pn54x_probe,
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,0)
	.remove		= __devexit_p(pn54x_remove),
#else
	.remove		= pn54x_remove,
#endif
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "pn544",
#ifdef CONFIG_OF
		.of_match_table = pn54x_dt_match,
#endif
#ifdef CONFIG_ACPI
		.acpi_match_table = ACPI_PTR(pn54x_acpi_match),
#endif
	},
};

/*
 * module load/unload record keeping
 */

static int __init pn54x_dev_init(void)
{
	pr_info("%s\n", __func__);
	return i2c_add_driver(&pn54x_driver);
}

static void __exit pn54x_dev_exit(void)
{
	pr_info("%s\n", __func__);
	i2c_del_driver(&pn54x_driver);
}

module_init(pn54x_dev_init);
module_exit(pn54x_dev_exit);

MODULE_AUTHOR("Sylvain Fonteneau");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
