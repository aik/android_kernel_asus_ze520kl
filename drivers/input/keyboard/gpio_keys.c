/*
 * Driver for keys on GPIO lines capable of generating interrupts.
 *
 * Copyright 2005 Phil Blundell
 * Copyright 2010, 2011 David Jander <david@protonic.nl>
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/gpio_keys.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/spinlock.h>
#include <linux/pinctrl/consumer.h>
#include <linux/syscore_ops.h>
//ASUS BSP Austin_T+++ : Fix DoubleClickVolumeKey sometimes can't bring up panel
#include <linux/wakelock.h>
static bool g_bResume = 1;
static struct wake_lock pwr_key_wake_lock;
//ASUS BSP Austin_T--- : Fix DoubleClickVolumeKey sometimes can't bring up panel

static struct work_struct __keysavelog_work;

//freddy +++ for key porting
static int g_keycheck_abort = 0, g_keystate = 0; //set tag for abort method
//freddy --- for key porting

//freddy +++ for fake key
enum {
	DEBUG_REPORT_EVENT = 1U << 0,
	DEBUG_PAD_EVENT = 1U << 1,
};

static int debug_mask = DEBUG_REPORT_EVENT;
module_param_named(debug_mask, debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP);

#define GPIO_KEYS_PRINTK(mask, message, ...) \
	do { \
		if ((mask) & debug_mask) \
			printk(message, ## __VA_ARGS__); \
	} while (0)
//freddy --- for fake key
struct gpio_button_data {
	struct gpio_keys_button *button; //ASUS BSP Vincent remove const
	struct input_dev *input;
	struct timer_list timer;
	struct work_struct work;
	unsigned int timer_debounce;	/* in msecs */
	unsigned int irq;
	spinlock_t lock;
	bool disabled;
	bool key_pressed;
};

struct gpio_keys_drvdata {
	const struct gpio_keys_platform_data *pdata;
	struct pinctrl *key_pinctrl;
	struct input_dev *input;
	struct mutex disable_lock;
	int force_trigger;  //ASUS_BSP : Eric
	struct gpio_button_data data[0];
};

static struct device *global_dev;
static struct syscore_ops gpio_keys_syscore_pm_ops;

static void gpio_keys_syscore_resume(void);

void keysavelog_func(struct work_struct *work)
{
	int ret = -1;
	char cmdpath[] = "/system/bin/recvkernelevt";
	char *argv[] = {cmdpath, "savelogmtp",NULL};
	char *envp[] = {"HOME=/", "PATH=/sbin:/system/bin:/system/sbin:/vendor/bin", NULL};

	printk("[Debug+++] trigger savelogmtp on userspace\n");
	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_EXEC);
	printk("[Debug---] trigger savelogmtp on userspace, ret = %d\n", ret);

	return;
}

/*
 * SYSFS interface for enabling/disabling keys and switches:
 *
 * There are 4 attributes under /sys/devices/platform/gpio-keys/
 *	keys [ro]              - bitmap of keys (EV_KEY) which can be
 *	                         disabled
 *	switches [ro]          - bitmap of switches (EV_SW) which can be
 *	                         disabled
 *	disabled_keys [rw]     - bitmap of keys currently disabled
 *	disabled_switches [rw] - bitmap of switches currently disabled
 *
 * Userland can change these values and hence disable event generation
 * for each key (or switch). Disabling a key means its interrupt line
 * is disabled.
 *
 * For example, if we have following switches set up as gpio-keys:
 *	SW_DOCK = 5
 *	SW_CAMERA_LENS_COVER = 9
 *	SW_KEYPAD_SLIDE = 10
 *	SW_FRONT_PROXIMITY = 11
 * This is read from switches:
 *	11-9,5
 * Next we want to disable proximity (11) and dock (5), we write:
 *	11,5
 * to file disabled_switches. Now proximity and dock IRQs are disabled.
 * This can be verified by reading the file disabled_switches:
 *	11,5
 * If we now want to enable proximity (11) switch we write:
 *	5
 * to disabled_switches.
 *
 * We can disable only those keys which don't allow sharing the irq.
 */

/**
 * get_n_events_by_type() - returns maximum number of events per @type
 * @type: type of button (%EV_KEY, %EV_SW)
 *
 * Return value of this function can be used to allocate bitmap
 * large enough to hold all bits for given type.
 */
static inline int get_n_events_by_type(int type)
{
	BUG_ON(type != EV_SW && type != EV_KEY);

	return (type == EV_KEY) ? KEY_CNT : SW_CNT;
}

