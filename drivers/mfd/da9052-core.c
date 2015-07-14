/*
 * da9052-core.c  --  Device access for Dialog DA9052
 *
 * Copyright(c) 2009 Dialog Semiconductor Ltd.
 *
 * Author: Dialog Semiconductor Ltd <dchen@diasemi.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/list.h>
#include <linux/mfd/core.h>
#include <linux/spi/spi.h>
#include <linux/i2c.h>
#include <linux/semaphore.h>

#include <linux/mfd/da9052/da9052.h>
#include <linux/mfd/da9052/adc.h>

/* Add Sanyo CE*/
#include <linux/rtc.h>
#include <linux/mfd/da9052/rtc.h>
/* Add Sanyo CE End*/

#define SUCCESS		0
#define FAILURE		1

struct da9052_eh_nb eve_nb_array[EVE_CNT];
static struct da9052_ssc_ops ssc_ops;
struct mutex manconv_lock;
static struct semaphore eve_nb_array_lock;
static struct da9052 *da9052_data;

void da9052_lock(struct da9052 *da9052)
{
	mutex_lock(&da9052->ssc_lock);
}
EXPORT_SYMBOL(da9052_lock);

void da9052_unlock(struct da9052 *da9052)
{
	mutex_unlock(&da9052->ssc_lock);
}
EXPORT_SYMBOL(da9052_unlock);

int da9052_ssc_write(struct da9052 *da9052, struct da9052_ssc_msg *sscmsg)
{
	int ret = 0;

	/* Reg address should be a valid address on PAGE0 or PAGE1 */
	if ((sscmsg->addr < DA9052_PAGE0_REG_START) ||
	(sscmsg->addr > DA9052_PAGE1_REG_END) ||
	((sscmsg->addr > DA9052_PAGE0_REG_END) &&
	(sscmsg->addr < DA9052_PAGE1_REG_START)))
		return INVALID_REGISTER;

	ret = ssc_ops.write(da9052, sscmsg);

	/* Update local cache if required */
	if (!ret) {
		/* Check if this register is Non-volatile*/
		if (da9052->ssc_cache[sscmsg->addr].type != VOLATILE) {
			/* Update value */
			da9052->ssc_cache[sscmsg->addr].val = sscmsg->data;
			/* Make this cache entry valid */
			da9052->ssc_cache[sscmsg->addr].status = VALID;
		}
	}

	return ret;
}

int da9052_ssc_read(struct da9052 *da9052, struct da9052_ssc_msg *sscmsg)
{
	int ret = 0;

	/* Reg addr should be a valid address on PAGE0 or PAGE1 */
	if ((sscmsg->addr < DA9052_PAGE0_REG_START) ||
	(sscmsg->addr > DA9052_PAGE1_REG_END) ||
	((sscmsg->addr > DA9052_PAGE0_REG_END) &&
	(sscmsg->addr < DA9052_PAGE1_REG_START)))
		return INVALID_REGISTER;

	/*
	 * Check if this is a Non-volatile register, if yes then return value -
	 * from cache instead of actual reading from hardware. Before reading -
	 * cache entry, make sure that the entry is valid
	 */
	/* The read request is for Non-volatile register */
	/* Check if we have valid cached value for this */
	if (da9052->ssc_cache[sscmsg->addr].status == VALID) {
		/* We have valid cached value, copy this value */
		sscmsg->data = da9052->ssc_cache[sscmsg->addr].val;

		return 0;
	}

	ret = ssc_ops.read(da9052, sscmsg);

	/* Update local cache if required */
	if (!ret) {
		/* Check if this register is Non-volatile*/
		if (da9052->ssc_cache[sscmsg->addr].type != VOLATILE) {
			/* Update value */
			da9052->ssc_cache[sscmsg->addr].val = sscmsg->data;
			/* Make this cache entry valid */
			da9052->ssc_cache[sscmsg->addr].status = VALID;
		}
	}

	return ret;
}

int da9052_ssc_write_many(struct da9052 *da9052, struct da9052_ssc_msg *sscmsg,
				int msg_no)
{
	int ret = 0;
	int cnt = 0;

	/* Check request size */
	if (msg_no > MAX_READ_WRITE_CNT)
		return -EIO;

	ret = ssc_ops.write_many(da9052, sscmsg, msg_no);
	/* Update local cache, if required */
	for (cnt = 0; cnt < msg_no; cnt++) {
		/* Check if this register is Non-volatile*/
		if (da9052->ssc_cache[sscmsg[cnt].addr].type != VOLATILE) {
			/* Update value */
			da9052->ssc_cache[sscmsg[cnt].addr].val =
			sscmsg[cnt].data;
			/* Make this cache entry valid */
			da9052->ssc_cache[sscmsg[cnt].addr].status = VALID;
		}
	}
	return ret;
}