/**
 * gpio_keys_disable_button() - disables given GPIO button
 * @bdata: button data for button to be disabled
 *
 * Disables button pointed by @bdata. This is done by masking
 * IRQ line. After this function is called, button won't generate
 * input events anymore. Note that one can only disable buttons
 * that don't share IRQs.
 *
 * Make sure that @bdata->disable_lock is locked when entering
 * this function to avoid races when concurrent threads are
 * disabling buttons at the same time.
 */
static void gpio_keys_disable_button(struct gpio_button_data *bdata)
{
	if (!bdata->disabled) {
		/*
		 * Disable IRQ and possible debouncing timer.
		 */
		disable_irq(bdata->irq);
		if (bdata->timer_debounce)
			del_timer_sync(&bdata->timer);

		bdata->disabled = true;
	}
}

/**
 * gpio_keys_enable_button() - enables given GPIO button
 * @bdata: button data for button to be disabled
 *
 * Enables given button pointed by @bdata.
 *
 * Make sure that @bdata->disable_lock is locked when entering
 * this function to avoid races with concurrent threads trying
 * to enable the same button at the same time.
 */
static void gpio_keys_enable_button(struct gpio_button_data *bdata)
{
	if (bdata->disabled) {
		enable_irq(bdata->irq);
		bdata->disabled = false;
	}
}

/**
 * gpio_keys_attr_show_helper() - fill in stringified bitmap of buttons
 * @ddata: pointer to drvdata
 * @buf: buffer where stringified bitmap is written
 * @type: button type (%EV_KEY, %EV_SW)
 * @only_disabled: does caller want only those buttons that are
 *                 currently disabled or all buttons that can be
 *                 disabled
 *
 * This function writes buttons that can be disabled to @buf. If
 * @only_disabled is true, then @buf contains only those buttons
 * that are currently disabled. Returns 0 on success or negative
 * errno on failure.
 */
static ssize_t gpio_keys_attr_show_helper(struct gpio_keys_drvdata *ddata,
					  char *buf, unsigned int type,
					  bool only_disabled)
{
	int n_events = get_n_events_by_type(type);
	unsigned long *bits;
	ssize_t ret;
	int i;

	bits = kcalloc(BITS_TO_LONGS(n_events), sizeof(*bits), GFP_KERNEL);
	if (!bits)
		return -ENOMEM;

	for (i = 0; i < ddata->pdata->nbuttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];

		if (bdata->button->type != type)
			continue;

		if (only_disabled && !bdata->disabled)
			continue;

		__set_bit(bdata->button->code, bits);
	}

	ret = bitmap_scnlistprintf(buf, PAGE_SIZE - 2, bits, n_events);
	buf[ret++] = '\n';
	buf[ret] = '\0';

	kfree(bits);

	return ret;
}

/**
 * gpio_keys_attr_store_helper() - enable/disable buttons based on given bitmap
 * @ddata: pointer to drvdata
 * @buf: buffer from userspace that contains stringified bitmap
 * @type: button type (%EV_KEY, %EV_SW)
 *
 * This function parses stringified bitmap from @buf and disables/enables
 * GPIO buttons accordingly. Returns 0 on success and negative error
 * on failure.
 */
static ssize_t gpio_keys_attr_store_helper(struct gpio_keys_drvdata *ddata,
					   const char *buf, unsigned int type)
{
	int n_events = get_n_events_by_type(type);
	unsigned long *bits;
	ssize_t error;
	int i;

	bits = kcalloc(BITS_TO_LONGS(n_events), sizeof(*bits), GFP_KERNEL);
	if (!bits)
		return -ENOMEM;

	error = bitmap_parselist(buf, bits, n_events);
	if (error)
		goto out;

	/* First validate */
	for (i = 0; i < ddata->pdata->nbuttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];

		if (bdata->button->type != type)
			continue;

		if (test_bit(bdata->button->code, bits) &&
		    !bdata->button->can_disable) {
			error = -EINVAL;
			goto out;
		}
	}

	mutex_lock(&ddata->disable_lock);

	for (i = 0; i < ddata->pdata->nbuttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];

		if (bdata->button->type != type)
			continue;

		if (test_bit(bdata->button->code, bits))
			gpio_keys_disable_button(bdata);
		else
			gpio_keys_enable_button(bdata);
	}

	mutex_unlock(&ddata->disable_lock);

out:
	kfree(bits);
	return error;
}

#define ATTR_SHOW_FN(name, type, only_disabled)				\
static ssize_t gpio_keys_show_##name(struct device *dev,		\
				     struct device_attribute *attr,	\
				     char *buf)				\
{									\
	struct platform_device *pdev = to_platform_device(dev);		\
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);	\
									\
	return gpio_keys_attr_show_helper(ddata, buf,			\
					  type, only_disabled);		\
}

ATTR_SHOW_FN(keys, EV_KEY, false);
ATTR_SHOW_FN(switches, EV_SW, false);
ATTR_SHOW_FN(disabled_keys, EV_KEY, true);
ATTR_SHOW_FN(disabled_switches, EV_SW, true);

/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/gpio-keys/keys [ro]
 * /sys/devices/platform/gpio-keys/switches [ro]
 */
static DEVICE_ATTR(keys, S_IRUGO, gpio_keys_show_keys, NULL);
static DEVICE_ATTR(switches, S_IRUGO, gpio_keys_show_switches, NULL);

#define ATTR_STORE_FN(name, type)					\
static ssize_t gpio_keys_store_##name(struct device *dev,		\
				      struct device_attribute *attr,	\
				      const char *buf,			\
				      size_t count)			\
{									\
	struct platform_device *pdev = to_platform_device(dev);		\
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);	\
	ssize_t error;							\
									\
	error = gpio_keys_attr_store_helper(ddata, buf, type);		\
	if (error)							\
		return error;						\
									\
	return count;							\
}

ATTR_STORE_FN(disabled_keys, EV_KEY);
ATTR_STORE_FN(disabled_switches, EV_SW);

/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/gpio-keys/disabled_keys [rw]
 * /sys/devices/platform/gpio-keys/disables_switches [rw]
 */
static DEVICE_ATTR(disabled_keys, S_IWUSR | S_IRUGO,
		   gpio_keys_show_disabled_keys,
		   gpio_keys_store_disabled_keys);
static DEVICE_ATTR(disabled_switches, S_IWUSR | S_IRUGO,
		   gpio_keys_show_disabled_switches,
		   gpio_keys_store_disabled_switches);

//ASUS BSP : Austin_T +++
static ssize_t gpio_keys_wakeup_enable(struct device *dev,
				struct device_attribute *attr, const char *buf,
						size_t size, int enable_wakeup)
{
		int i, ret = -EINVAL;
		long code;
		struct gpio_keys_drvdata *ddata = dev_get_drvdata(dev);

		ret = kstrtol(buf, 10, &code);
		if (ret != 0) {

			dev_err(dev, "Invalid input.\n");
			return ret;
		}
		for (i = 0; i < ddata->pdata->nbuttons; i++) {
			struct gpio_button_data *bdata = &ddata->data[i];
			if ((int)code == bdata->button->code) {
				bdata->button->wakeup = enable_wakeup;
				device_init_wakeup(dev, bdata->button->wakeup);
				break;
				}
		}
		return size;
}

static ssize_t gpio_keys_store_enabled_wakeup(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t size)
{
				printk("[Gpio_keys] set volkey wakeup:True\n");
		return gpio_keys_wakeup_enable(dev, attr, buf, size, 1);
}

static ssize_t gpio_keys_store_disabled_wakeup(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t size)
{
				printk("[Gpio_keys] set volkey wakeup:False\n");
		return gpio_keys_wakeup_enable(dev, attr, buf, size, 0);
}
static DEVICE_ATTR(enabled_wakeup, S_IWUSR | S_IRUGO,
						NULL,
						gpio_keys_store_enabled_wakeup);
static DEVICE_ATTR(disabled_wakeup, S_IWUSR | S_IRUGO,
						NULL,
						gpio_keys_store_disabled_wakeup);
//ASUS BSP : Austin_T ---

static struct attribute *gpio_keys_attrs[] = {
	&dev_attr_keys.attr,
	&dev_attr_switches.attr,
	&dev_attr_disabled_keys.attr,
	&dev_attr_disabled_switches.attr,
	&dev_attr_enabled_wakeup.attr,		//ASUS BSP : Austin_T +++
    &dev_attr_disabled_wakeup.attr,		//ASUS BSP : Austin_T +++
	NULL,
};

static struct attribute_group gpio_keys_attr_group = {
	.attrs = gpio_keys_attrs,
};


unsigned int b_press = 0; //ASUS_BSP : Eric
static bool kvup = false;
static int kdown_count = 0;