int da9052_ssc_read_many(struct da9052 *da9052, struct da9052_ssc_msg *sscmsg,
			int msg_no)
{
	int ret = 0;
	int cnt = 0;

	/* Check request size */
	if (msg_no > MAX_READ_WRITE_CNT)
		return -EIO;

	ret = ssc_ops.read_many(da9052, sscmsg, msg_no);
	/* Update local cache, if required */
	for (cnt = 0; cnt < msg_no; cnt++) {
		/* Check if this register is Non-volatile*/
		if (da9052->ssc_cache[sscmsg[cnt].addr].type
			!= VOLATILE) {
			/* Update value */
			da9052->ssc_cache[sscmsg[cnt].addr].val =
			sscmsg[cnt].data;
			/* Make this cache entry valid */
			da9052->ssc_cache[sscmsg[cnt].addr].status = VALID;
		}
	}
	return ret;
}

static irqreturn_t da9052_eh_isr(int irq, void *dev_id)
{
	struct da9052 *da9052 = dev_id;
	/* Schedule work to be done */
	schedule_work(&da9052->eh_isr_work);
	/* Disable IRQ */
	disable_irq_nosync(da9052->irq);
	return IRQ_HANDLED;
}

int eh_register_nb(struct da9052 *da9052, struct da9052_eh_nb *nb)
{

	if (nb == NULL) {
		printk(KERN_INFO "EH REGISTER FUNCTION FAILED\n");
		return -EINVAL;
	}

	if (nb->eve_type >= EVE_CNT) {
		printk(KERN_INFO "Invalid DA9052 Event Type\n");
		return -EINVAL;
	}

	/* Initialize list head inside notifier block */
	INIT_LIST_HEAD(&nb->nb_list);

	/* Acquire NB array lock */
	if (down_interruptible(&eve_nb_array_lock))
		return -EAGAIN;

	/* Add passed NB to corresponding EVENT list */
	list_add_tail(&nb->nb_list, &(eve_nb_array[nb->eve_type].nb_list));

	/* Release NB array lock */
	up(&eve_nb_array_lock);

	return 0;
}

int eh_unregister_nb(struct da9052 *da9052, struct da9052_eh_nb *nb)
{

	if (nb == NULL)
		return -EINVAL;

	/* Acquire nb array lock */
	if (down_interruptible(&eve_nb_array_lock))
		return -EAGAIN;

	/* Remove passed NB from list */
	list_del_init(&(nb->nb_list));

	/* Release NB array lock */
	up(&eve_nb_array_lock);

	return 0;
}

static int process_events(struct da9052 *da9052, int events_sts)
{

	int cnt = 0;
	int tmp_events_sts = 0;
	unsigned char event = 0;

	struct list_head *ptr;
	struct da9052_eh_nb *nb_ptr;

	/* Now we have retrieved all events, process them one by one */
	//for (cnt = 0; cnt < PRIO_CNT; cnt++) {
	for (cnt = 0; cnt < EVE_CNT; cnt++) {
		/*
		 * Starting with highest priority event,
		 * traverse through all event
		 */
		tmp_events_sts = events_sts;

		/* Find the event associated with higher priority */

		/* KPIT NOTE: This is commented as we are not using prioritization */
		//	event = eve_prio_map[cnt];
		event = cnt;

		/* Check if interrupt is received for this event */
		/* KPIT : event is commneted to not use prioritization */
		//if (!((tmp_events_sts >> event) & 0x1)) {
		if (!((tmp_events_sts >> cnt) & 0x1))
			/* Event bit is not set for this event */
			/* Move to next priority event */
			continue;

		if (event == PEN_DOWN_EVE) {
			if (list_empty(&(eve_nb_array[event].nb_list)))
				continue;
		}
		/* Event bit is set, execute all registered call backs */
		if (down_interruptible(&eve_nb_array_lock)){
			printk(KERN_CRIT "Can't acquire eve_nb_array_lock \n");
			return -EIO;
		}

		list_for_each(ptr, &(eve_nb_array[event].nb_list)) {
			/*
			 * nb_ptr will point to the structure in which
			 * nb_list is embedded
			 */
			nb_ptr = list_entry(ptr, struct da9052_eh_nb, nb_list);
			nb_ptr->call_back(nb_ptr, events_sts);
		}
		up(&eve_nb_array_lock);
	}
	return 0;
}