static void gpio_keys_gpio_report_event(struct gpio_button_data *bdata)
{
	const struct gpio_keys_button *button = bdata->button;
	struct input_dev *input = bdata->input;
	unsigned int type = button->type ?: EV_KEY;
	int state;

	state = (__gpio_get_value(button->gpio) ? 1 : 0) ^ button->active_low;
	printk("[Keys][gpio_keys.c] keycode=%d, state=%s\n",
			button->code, state?"press":"release");  //ASUS BSP Vincent +++

	if (type == EV_ABS) {
		if (state)
			input_event(input, type, button->code, button->value);
	} else {
		input_event(input, type, button->code, !!state);

		if (button->code == KEY_VOLUMEUP) {
			if(state) {
				kvup = true;
			} else {
				kvup = false;
				kdown_count = 0;
			}
		}

		if (button->code == KEY_VOLUMEDOWN) {
			if(state && kvup) {
				kdown_count++;
				printk("[Debug] Prepare to trigger savelogmtp on userspace: volumn down count: %d\n", kdown_count);

				if(kdown_count == 10) {
					printk("[Debug] kernel trigger savelogmtp on userspace: \n");
					schedule_work(&__keysavelog_work);
				}
			}
		}

		if(state) {
			//ASUS BSP Austin_T+++ : Fix DoubleClickVolumeKey sometimes can't bring up panel
			if ((button->code == KEY_VOLUMEUP) || (button->code == KEY_VOLUMEDOWN)) {
				printk("[Gpio_keys]vol_key:%x,pm sts:%x,\r\n", state, g_bResume);
				wake_lock_timeout(&pwr_key_wake_lock, 3 * HZ);
				printk("[Gpio_keys]Wakelock 3 sec for vol_key \n");
			}
			//ASUS BSP Austin_T--- : Fix DoubleClickVolumeKey sometimes can't bring up panel

			if(button->code == 114)
				b_press |= 0x01;

			if(button->code == 115)
				b_press |= 0x02;
		}
		else {
			if(button->code == 114)
				b_press &= ~(0x01);

			if(button->code == 115)
				b_press &= ~(0x02);
		}
	}
	input_sync(input);
}

static void gpio_keys_gpio_work_func(struct work_struct *work)
{
	struct gpio_button_data *bdata =
		container_of(work, struct gpio_button_data, work);

	gpio_keys_gpio_report_event(bdata);

	if (bdata->button->wakeup)
		pm_relax(bdata->input->dev.parent);
}

static void gpio_keys_gpio_timer(unsigned long _data)
{
	struct gpio_button_data *bdata = (struct gpio_button_data *)_data;
//ASUS BSP freddy+++ fix TT 281235:Power key suspend/resume fail
	const struct gpio_keys_button *button = bdata->button;
	g_keystate = (gpio_get_value(button->gpio) ? 1 : 0) ^ button->active_low;//memo button->active_low==1;
	printk("[Gpio_keys] after debounce,gpio=%d key state=%d \n",button->gpio, g_keystate);	
//ASUS BSP freddy--- fix TT 281235:Power key suspend/resume fail
	schedule_work(&bdata->work);
}

static irqreturn_t gpio_keys_gpio_isr(int irq, void *dev_id)
{
	struct gpio_button_data *bdata = dev_id;
//ASUS BSP freddy+++ fix TT 281235:Power key suspend/resume fail
	const struct gpio_keys_button *button = bdata->button;
	int state = (gpio_get_value(button->gpio) ? 1 : 0) ^ button->active_low;//memo button->active_low==1;
	BUG_ON(irq != bdata->irq);
	g_keycheck_abort = 1;
//ASUS BSP freddy--- fix TT 281235:Power key suspend/resume fail
	if (bdata->button->wakeup)
		pm_stay_awake(bdata->input->dev.parent);
	if (bdata->timer_debounce)
		mod_timer(&bdata->timer,
			jiffies + msecs_to_jiffies(bdata->timer_debounce));
	else
		schedule_work(&bdata->work);
	printk("[Gpio_keys]debouncing,irq_number=%d key state=%d \n",irq, state);
	return IRQ_HANDLED;
}

static void gpio_keys_irq_timer(unsigned long _data)
{
	struct gpio_button_data *bdata = (struct gpio_button_data *)_data;
	struct input_dev *input = bdata->input;
	unsigned long flags;

	spin_lock_irqsave(&bdata->lock, flags);
	if (bdata->key_pressed) {
		input_event(input, EV_KEY, bdata->button->code, 0);
		input_sync(input);
		bdata->key_pressed = false;
	}
	spin_unlock_irqrestore(&bdata->lock, flags);
}

static irqreturn_t gpio_keys_irq_isr(int irq, void *dev_id)
{
	struct gpio_button_data *bdata = dev_id;
	const struct gpio_keys_button *button = bdata->button;
	struct input_dev *input = bdata->input;
	unsigned long flags;

	BUG_ON(irq != bdata->irq);

	spin_lock_irqsave(&bdata->lock, flags);

	if (!bdata->key_pressed) {
		if (bdata->button->wakeup)
			pm_wakeup_event(bdata->input->dev.parent, 0);

		input_event(input, EV_KEY, button->code, 1);
		input_sync(input);

		if (!bdata->timer_debounce) {
			input_event(input, EV_KEY, button->code, 0);
			input_sync(input);
			goto out;
		}

		bdata->key_pressed = true;
	}

	if (bdata->timer_debounce)
		mod_timer(&bdata->timer,
			jiffies + msecs_to_jiffies(bdata->timer_debounce));
out:
	spin_unlock_irqrestore(&bdata->lock, flags);
	return IRQ_HANDLED;
}

static void gpio_keys_quiesce_key(void *data)
{
	struct gpio_button_data *bdata = data;

	if (bdata->timer_debounce)
		del_timer_sync(&bdata->timer);

	cancel_work_sync(&bdata->work);
}

static int gpio_keys_setup_key(struct platform_device *pdev,
				struct input_dev *input,
				struct gpio_button_data *bdata,
				struct gpio_keys_button *button) //ASUS BSP Vincent remove const
{
	const char *desc = button->desc ? button->desc : "gpio_keys";
	struct device *dev = &pdev->dev;
	irq_handler_t isr;
	unsigned long irqflags;
	int irq;
	int error;

	bdata->input = input;
	bdata->button = button;
	spin_lock_init(&bdata->lock);

	if (gpio_is_valid(button->gpio)) {

		error = devm_gpio_request_one(&pdev->dev, button->gpio,
					      GPIOF_IN, desc);
		if (error < 0) {
			dev_err(dev, "Failed to request GPIO %d, error %d\n",
				button->gpio, error);
			return error;
		}

		if (button->debounce_interval) {
			error = gpio_set_debounce(button->gpio,
					button->debounce_interval * 1000);
			/* use timer if gpiolib doesn't provide debounce */
			if (error < 0)
				bdata->timer_debounce =
						button->debounce_interval;
		}

		irq = gpio_to_irq(button->gpio);
		if (irq < 0) {
			error = irq;
			dev_err(dev,
				"Unable to get irq number for GPIO %d, error %d\n",
				button->gpio, error);
			return error;
		}
		bdata->irq = irq;

		INIT_WORK(&bdata->work, gpio_keys_gpio_work_func);
		setup_timer(&bdata->timer,
			    gpio_keys_gpio_timer, (unsigned long)bdata);

		isr = gpio_keys_gpio_isr;
		irqflags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;

	} else {
		if (!button->irq) {
			dev_err(dev, "No IRQ specified\n");
			return -EINVAL;
		}
		bdata->irq = button->irq;

		if (button->type && button->type != EV_KEY) {
			dev_err(dev, "Only EV_KEY allowed for IRQ buttons.\n");
			return -EINVAL;
		}

		bdata->timer_debounce = button->debounce_interval;
		setup_timer(&bdata->timer,
			    gpio_keys_irq_timer, (unsigned long)bdata);

		isr = gpio_keys_irq_isr;
		irqflags = 0;
	}

	input_set_capability(input, button->type ?: EV_KEY, button->code);

	/*
	 * Install custom action to cancel debounce timer and
	 * workqueue item.
	 */
	error = devm_add_action(&pdev->dev, gpio_keys_quiesce_key, bdata);
	if (error) {
		dev_err(&pdev->dev,
			"failed to register quiesce action, error: %d\n",
			error);
		return error;
	}

	/*
	 * If platform has specified that the button can be disabled,
	 * we don't want it to share the interrupt line.
	 */
	if (!button->can_disable)
		irqflags |= IRQF_SHARED;

	error = devm_request_any_context_irq(&pdev->dev, bdata->irq,
					     isr, irqflags, desc, bdata);
	if (error < 0) {
		dev_err(dev, "Unable to claim irq %d; error %d\n",
			bdata->irq, error);
		return error;
	}

	return 0;
}

/*static void gpio_keys_report_state(struct gpio_keys_drvdata *ddata)
{
	struct input_dev *input = ddata->input;
	int i;

	for (i = 0; i < ddata->pdata->nbuttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];
		if (gpio_is_valid(bdata->button->gpio))
			gpio_keys_gpio_report_event(bdata);
	}
	input_sync(input);
}*/