void eh_workqueue_isr(struct work_struct *work)
{
	struct da9052 *da9052 =
		container_of(work, struct da9052, eh_isr_work);

	struct da9052_ssc_msg eve_data[4];
	int events_sts, ret;
	unsigned char cnt = 0;

	/* nIRQ is asserted, read event registeres to know what happened */
	events_sts = 0;

	/* Prepare ssc message to read all four event registers */
	for (cnt = 0; cnt < 4; cnt++) {
		eve_data[cnt].addr = (DA9052_EVENTA_REG + cnt);
		eve_data[cnt].data = 0;
	}

	/* Now read all event registers */
	da9052_lock(da9052);

	ret = da9052_ssc_read_many(da9052, eve_data, 4);
	if (ret) {
		enable_irq(da9052->irq);
		da9052_unlock(da9052);
		return;
	}

	/* Collect all events */
	for (cnt = 0; cnt < 4; cnt++)
		events_sts |= ((eve_data[cnt].data&0x00ff) << (8 * cnt));

	/* Check if we really got any event */
	if (events_sts == 0) {
		enable_irq(da9052->irq);
		da9052_unlock(da9052);
		return;
	}
	da9052_unlock(da9052);

	/* Process all events occurred */
	process_events(da9052, events_sts);

	da9052_lock(da9052);
	/* Now clear EVENT registers */
	for (cnt = 0; cnt < 4; cnt++) {
		if (eve_data[cnt].data) {
			ret = da9052_ssc_write(da9052, &eve_data[cnt]);
			if (ret) {
				enable_irq(da9052->irq);
				da9052_unlock(da9052);
				return;
			}
		}
	}
	da9052_unlock(da9052);

	/*
	 * This delay is necessary to avoid hardware fake interrupts
	 * from DA9052.
	 */
	udelay(50);
	/* Enable HOST interrupt */
	enable_irq(da9052->irq);
}

static void da9052_eh_restore_irq(struct da9052 *da9052)
{
	/* Put your platform and board specific code here */
	free_irq(da9052->irq, NULL);
}

static int da9052_add_subdevice_pdata(struct da9052 *da9052,
		const char *name, void *pdata, size_t pdata_size)
{
	struct mfd_cell cell = {
		.name = name,
		.platform_data = pdata,
		.data_size = pdata_size,
	};
	return mfd_add_devices(da9052->dev, -1, &cell, 1, NULL, 0);
}

static int da9052_add_subdevice(struct da9052 *da9052, const char *name)
{
	return da9052_add_subdevice_pdata(da9052, name, NULL, 0);
}

static int add_da9052_devices(struct da9052 *da9052)
{
	s32 ret = 0;
	struct da9052_platform_data *pdata = da9052->dev->platform_data;
	struct da9052_leds_platform_data leds_data = {
			.num_leds = pdata->led_data->num_leds,
			.led = pdata->led_data->led,
	};
	struct da9052_regulator_platform_data regulator_pdata = {
		.regulators = pdata->regulators,
	};

	struct da9052_tsi_platform_data tsi_data = *(pdata->tsi_data);
	struct da9052_bat_platform_data bat_data = *(pdata->bat_data);

	if (pdata && pdata->init) {
		ret = pdata->init(da9052);
		if (ret != 0)
			return ret;
	} else
		pr_err("No platform initialisation supplied\n");
	ret = da9052_add_subdevice(da9052, "da9052-rtc");
	if (ret)
		return ret;
	ret = da9052_add_subdevice(da9052, "da9052-onkey");
	if (ret)
		return ret;

	ret = da9052_add_subdevice(da9052, "WLED-1");
	if (ret)
		return ret;

	ret = da9052_add_subdevice(da9052, "WLED-2");
	if (ret)
		return ret;

	ret = da9052_add_subdevice(da9052, "WLED-3");
	if (ret)
		return ret;

	ret = da9052_add_subdevice(da9052, "da9052-adc");
	if (ret)
		return ret;

	ret = da9052_add_subdevice(da9052, "da9052-wdt");
	if (ret)
		return ret;

	ret = da9052_add_subdevice_pdata(da9052, "da9052-leds",
				&leds_data, sizeof(leds_data));
	if (ret)
		return ret;

	ret = da9052_add_subdevice_pdata(da9052, "da9052-regulator",
		&regulator_pdata, sizeof(regulator_pdata));
	if (ret)
		return ret;

	ret = da9052_add_subdevice_pdata(da9052, "da9052-tsi",
				&tsi_data, sizeof(tsi_data));
	if (ret)
		return ret;

	ret = da9052_add_subdevice_pdata(da9052, "da9052-bat",
				&bat_data, sizeof(bat_data));
	if (ret)
		return ret;

	return ret;
}