static int gpio_keys_pinctrl_configure(struct gpio_keys_drvdata *ddata,
							bool active)
{
	struct pinctrl_state *set_state;
	int retval;

	if (active) {
		set_state =
			pinctrl_lookup_state(ddata->key_pinctrl,
						"tlmm_gpio_key_active");
		if (IS_ERR(set_state)) {
			dev_err(&ddata->input->dev,
				"cannot get ts pinctrl active state\n");
			return PTR_ERR(set_state);
		}
	} else {
		set_state =
			pinctrl_lookup_state(ddata->key_pinctrl,
						"tlmm_gpio_key_suspend");
		if (IS_ERR(set_state)) {
			dev_err(&ddata->input->dev,
				"cannot get gpiokey pinctrl sleep state\n");
			return PTR_ERR(set_state);
		}
	}
	retval = pinctrl_select_state(ddata->key_pinctrl, set_state);
	if (retval) {
		dev_err(&ddata->input->dev,
				"cannot set ts pinctrl active state\n");
		return retval;
	}

	return 0;
}

static int gpio_keys_open(struct input_dev *input)
{
	struct gpio_keys_drvdata *ddata = input_get_drvdata(input);
	const struct gpio_keys_platform_data *pdata = ddata->pdata;
	int error;

	if (pdata->enable) {
		error = pdata->enable(input->dev.parent);
		if (error)
			return error;
	}

	/* Report current state of buttons that are connected to GPIOs */
	//gpio_keys_report_state(ddata);

	return 0;
}

static void gpio_keys_close(struct input_dev *input)
{
	struct gpio_keys_drvdata *ddata = input_get_drvdata(input);
	const struct gpio_keys_platform_data *pdata = ddata->pdata;

	if (pdata->disable)
		pdata->disable(input->dev.parent);
}

/*
 * Handlers for alternative sources of platform_data
 */

#ifdef CONFIG_OF
/*
 * Translate OpenFirmware node properties into platform_data
 */