int da9052_ssc_init(struct da9052 *da9052)
{
	int cnt;
	struct da9052_platform_data *pdata;
	struct da9052_ssc_msg ssc_msg;

	/* Initialize eve_nb_array */
	for (cnt = 0; cnt < EVE_CNT; cnt++)
		INIT_LIST_HEAD(&(eve_nb_array[cnt].nb_list));

	/* Initialize mutex required for ADC Manual read */
	mutex_init(&manconv_lock);

	/* Initialize NB array lock */
	init_MUTEX(&eve_nb_array_lock);

	/* Assign the read-write function pointers */
	da9052->read = da9052_ssc_read;
	da9052->write = da9052_ssc_write;
	da9052->read_many = da9052_ssc_read_many;
	da9052->write_many = da9052_ssc_write_many;

	if (SPI  == da9052->connecting_device && ssc_ops.write == NULL) {
		/* Assign the read/write pointers to SPI/read/write */
		ssc_ops.write = da9052_spi_write;
		ssc_ops.read = da9052_spi_read;
		ssc_ops.write_many = da9052_spi_write_many;
		ssc_ops.read_many = da9052_spi_read_many;
	}
	else if (I2C  == da9052->connecting_device && ssc_ops.write == NULL) {
		/* Assign the read/write pointers to SPI/read/write */
		ssc_ops.write = da9052_i2c_write;
		ssc_ops.read = da9052_i2c_read;
		ssc_ops.write_many = da9052_i2c_write_many;
		ssc_ops.read_many = da9052_i2c_read_many;
	} else
		return -1;

	/* Assign the EH notifier block register/de-register functions */
	da9052->register_event_notifier = eh_register_nb;
	da9052->unregister_event_notifier = eh_unregister_nb;

	/* Initialize ssc lock */
	mutex_init(&da9052->ssc_lock);

	pdata = da9052->dev->platform_data;
	add_da9052_devices(da9052);

	INIT_WORK(&da9052->eh_isr_work, eh_workqueue_isr);

	ssc_msg.addr = DA9052_IRQMASKA_REG;
	ssc_msg.data = 0xff;
	da9052->write(da9052, &ssc_msg);
	ssc_msg.addr = DA9052_IRQMASKB_REG;
	ssc_msg.data = 0xde;
	da9052->write(da9052, &ssc_msg);
	ssc_msg.addr = DA9052_IRQMASKC_REG;
	ssc_msg.data = 0xff;
	da9052->write(da9052, &ssc_msg);

	ssc_msg.addr = DA9052_CONTROLC_REG;
	da9052->read(da9052, &ssc_msg);
//	pr_info("DA9052_CONTROLC_REG=0x%x ", ssc_msg.data);
	ssc_msg.data = (ssc_msg.data & ~(DA9052_CONTROLC_DEBOUNCING | DA9052_CONTROLC_PMFB2PIN)) | 0x04;//10ms
	da9052->write(da9052, &ssc_msg);
//	pr_info("DA9052_CONTROLC_REG=0x%x ", ssc_msg.data);

#if 0
	ssc_msg.addr = DA9052_BATCHG_REG;
	ssc_msg.data = 0x00;//chaging suspend
	da9052->write(da9052, &ssc_msg);
#endif

	/* read chip version */
	ssc_msg.addr = DA9052_CHIPID_REG;
	da9052->read(da9052, &ssc_msg);
	pr_info("DA9053 chip ID reg read=0x%x ", ssc_msg.data);
	if ((ssc_msg.data & DA9052_CHIPID_MRC) == 0x80) {
		da9052->chip_version = DA9053_VERSION_AA;
		pr_info("AA version probed\n");
	} else if ((ssc_msg.data & DA9052_CHIPID_MRC) == 0xa0) {
		da9052->chip_version = DA9053_VERSION_BB;
		pr_info("BB version probed\n");
	} else {
		da9052->chip_version = 0;
		pr_info("unknown chip version\n");
	}

	if (request_irq(da9052->irq, da9052_eh_isr, IRQ_TYPE_LEVEL_LOW,
		DA9052_EH_DEVICE_NAME, da9052))
		return -EIO;
	enable_irq_wake(da9052->irq);
	da9052_data = da9052;

	return 0;
}