static struct gpio_keys_platform_data *
gpio_keys_get_devtree_pdata(struct device *dev)
{
	struct device_node *node, *pp;
	struct gpio_keys_platform_data *pdata;
	struct gpio_keys_button *button;
	int error;
	int nbuttons;
	int i;

	node = dev->of_node;
	if (!node)
		return ERR_PTR(-ENODEV);

	nbuttons = of_get_child_count(node);
	if (nbuttons == 0)
		return ERR_PTR(-ENODEV);

	pdata = devm_kzalloc(dev,
			     sizeof(*pdata) + nbuttons * sizeof(*button),
			     GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->buttons = (struct gpio_keys_button *)(pdata + 1);
	pdata->nbuttons = nbuttons;

	pdata->rep = !!of_get_property(node, "autorepeat", NULL);
	pdata->name = of_get_property(node, "input-name", NULL);
	pdata->use_syscore = of_property_read_bool(node, "use-syscore");

	i = 0;
	for_each_child_of_node(node, pp) {
		int gpio;
		enum of_gpio_flags flags;

		if (!of_find_property(pp, "gpios", NULL)) {
			pdata->nbuttons--;
			dev_warn(dev, "Found button without gpios\n");
			continue;
		}

		gpio = of_get_gpio_flags(pp, 0, &flags);
		if (gpio < 0) {
			error = gpio;
			if (error != -EPROBE_DEFER)
				dev_err(dev,
					"Failed to get gpio flags, error: %d\n",
					error);
			return ERR_PTR(error);
		}

		button = &pdata->buttons[i++];

		button->gpio = gpio;
		button->active_low = flags & OF_GPIO_ACTIVE_LOW;

		if (of_property_read_u32(pp, "linux,code", &button->code)) {
			dev_err(dev, "Button without keycode: 0x%x\n",
				button->gpio);
			return ERR_PTR(-EINVAL);
		}

		button->desc = of_get_property(pp, "label", NULL);

		if (of_property_read_u32(pp, "linux,input-type", &button->type))
			button->type = EV_KEY;

		button->wakeup = !!of_get_property(pp, "gpio-key,wakeup", NULL);

		if (of_property_read_u32(pp, "debounce-interval",
					&button->debounce_interval))
			button->debounce_interval = 5;
	}

	if (pdata->nbuttons == 0)
		return ERR_PTR(-EINVAL);

	return pdata;
}

static const struct of_device_id gpio_keys_of_match[] = {
	{ .compatible = "gpio-keys", },
	{ },
};
MODULE_DEVICE_TABLE(of, gpio_keys_of_match);

#else

static inline struct gpio_keys_platform_data *
gpio_keys_get_devtree_pdata(struct device *dev)
{
	return ERR_PTR(-ENODEV);
}

#endif

static int gpio_keys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct gpio_keys_platform_data *pdata = dev_get_platdata(dev);
	struct gpio_keys_drvdata *ddata;
	struct input_dev *input;
	size_t size;
	int i, error;
	int wakeup = 0;
	struct pinctrl_state *set_state;

	printk("[Keys][gpio_keys.c] gpio_keys_probe() +++\n");

	if (!pdata) {
		pdata = gpio_keys_get_devtree_pdata(dev);
		if (IS_ERR(pdata))
			return PTR_ERR(pdata);
	}

	size = sizeof(struct gpio_keys_drvdata) +
			pdata->nbuttons * sizeof(struct gpio_button_data);
	ddata = devm_kzalloc(dev, size, GFP_KERNEL);
	if (!ddata) {
		dev_err(dev, "failed to allocate state\n");
		return -ENOMEM;
	}

	input = devm_input_allocate_device(dev);
	if (!input) {
		dev_err(dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	global_dev = dev;
	ddata->pdata = pdata;
	ddata->input = input;
	mutex_init(&ddata->disable_lock);

	platform_set_drvdata(pdev, ddata);
	input_set_drvdata(input, ddata);

	input->name = GPIO_KEYS_DEV_NAME;
	input->phys = "gpio-keys/input0";
	input->dev.parent = &pdev->dev;
	input->open = gpio_keys_open;
	input->close = gpio_keys_close;

	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;

	/* Enable auto repeat feature of Linux input subsystem */
	if (pdata->rep)
		__set_bit(EV_REP, input->evbit);

	/* Get pinctrl if target uses pinctrl */
	ddata->key_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(ddata->key_pinctrl)) {
		if (PTR_ERR(ddata->key_pinctrl) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		pr_debug("Target does not use pinctrl\n");
		ddata->key_pinctrl = NULL;
	}

	if (ddata->key_pinctrl) {
		error = gpio_keys_pinctrl_configure(ddata, true);
		if (error) {
			dev_err(dev, "cannot set ts pinctrl active state\n");
			return error;
		}
	}

	for (i = 0; i < pdata->nbuttons; i++) {
		struct gpio_keys_button *button = &pdata->buttons[i]; //ASUS BSP Vincent remover const
		struct gpio_button_data *bdata = &ddata->data[i];

		error = gpio_keys_setup_key(pdev, input, bdata, button);
		if (error)
			goto err_setup_key;

		if (button->wakeup)
			wakeup = 1;
	}

	error = sysfs_create_group(&pdev->dev.kobj, &gpio_keys_attr_group);
	if (error) {
		dev_err(dev, "Unable to export keys/switches, error: %d\n",
			error);
		goto err_create_sysfs;
	}

	error = input_register_device(input);
	if (error) {
		dev_err(dev, "Unable to register input device, error: %d\n",
			error);
		goto err_remove_group;
	}

	device_init_wakeup(&pdev->dev, wakeup);
	wake_lock_init(&pwr_key_wake_lock, WAKE_LOCK_SUSPEND, "pwr_key_lock");
	printk(KERN_INFO "[Gpio_keys]Initialize a wakelock for gpio_key\r\n");

	if (pdata->use_syscore)
		gpio_keys_syscore_pm_ops.resume = gpio_keys_syscore_resume;

	register_syscore_ops(&gpio_keys_syscore_pm_ops);
	printk("[Keys][gpio_keys.c] gpio_keys_probe() ---\n");
	return 0;

err_remove_group:
	sysfs_remove_group(&pdev->dev.kobj, &gpio_keys_attr_group);
err_create_sysfs:
err_setup_key:
	if (ddata->key_pinctrl) {
		set_state =
		pinctrl_lookup_state(ddata->key_pinctrl,
						"tlmm_gpio_key_suspend");
		if (IS_ERR(set_state))
			dev_err(dev, "cannot get gpiokey pinctrl sleep state\n");
		else
			pinctrl_select_state(ddata->key_pinctrl, set_state);
	}

	return error;
}

static int gpio_keys_remove(struct platform_device *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &gpio_keys_attr_group);
	unregister_syscore_ops(&gpio_keys_syscore_pm_ops);

	device_init_wakeup(&pdev->dev, 0);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static void gpio_keys_syscore_resume(void)
{
	struct gpio_keys_drvdata *ddata = dev_get_drvdata(global_dev);
	struct input_dev *input = ddata->input;
	struct gpio_button_data *bdata = NULL;
	int error = 0;
	int i;

	if (ddata->key_pinctrl) {
		error = gpio_keys_pinctrl_configure(ddata, true);
		if (error) {
			dev_err(global_dev, "failed to put the pin in resume state\n");
			return;
		}
	}

	if (device_may_wakeup(global_dev)) {
		for (i = 0; i < ddata->pdata->nbuttons; i++) {
			bdata = &ddata->data[i];
			if (bdata->button->wakeup)
				disable_irq_wake(bdata->irq);
		}
	} else {
		mutex_lock(&input->mutex);
		if (input->users)
			error = gpio_keys_open(input);
		mutex_unlock(&input->mutex);
	}

	if (error)
		return;

	//gpio_keys_report_state(ddata);  //ASUS_BSP : Eric
}

static int gpio_keys_suspend(struct device *dev)
{
	struct gpio_keys_drvdata *ddata = dev_get_drvdata(dev);
	struct input_dev *input = ddata->input;
	int i, ret;

	if (ddata->key_pinctrl) {
		ret = gpio_keys_pinctrl_configure(ddata, false);
		if (ret) {
			dev_err(dev, "failed to put the pin in suspend state\n");
			return ret;
		}
	}

	if (device_may_wakeup(dev)) {
		for (i = 0; i < ddata->pdata->nbuttons; i++) {
			struct gpio_button_data *bdata = &ddata->data[i];
			if (bdata->button->wakeup)
				enable_irq_wake(bdata->irq);
		}
	} else {
		mutex_lock(&input->mutex);
		if (input->users)
			gpio_keys_close(input);
		mutex_unlock(&input->mutex);
	}

	return 0;
}

static int gpio_keys_resume(struct device *dev)
{
	struct gpio_keys_drvdata *ddata = dev_get_drvdata(dev);
	struct input_dev *input = ddata->input;
	int error = 0;
	int i;

	if (ddata->pdata->use_syscore == true) {
		dev_dbg(global_dev, "Using syscore resume, no need of this resume.\n");
		return 0;
	}

	if (ddata->key_pinctrl) {
		error = gpio_keys_pinctrl_configure(ddata, true);
		if (error) {
			dev_err(dev, "failed to put the pin in resume state\n");
			return error;
		}
	}

	if (device_may_wakeup(dev)) {
		for (i = 0; i < ddata->pdata->nbuttons; i++) {
			struct gpio_button_data *bdata = &ddata->data[i];
			if (bdata->button->wakeup)
				disable_irq_wake(bdata->irq);
		}
	} else {
		mutex_lock(&input->mutex);
		if (input->users)
			error = gpio_keys_open(input);
		mutex_unlock(&input->mutex);
	}

	if (error)
		return error;

	//gpio_keys_report_state(ddata);
	return 0;
}

//ASUS BSP Austin_T+++ : Fix DoubleClickVolumeKey sometimes can't bring up panel
static int gpio_keys_suspend_noirq(struct device *dev)
{

       g_bResume = 0;
       printk("[Gpio_keys]gpio_keys_suspend_noirq\n");

       return 0;
}

static int gpio_keys_resume_noirq(struct device *dev)
{
	struct gpio_keys_drvdata *ddata = dev_get_drvdata(dev);

	ddata->force_trigger = 1;
	g_bResume = 1;
	printk("[Gpio_keys]gpio_keys_resume_noirq\n");

	return 0;
}

static const struct dev_pm_ops gpio_keys_pm_ops = {
	.suspend	= gpio_keys_suspend,
	.resume		= gpio_keys_resume,
	.suspend_noirq  = gpio_keys_suspend_noirq,
	.resume_noirq   = gpio_keys_resume_noirq,
};

#else

static void gpio_keys_syscore_resume(void){}

static int gpio_keys_suspend(struct device *dev)
{
	return 0;
}

static int gpio_keys_resume(struct device *dev)
{
	return 0;
}

#endif

//static SIMPLE_DEV_PM_OPS(gpio_keys_pm_ops, gpio_keys_suspend, gpio_keys_resume);

static struct platform_driver gpio_keys_device_driver = {
	.probe		= gpio_keys_probe,
	.remove		= gpio_keys_remove,
	.driver		= {
		.name	= "gpio-keys",
		.owner	= THIS_MODULE,
#ifdef CONFIG_PM_SLEEP
		.pm	= &gpio_keys_pm_ops,
#endif
		.of_match_table = of_match_ptr(gpio_keys_of_match),
	}
};
//ASUS BSP Austin_T--- : Fix DoubleClickVolumeKey sometimes can't bring up panel

static int __init gpio_keys_init(void)
{
	INIT_WORK(&__keysavelog_work, keysavelog_func);
	return platform_driver_register(&gpio_keys_device_driver);
}

static void __exit gpio_keys_exit(void)
{
	platform_driver_unregister(&gpio_keys_device_driver);
}

late_initcall(gpio_keys_init);
module_exit(gpio_keys_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Phil Blundell <pb@handhelds.org>");
MODULE_DESCRIPTION("Keyboard driver for GPIOs");
MODULE_ALIAS("platform:gpio-keys");