void da9052_ssc_exit(struct da9052 *da9052)
{
	printk(KERN_INFO "DA9052: Unregistering SSC device.\n");
	mutex_destroy(&manconv_lock);
	/* Restore IRQ line */
	da9052_eh_restore_irq(da9052);
	free_irq(da9052->irq, NULL);
	mutex_destroy(&da9052->ssc_lock);
	mutex_destroy(&da9052->eve_nb_lock);
	return;
}

extern void mx53_bej_watchdog_en(int on);

void da9053_power_off(void)
{
	struct da9052_ssc_msg ssc_msg;
	if (!da9052_data)
		return;

	ssc_msg.addr = DA9052_CONTROLB_REG;
	da9052_data->read(da9052_data, &ssc_msg);
	ssc_msg.data |= DA9052_CONTROLB_SHUTDOWN;
	pr_info("da9052 shutdown: DA9052_CONTROLB_REG=%x\n", ssc_msg.data);
	da9052_data->write(da9052_data, &ssc_msg);
//	ssc_msg.addr = DA9052_GPID9_REG;
//	ssc_msg.data = 0;
//	da9052_data->read(da9052_data, &ssc_msg);
}

int da9053_get_chip_version(void)
{
	return da9052_data->chip_version;
}


/* Add Sanyo CE*/
static int da9052_rtc_validate_parameters(struct rtc_time *rtc_tm)
{

	if (rtc_tm->tm_sec > DA9052_RTC_SECONDS_LIMIT)
		return DA9052_RTC_INVALID_SECONDS;

	if (rtc_tm->tm_min > DA9052_RTC_MINUTES_LIMIT)
		return DA9052_RTC_INVALID_MINUTES;

	if (rtc_tm->tm_hour > DA9052_RTC_HOURS_LIMIT)
		return DA9052_RTC_INVALID_HOURS;

	if (rtc_tm->tm_mday == 0)
		return DA9052_RTC_INVALID_DAYS;

	if ((rtc_tm->tm_mon > DA9052_RTC_MONTHS_LIMIT) ||
	(rtc_tm->tm_mon == 0))
		return DA9052_RTC_INVALID_MONTHS;

	if (rtc_tm->tm_year > DA9052_RTC_YEARS_LIMIT)
		return DA9052_RTC_INVALID_YEARS;

	if ((rtc_tm->tm_mon == FEBRUARY)) {
		if (((rtc_tm->tm_year % 4 == 0) &&
			(rtc_tm->tm_year % 100 != 0)) ||
			(rtc_tm->tm_year % 400 == 0)) {
			if (rtc_tm->tm_mday > 29)
				return DA9052_RTC_INVALID_DAYS;
		} else if (rtc_tm->tm_mday > 28) {
			return DA9052_RTC_INVALID_DAYS;
		}
	}

	if (((rtc_tm->tm_mon == APRIL) || (rtc_tm->tm_mon == JUNE) ||
		(rtc_tm->tm_mon == SEPTEMBER) || (rtc_tm->tm_mon == NOVEMBER))
		&& (rtc_tm->tm_mday == 31)) {
		return DA9052_RTC_INVALID_DAYS;
	}


	return 0;
}

int da9053_rtc_set_time(struct rtc_time *rtc_tm)
{
	struct da9052_ssc_msg msg_arr[6];
	int validate_param = 0;
	unsigned char loop_index = 0;
	int ret = 0;

	/*2000年以前の値は、設定しない*/
	if(rtc_tm->tm_year < 100)
	{
		return DA9052_RTC_INVALID_YEARS;
	}

	/* System compatability */
	rtc_tm->tm_year -= 100;
	rtc_tm->tm_mon += 1;

	validate_param = da9052_rtc_validate_parameters(rtc_tm);
	if (validate_param)
		return validate_param;

//printk("da9052 %d,%d,%d,%d,%d,%d\n",rtc_tm->tm_year,rtc_tm->tm_mon,rtc_tm->tm_mday,rtc_tm->tm_hour,rtc_tm->tm_min,rtc_tm->tm_sec);

	msg_arr[loop_index].addr = DA9052_COUNTS_REG;
	msg_arr[loop_index++].data = DA9052_COUNTS_MONITOR | rtc_tm->tm_sec;

	msg_arr[loop_index].addr = DA9052_COUNTMI_REG;
	msg_arr[loop_index].data = 0;
	msg_arr[loop_index++].data = rtc_tm->tm_min;

	msg_arr[loop_index].addr = DA9052_COUNTH_REG;
	msg_arr[loop_index].data = 0;
	msg_arr[loop_index++].data = rtc_tm->tm_hour;

	msg_arr[loop_index].addr = DA9052_COUNTD_REG;
	msg_arr[loop_index].data = 0;
	msg_arr[loop_index++].data = rtc_tm->tm_mday;

	msg_arr[loop_index].addr = DA9052_COUNTMO_REG;
	msg_arr[loop_index].data = 0;
	msg_arr[loop_index++].data = rtc_tm->tm_mon;

	msg_arr[loop_index].addr = DA9052_COUNTY_REG;
	msg_arr[loop_index].data = 0;
	msg_arr[loop_index++].data = rtc_tm->tm_year;

	da9052_lock(da9052_data);
	ret = da9052_data->write_many(da9052_data, msg_arr, loop_index);
	if (ret != 0) {
		da9052_unlock(da9052_data);
		return ret;
	}

	da9052_unlock(da9052_data);
	return 0;
}

int da9053_rtc_get_time(struct rtc_time *rtc_tm)
{
	struct da9052_ssc_msg msg[6];
	unsigned char loop_index = 0;
	int validate_param = 0;
	int ret = 0;

	msg[loop_index].data = 0;
	msg[loop_index++].addr = DA9052_COUNTS_REG;

	msg[loop_index].data = 0;
	msg[loop_index++].addr = DA9052_COUNTMI_REG;

	msg[loop_index].data = 0;
	msg[loop_index++].addr = DA9052_COUNTH_REG;

	msg[loop_index].data = 0;
	msg[loop_index++].addr = DA9052_COUNTD_REG;

	msg[loop_index].data = 0;
	msg[loop_index++].addr = DA9052_COUNTMO_REG;

	msg[loop_index].data = 0;
	msg[loop_index++].addr = DA9052_COUNTY_REG;

	da9052_lock(da9052_data);
	ret = da9052_data->read_many(da9052_data, msg, loop_index);
	if (ret != 0) {
		da9052_unlock(da9052_data);
		return ret;
	}
	da9052_unlock(da9052_data);

	rtc_tm->tm_year = msg[--loop_index].data & DA9052_COUNTY_COUNTYEAR;
	rtc_tm->tm_mon = msg[--loop_index].data & DA9052_COUNTMO_COUNTMONTH;
	rtc_tm->tm_mday = msg[--loop_index].data & DA9052_COUNTD_COUNTDAY;
	rtc_tm->tm_hour = msg[--loop_index].data & DA9052_COUNTH_COUNTHOUR;
	rtc_tm->tm_min = msg[--loop_index].data & DA9052_COUNTMI_COUNTMIN;
	rtc_tm->tm_sec = msg[--loop_index].data & DA9052_COUNTS_COUNTSEC;

	validate_param = da9052_rtc_validate_parameters(rtc_tm);
	if (validate_param)
		return validate_param;

	/* System compatability */
	rtc_tm->tm_year += 100;
	rtc_tm->tm_mon -= 1;
	return 0;
}

int da9053_rtc_chk_valid(void)
{
	struct da9052_ssc_msg ssc_msg;

	if (!da9052_data)
		return 0;

	da9052_lock(da9052_data);
	ssc_msg.addr = DA9052_COUNTS_REG;
	da9052_data->read(da9052_data, &ssc_msg);
	da9052_unlock(da9052_data);

	if(ssc_msg.data & DA9052_COUNTS_MONITOR)
	{
		/*valid*/
		return 1;
	}

	return 0;
}
/* Add Sanyo CE End*/

MODULE_AUTHOR("Dialog Semiconductor Ltd <dchen@diasemi.com>");
MODULE_DESCRIPTION("DA9052 MFD Core");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DA9052_SSC_DEVICE_NAME);
