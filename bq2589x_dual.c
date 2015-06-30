/*
 * BQ2589x battery charging driver
 *
 * Copyright (C) 2013 Texas Instruments
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include "bq2589x_reg.h"

enum bq2589x_vbus_type {
	BQ2589X_VBUS_NONE,
	BQ2589X_VBUS_USB_SDP,
	BQ2589X_VBUS_USB_CDP,
	BQ2589X_VBUS_USB_DCP,
	BQ2589X_VBUS_MAXC,
	BQ2589X_VBUS_UNKNOWN,
	BQ2589X_VBUS_NONSTAND,
	BQ2589X_VBUS_OTG,
    BQ2589X_VBUS_TYPE_NUM,
};

enum bq2589x_part_no{
    BQ25890 = 0x03,
    BQ25892 = 0x00,
    BQ25895 = 0x07,
};


#define BQ2589X_STATUS_PLUGIN      	0x0001    //plugin
#define BQ2589X_STATUS_PG          	0x0002    //power good
//#define BQ2589X_STATUS_CHG_DONE    	0x0004
#define BQ2589X_STATUS_FAULT    	0x0008

#define BQ2589X_STATUS_EXIST		0x0100
#define BQ2589X_STATUS_CHARGE_ENABLE 0x0200

struct bq2589x {
	struct device *dev;
	struct i2c_client *client;
    
    enum   bq2589x_part_no part_no;
    int    revision;

    unsigned int    status;  //charger status:
	int		vbus_type;		//

	bool 	enabled;
	
    bool    interrupt;
	bool	use_absolute_vindpm;
	
	int		charge_volt;
	int		charge_current;

    int     vbus_volt;
    int     vbat_volt;

    int     vbus_volt_high_level;// voltage level that expect adapter output after tune up
    int     vbus_volt_low_level;// voltage level that expect adapter output after tune down
    int     vbat_min_volt_to_tuneup;// battery minimum voltage to tune up adapter

    int     rsoc;
	struct work_struct irq_work;
	struct work_struct adapter_in_work;
	struct work_struct adapter_out_work;
	struct delayed_work monitor_work;
	struct delayed_work ico_work;
	struct delayed_work volt_tune_work;
	struct delayed_work check_to_tuneup_work;
	struct delayed_work charger2_enable_work;

	
	
	struct power_supply usb;
	struct power_supply wall;

	wait_queue_head_t wq;
	
};

struct volt_control_t{
	bool TuneVoltwithPEplus;
	int TuneTargetVolt;
	bool toTuneDownVolt;
	bool toTuneUpVolt;
	bool TuneVoltDone;
	int TuneCounter;
	bool TuneFail;
};

static struct bq2589x *g_bq1;
static struct bq2589x *g_bq2;
static struct volt_control_t voltcontrol;
 
//static struct task_struct *charger_thread;


static DEFINE_MUTEX(bq2589x_i2c_lock);

static int bq2589x_read_byte(struct bq2589x *bq, u8 *data, u8 reg)
{
	int ret;

	mutex_lock(&bq2589x_i2c_lock);
	ret = i2c_smbus_read_byte_data(bq->client, reg);
	if (ret < 0) {
		dev_err(bq->dev, "failed to read 0x%.2x\n", reg);
		mutex_unlock(&bq2589x_i2c_lock);
		return ret;
	}

	*data = (u8)ret;
	mutex_unlock(&bq2589x_i2c_lock);
	
	return 0;
}

static int bq2589x_write_byte(struct bq2589x *bq, u8 reg, u8 data)
{
	int ret;
	mutex_lock(&bq2589x_i2c_lock);	
	ret = i2c_smbus_write_byte_data(bq->client, reg, data);
	mutex_unlock(&bq2589x_i2c_lock);
	return ret;
}

static int bq2589x_update_bits(struct bq2589x *bq, u8 reg, u8 mask, u8 data)
{
	int ret;
	u8 tmp;

	ret = bq2589x_read_byte(bq, &tmp, reg);
	
	if (ret)	
		return ret;

	tmp &= ~mask;
	tmp |= data & mask;
	
	return bq2589x_write_byte(bq, reg, tmp);
}


static enum bq2589x_vbus_type bq2589x_get_vbus_type(struct bq2589x *bq)
{
	u8 val = 0;
    int ret;
	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_0B);
    if (ret < 0) return 0;
	val &= BQ2589X_VBUS_STAT_MASK;
	val >>= BQ2589X_VBUS_STAT_SHIFT;

	return val;
}


static int bq2589x_enable_otg(struct bq2589x *bq)
{
    u8 val = BQ2589X_OTG_ENABLE << BQ2589X_OTG_CONFIG_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_03, BQ2589X_OTG_CONFIG_MASK, val);
	
}

static int bq2589x_disable_otg(struct bq2589x *bq)
{
    u8 val = BQ2589X_OTG_DISABLE << BQ2589X_OTG_CONFIG_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_03, BQ2589X_OTG_CONFIG_MASK, val);
	
}
EXPORT_SYMBOL_GPL(bq2589x_disable_otg);

static int bq2589x_set_otg_volt(struct bq2589x *bq, int volt )
{
    u8 val = 0;

    if (volt < BQ2589X_BOOSTV_BASE)
        volt = BQ2589X_BOOSTV_BASE;
    if (volt > BQ2589X_BOOSTV_BASE + (BQ2589X_BOOSTV_MASK >> BQ2589X_BOOSTV_SHIFT) * BQ2589X_BOOSTV_LSB)
        volt = BQ2589X_BOOSTV_BASE + (BQ2589X_BOOSTV_MASK >> BQ2589X_BOOSTV_SHIFT) * BQ2589X_BOOSTV_LSB;
    

	val = ((volt - BQ2589X_BOOSTV_BASE)/BQ2589X_BOOSTV_LSB) << BQ2589X_BOOSTV_SHIFT;
	
	return bq2589x_update_bits(bq,BQ2589X_REG_0A,BQ2589X_BOOSTV_MASK,val);

}
EXPORT_SYMBOL_GPL(bq2589x_set_otg_volt);

static int bq2589x_set_otg_current(struct bq2589x *bq, int curr )
{
	u8 temp;

    if(curr  == 500)
        temp = BQ2589X_BOOST_LIM_500MA;
    else if(curr == 700)
        temp = BQ2589X_BOOST_LIM_700MA;
    else if(curr == 1100)
        temp = BQ2589X_BOOST_LIM_1100MA;
    else if(curr == 1600)
        temp = BQ2589X_BOOST_LIM_1600MA;
    else if(curr == 1800)
        temp = BQ2589X_BOOST_LIM_1800MA;
    else if(curr == 2100)
        temp = BQ2589X_BOOST_LIM_2100MA;
    else if(curr == 2400)
        temp = BQ2589X_BOOST_LIM_2400MA;
    else
        temp = BQ2589X_BOOST_LIM_1300MA;

    return bq2589x_update_bits(bq,BQ2589X_REG_0A,BQ2589X_BOOST_LIM_MASK,temp << BQ2589X_BOOST_LIM_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_otg_current);

static int bq2589x_enable_charger(struct bq2589x *bq)
{
    int ret;
    u8 val = BQ2589X_CHG_ENABLE << BQ2589X_CHG_CONFIG_SHIFT;

    ret = bq2589x_update_bits(bq, BQ2589X_REG_03, BQ2589X_CHG_CONFIG_MASK, val);
	if(ret == 0)
        bq->status |= BQ2589X_STATUS_CHARGE_ENABLE;
    return ret;
}

static int bq2589x_disable_charger(struct bq2589x *bq)
{
    int ret;
    u8 val = BQ2589X_CHG_DISABLE << BQ2589X_CHG_CONFIG_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_03, BQ2589X_CHG_CONFIG_MASK, val);
	if(ret == 0)
        bq->status &=~BQ2589X_STATUS_CHARGE_ENABLE;
    return ret;    
}


/* interfaces that can be called by other module */
int bq2589x_adc_start(struct bq2589x *bq, bool oneshot)
{
    u8 val;
    int ret;

    ret = bq2589x_read_byte(bq,&val,BQ2589X_REG_02);
    if(ret < 0){
        dev_err(bq->dev,"%s failed to read register 0x02:%d\n",__func__,ret);
        return ret;
    }

    if(((val & BQ2589X_CONV_RATE_MASK) >> BQ2589X_CONV_RATE_SHIFT) == BQ2589X_ADC_CONTINUE_ENABLE)
        return 0; //is doing continuous scan
    if(oneshot)
        ret = bq2589x_update_bits(bq,BQ2589X_REG_02,BQ2589X_CONV_START_MASK, BQ2589X_CONV_START << BQ2589X_CONV_START_SHIFT);
    else
        ret = bq2589x_update_bits(bq,BQ2589X_REG_02,BQ2589X_CONV_RATE_MASK, BQ2589X_ADC_CONTINUE_ENABLE << BQ2589X_CONV_RATE_SHIFT);
    return ret;
}
EXPORT_SYMBOL_GPL(bq2589x_adc_start);

int bq2589x_adc_stop(struct bq2589x *bq)//stop continue scan 
{
    return bq2589x_update_bits(bq,BQ2589X_REG_02,BQ2589X_CONV_RATE_MASK, BQ2589X_ADC_CONTINUE_DISABLE << BQ2589X_CONV_RATE_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_adc_stop);


int bq2589x_adc_read_battery_volt(struct bq2589x *bq)
{
    uint8_t val;
    int volt;
    int ret;
	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_0E);
    if(ret < 0){
        dev_err(bq->dev,"read battery voltage failed :%d\n",ret);
        return ret;
    }
    else{
        volt = BQ2589X_BATV_BASE + ((val & BQ2589X_BATV_MASK) >> BQ2589X_BATV_SHIFT) * BQ2589X_BATV_LSB ;
        return volt;
    }
}
EXPORT_SYMBOL_GPL(bq2589x_adc_read_battery_volt);


int bq2589x_adc_read_sys_volt(struct bq2589x *bq)
{
    uint8_t val;
    int volt;
    int ret;
	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_0F);
    if(ret < 0){
        dev_err(bq->dev,"read system voltage failed :%d\n",ret);
        return ret;
    }
    else{
        volt = BQ2589X_SYSV_BASE + ((val & BQ2589X_SYSV_MASK) >> BQ2589X_SYSV_SHIFT) * BQ2589X_SYSV_LSB ;
        return volt;
    }
}
EXPORT_SYMBOL_GPL(bq2589x_adc_read_sys_volt);

int bq2589x_adc_read_vbus_volt(struct bq2589x *bq)
{
    uint8_t val;
    int volt;
    int ret;
	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_11);
    if(ret < 0){
        dev_err(bq->dev,"read vbus voltage failed :%d\n",ret);
        return ret;
    }
    else{
        volt = BQ2589X_VBUSV_BASE + ((val & BQ2589X_VBUSV_MASK) >> BQ2589X_VBUSV_SHIFT) * BQ2589X_VBUSV_LSB ;
        return volt;
    }
}
EXPORT_SYMBOL_GPL(bq2589x_adc_read_vbus_volt);

int bq2589x_adc_read_temperature(struct bq2589x *bq)
{
    uint8_t val;
    int temp;
    int ret;
	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_10);
    if(ret < 0){
        dev_err(bq->dev,"read temperature failed :%d\n",ret);
        return ret;
    }
    else{
        temp = BQ2589X_TSPCT_BASE + ((val & BQ2589X_TSPCT_MASK) >> BQ2589X_TSPCT_SHIFT) * BQ2589X_TSPCT_LSB ;
        return temp;
    }
}
EXPORT_SYMBOL_GPL(bq2589x_adc_read_temperature);

int bq2589x_adc_read_charge_current(struct bq2589x *bq)
{
    uint8_t val;
    int volt;
    int ret;
	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_12);
    if(ret < 0){
        dev_err(bq->dev,"read charge current failed :%d\n",ret);
        return ret;
    }
    else{
        volt = (int)(BQ2589X_ICHGR_BASE + ((val & BQ2589X_ICHGR_MASK) >> BQ2589X_ICHGR_SHIFT) * BQ2589X_ICHGR_LSB) ;
        return volt;
    }
}
EXPORT_SYMBOL_GPL(bq2589x_adc_read_charge_current);

int bq2589x_set_chargecurrent(struct bq2589x *bq,int curr)	
{

	u8 ichg;
    
    ichg = (curr - BQ2589X_ICHG_BASE)/BQ2589X_ICHG_LSB;
	return bq2589x_update_bits(bq, BQ2589X_REG_04,BQ2589X_ICHG_MASK, ichg << BQ2589X_ICHG_SHIFT);

}
EXPORT_SYMBOL_GPL(bq2589x_set_chargecurrent);

int bq2589x_set_term_current(struct bq2589x *bq,int curr)
{
    u8 iterm;

    iterm = (curr - BQ2589X_ITERM_BASE) / BQ2589X_ITERM_LSB;
    
    return bq2589x_update_bits(bq, BQ2589X_REG_05,BQ2589X_ITERM_MASK, iterm << BQ2589X_ITERM_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_term_current);


int bq2589x_set_prechg_current(struct bq2589x *bq,int curr)
{
    u8 iprechg;

    iprechg = (curr - BQ2589X_IPRECHG_BASE) / BQ2589X_IPRECHG_LSB;
    
    return bq2589x_update_bits(bq, BQ2589X_REG_05,BQ2589X_IPRECHG_MASK, iprechg << BQ2589X_IPRECHG_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_prechg_current);

int bq2589x_set_chargevoltage(struct bq2589x *bq,int volt)
{
	u8 val;

    val = (volt - BQ2589X_VREG_BASE)/BQ2589X_VREG_LSB;
	return bq2589x_update_bits(bq, BQ2589X_REG_06,BQ2589X_VREG_MASK, val << BQ2589X_VREG_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_chargevoltage);


int bq2589x_set_input_volt_limit(struct bq2589x *bq,int volt)
{
	u8 val;
    val = (volt - BQ2589X_VINDPM_BASE)/BQ2589X_VINDPM_LSB;
    return bq2589x_update_bits(bq, BQ2589X_REG_0D,BQ2589X_VINDPM_MASK, val << BQ2589X_VINDPM_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_input_volt_limit);

int bq2589x_set_input_current_limit(struct bq2589x *bq,int curr)
{

	u8 val;

    val = (curr - BQ2589X_IINLIM_BASE)/BQ2589X_IINLIM_LSB;
	return bq2589x_update_bits(bq, BQ2589X_REG_00,BQ2589X_IINLIM_MASK, val << BQ2589X_IINLIM_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_input_current_limit);


int bq2589x_set_vindpm_offset(struct bq2589x *bq, int offset)
{
	u8 val;
	
	val = (offset - BQ2589X_VINDPMOS_BASE)/BQ2589X_VINDPMOS_LSB;
	return bq2589x_update_bits(bq, BQ2589X_REG_01,BQ2589X_VINDPMOS_MASK, val << BQ2589X_VINDPMOS_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_vindpm_offset);


void bq2589x_start_charging(struct bq2589x *bq)
{
    bq2589x_enable_charger(bq);
}
EXPORT_SYMBOL_GPL(bq2589x_start_charging);

void bq2589x_stop_charging(struct bq2589x *bq)
{
    bq2589x_disable_charger(bq);
}
EXPORT_SYMBOL_GPL(bq2589x_stop_charging);

int bq2589x_get_charging_status(struct bq2589x *bq)
{
    u8 val = 0;
    int ret;

    ret = bq2589x_read_byte(bq,&val, BQ2589X_REG_0B);
    if(ret < 0){
        dev_err(bq->dev,"%s Failed to read register 0x0b:%d\n",__func__,ret);
        return ret;
    }
    val &= BQ2589X_CHRG_STAT_MASK;
    val >>= BQ2589X_CHRG_STAT_SHIFT;
    return val;
}
EXPORT_SYMBOL_GPL(bq2589x_get_charging_status);

void bq2589x_set_otg(struct bq2589x *bq,int enable)
{
    int ret;

    if(enable){
        ret = bq2589x_enable_otg(bq);
        if(ret < 0){
            dev_err(bq->dev,"%s:Failed to enable otg-%d\n",__func__,ret);
            return;
        }
    }
    else{
        ret = bq2589x_disable_otg(bq);
        if(ret < 0){
            dev_err(bq->dev,"%s:Failed to disable otg-%d\n",__func__,ret);
        }
    }
}
EXPORT_SYMBOL_GPL(bq2589x_set_otg);

int bq2589x_set_watchdog_timer(struct bq2589x *bq,u8 timeout)
{
    return bq2589x_update_bits(bq,BQ2589X_REG_07,BQ2589X_WDT_MASK, (u8)((timeout - BQ2589X_WDT_BASE)/BQ2589X_WDT_LSB)<< BQ2589X_WDT_SHIFT);
}
EXPORT_SYMBOL_GPL(bq2589x_set_watchdog_timer);

int bq2589x_disable_watchdog_timer(struct bq2589x *bq)
{
	u8 val = BQ2589X_WDT_DISABLE << BQ2589X_WDT_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_07, BQ2589X_WDT_MASK, val);
}
EXPORT_SYMBOL_GPL(bq2589x_disable_watchdog_timer);

int bq2589x_reset_watchdog_timer(struct bq2589x *bq)
{
	u8 val = BQ2589X_WDT_RESET << BQ2589X_WDT_RESET_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_03, BQ2589X_WDT_RESET_MASK, val);
}
EXPORT_SYMBOL_GPL(bq2589x_reset_watchdog_timer);

int bq2589x_force_dpdm(struct bq2589x *bq)
{
    int ret;
	u8 val = BQ2589X_FORCE_DPDM << BQ2589X_FORCE_DPDM_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_02, BQ2589X_FORCE_DPDM_MASK, val);
    if(ret) return ret;

    mdelay(10);//TODO: how much time needed to finish dpdm detect?
    return 0;

}
EXPORT_SYMBOL_GPL(bq2589x_force_dpdm);

int bq2589x_reset_chip(struct bq2589x *bq)
{
    int ret;
	u8 val = BQ2589X_RESET << BQ2589X_RESET_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_14, BQ2589X_RESET_MASK, val);
    return ret;
}
EXPORT_SYMBOL_GPL(bq2589x_reset_chip);

int bq2589x_enter_ship_mode(struct bq2589x *bq)
{
    int ret;
	u8 val = BQ2589X_BATFET_OFF << BQ2589X_BATFET_DIS_SHIFT;

	ret = bq2589x_update_bits(bq, BQ2589X_REG_09, BQ2589X_BATFET_DIS_MASK, val);
    return ret;

}
EXPORT_SYMBOL_GPL(bq2589x_enter_ship_mode);

int bq2589x_enter_hiz_mode(struct bq2589x *bq)
{
	u8 val = BQ2589X_HIZ_ENABLE << BQ2589X_ENHIZ_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_00, BQ2589X_ENHIZ_MASK, val);

}
EXPORT_SYMBOL_GPL(bq2589x_enter_hiz_mode);

int bq2589x_exit_hiz_mode(struct bq2589x *bq)
{

	u8 val = BQ2589X_HIZ_DISABLE << BQ2589X_ENHIZ_SHIFT;

	return bq2589x_update_bits(bq, BQ2589X_REG_00, BQ2589X_ENHIZ_MASK, val);

}
EXPORT_SYMBOL_GPL(bq2589x_exit_hiz_mode);

int bq2589x_get_hiz_mode(struct bq2589x *bq,u8* state)
{
    u8 val;
    int ret;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_00);
	if (ret) return ret;
    *state = (val & BQ2589X_ENHIZ_MASK) >> BQ2589X_ENHIZ_SHIFT;
	
    return 0;
}
EXPORT_SYMBOL_GPL(bq2589x_get_hiz_mode);


int bq2589x_pumpx_enable(struct bq2589x *bq,int enable)
{
    u8 val;
    int ret;

    if(enable)
        val = BQ2589X_PUMPX_ENABLE << BQ2589X_EN_PUMPX_SHIFT;
    else
        val = BQ2589X_PUMPX_DISABLE << BQ2589X_EN_PUMPX_SHIFT;

    ret = bq2589x_update_bits(bq, BQ2589X_REG_04, BQ2589X_EN_PUMPX_MASK, val);
    
    return ret;
}
EXPORT_SYMBOL_GPL(bq2589x_pumpx_enable);

int bq2589x_pumpx_increase_volt(struct bq2589x *bq)
{
    u8 val;
    int ret;

    val = BQ2589X_PUMPX_UP << BQ2589X_PUMPX_UP_SHIFT;

    ret = bq2589x_update_bits(bq, BQ2589X_REG_09, BQ2589X_PUMPX_UP_MASK, val);
    
    return ret;
   
}
EXPORT_SYMBOL_GPL(bq2589x_pumpx_increase_volt);

int bq2589x_pumpx_increase_volt_done(struct bq2589x *bq)
{
    u8 val;
    int ret;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_09);
    if(ret) return ret;

    if(val & BQ2589X_PUMPX_UP_MASK) 
        return 1;   // not finished
    else
        return 0;   // pumpx up finished
   
}
EXPORT_SYMBOL_GPL(bq2589x_pumpx_increase_volt_done);

int bq2589x_pumpx_decrease_volt(struct bq2589x *bq)
{
    u8 val;
    int ret;

    val = BQ2589X_PUMPX_DOWN << BQ2589X_PUMPX_DOWN_SHIFT;

    ret = bq2589x_update_bits(bq, BQ2589X_REG_09, BQ2589X_PUMPX_DOWN_MASK, val);
    
    return ret;
   
}
EXPORT_SYMBOL_GPL(bq2589x_pumpx_decrease_volt);

int bq2589x_pumpx_decrease_volt_done(struct bq2589x *bq)
{
    u8 val;
    int ret;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_09);
    if(ret) return ret;

    if(val & BQ2589X_PUMPX_DOWN_MASK) 
        return 1;   // not finished
    else
        return 0;   // pumpx down finished
   
}
EXPORT_SYMBOL_GPL(bq2589x_pumpx_decrease_volt_done);

static int bq2589x_force_ico(struct bq2589x* bq)
{
    u8 val;
    int ret;

    val = BQ2589X_FORCE_ICO << BQ2589X_FORCE_ICO_SHIFT;

    ret = bq2589x_update_bits(bq, BQ2589X_REG_09, BQ2589X_FORCE_ICO_MASK, val);
    
    return ret;
}
EXPORT_SYMBOL_GPL(bq2589x_force_ico);

static int bq2589x_check_force_ico_done(struct bq2589x* bq)
{
    u8 val;
    int ret;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_14);
    if(ret) return ret;

    if(val & BQ2589X_ICO_OPTIMIZED_MASK) 
        return 1;  //finished
    else
        return 0;   // in progress
}
EXPORT_SYMBOL_GPL(bq2589x_check_force_ico_done);

static int bq2589x_read_idpm_limit(struct bq2589x* bq)
{
    uint8_t val;
    int curr;
    int ret;
	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_13);
    if(ret < 0){
        dev_err(bq->dev,"read vbus voltage failed :%d\n",ret);
        return ret;
    }
    else{
        curr = BQ2589X_IDPM_LIM_BASE + ((val & BQ2589X_IDPM_LIM_MASK) >> BQ2589X_IDPM_LIM_SHIFT) * BQ2589X_IDPM_LIM_LSB ;
        return curr;
    }
}
EXPORT_SYMBOL_GPL(bq2589x_read_idpm_limit);

static bool bq2589x_is_charge_done(struct bq2589x* bq)
{
    int ret;
    u8 val;

	ret = bq2589x_read_byte(bq, &val, BQ2589X_REG_0B);
    if(ret < 0){
        dev_err(bq->dev,"%s:read REG0B failed :%d\n",__func__,ret);
        return false;
    }
    val &= BQ2589X_CHRG_STAT_MASK;
    val >>= BQ2589X_CHRG_STAT_SHIFT;

    return(val == BQ2589X_CHRG_STAT_CHGDONE);
}
EXPORT_SYMBOL_GPL(bq2589x_is_charge_done);



static int bq2589x_init_device(struct bq2589x *bq)
{
	
    int ret;

    //common initialization

    bq2589x_disable_watchdog_timer(bq);
    

	ret = bq2589x_set_term_current(bq,256);
	if(ret < 0){
		dev_err(bq->dev,"%s:Failed to set termination current:%d\n",__func__,ret);
		return ret;
	}
	ret = bq2589x_set_chargevoltage(bq,4208);
	if(ret < 0){
		dev_err(bq->dev,"%s:Failed to set charge voltage:%d\n",__func__,ret);
		return ret;
	}

    ret = bq2589x_set_vindpm_offset(bq,600);
	if(ret < 0){
		dev_err(bq->dev,"%s:Failed to set vindpm offset:%d\n",__func__,ret);
		return ret;
	}

	ret = bq2589x_enable_charger(bq);		
	if(ret < 0){
		dev_err(bq->dev,"%s:Failed to enable charger:%d\n",__func__,ret);
		return ret;
	}
 
    if(bq == g_bq1){//charger 1 specific initialization
        bq2589x_adc_start(bq,false);

        ret = bq2589x_pumpx_enable(bq,1);
        if(ret){
            dev_err(bq->dev,"%s:Failed to enable pumpx:%d\n",__func__,ret);
            return ret;
        }

		ret = bq2589x_set_chargecurrent(bq,2250);  
		if(ret < 0){
			dev_err(bq->dev,"%s:Failed to set charger1 charge current:%d\n",__func__,ret);
			return ret;
		}
        bq2589x_set_watchdog_timer(bq,160);
	
    }
    else if(bq == g_bq2){//charger2 specific initialization
	    ret = bq2589x_set_chargecurrent(bq,2250);
		if(ret < 0){
			dev_err(bq->dev,"%s:Failed to set charger2 charge current:%d\n",__func__,ret);
			return ret;
		}
        ret = bq2589x_enter_hiz_mode(bq);//disabled by default
		if(ret < 0){
			dev_err(bq->dev,"%s:Failed to enter hiz charger 2:%d\n",__func__,ret);
			return ret;
		}
      
    }

    return ret;
}


static int bq2589x_charge_status(struct bq2589x * bq)
{
    u8 val = 0;

    bq2589x_read_byte(bq,&val, BQ2589X_REG_0B);
    val &= BQ2589X_CHRG_STAT_MASK;
    val >>= BQ2589X_CHRG_STAT_SHIFT;
    switch(val){
        case BQ2589X_CHRG_STAT_FASTCHG:
            return POWER_SUPPLY_CHARGE_TYPE_FAST;
        case BQ2589X_CHRG_STAT_PRECHG:
            return POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
        case BQ2589X_CHRG_STAT_CHGDONE:
        case BQ2589X_CHRG_STAT_IDLE:
            return POWER_SUPPLY_CHARGE_TYPE_NONE;
        default:
            return POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;
    }
}

static enum power_supply_property bq2589x_charger_props[] = {
	POWER_SUPPLY_PROP_CHARGE_TYPE, /* Charger status output */
	POWER_SUPPLY_PROP_ONLINE, /* External power source */
};


static int bq2589x_usb_get_property(struct power_supply *psy,
                enum power_supply_property psp,
                union power_supply_propval *val)
{

    struct bq2589x *bq = container_of(psy, struct bq2589x, usb);
    u8 type = bq2589x_get_vbus_type(bq);
    
    switch(psp) {
    case POWER_SUPPLY_PROP_ONLINE:
        if(type == BQ2589X_VBUS_USB_SDP || type == BQ2589X_VBUS_USB_DCP)
            val->intval = 1;
        else
            val->intval = 0;
        break;
    case POWER_SUPPLY_PROP_CHARGE_TYPE:
        val->intval = bq2589x_charge_status(bq);
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static int bq2589x_wall_get_property(struct power_supply *psy,
                enum power_supply_property psp,
                union power_supply_propval *val)
{

    struct bq2589x *bq = container_of(psy, struct bq2589x, wall);
    u8 type = bq2589x_get_vbus_type(bq);

    switch(psp) {
    case POWER_SUPPLY_PROP_ONLINE:
        if(type == BQ2589X_VBUS_MAXC || type == BQ2589X_VBUS_UNKNOWN || type == BQ2589X_VBUS_NONSTAND)
            val->intval = 1;
        else
            val->intval = 0;
        break;
    case POWER_SUPPLY_PROP_CHARGE_TYPE:
        val->intval = bq2589x_charge_status(bq);
        break;
    default:
        return -EINVAL;
    }

    return 0;
}



static int bq2589x_psy_register(struct bq2589x *bq)
{
    int ret;

    bq->usb.name = "bq2589x-usb";
    bq->usb.type = POWER_SUPPLY_TYPE_USB;
    bq->usb.properties = bq2589x_charger_props;
    bq->usb.num_properties = ARRAY_SIZE(bq2589x_charger_props);
    bq->usb.get_property = bq2589x_usb_get_property;
    bq->usb.external_power_changed = NULL;

    ret = power_supply_register(bq->dev, &bq->usb);
    if(ret < 0){
        dev_err(bq->dev,"%s:failed to register usb psy:%d\n",__func__,ret);
        return ret;
    }


    bq->wall.name = "bq2589x-Wall";
    bq->wall.type = POWER_SUPPLY_TYPE_MAINS;
    bq->wall.properties = bq2589x_charger_props;
    bq->wall.num_properties = ARRAY_SIZE(bq2589x_charger_props);
    bq->wall.get_property = bq2589x_wall_get_property;
    bq->wall.external_power_changed = NULL;

    ret = power_supply_register(bq->dev, &bq->wall);
    if(ret < 0){
        dev_err(bq->dev,"%s:failed to register wall psy:%d\n",__func__,ret);
        goto fail_1;
    }
    
    return 0;
    
fail_1:
    power_supply_unregister(&bq->usb);
    
    return ret;
}

static void bq2589x_psy_unregister(struct bq2589x *bq)
{
    power_supply_unregister(&bq->usb);
    power_supply_unregister(&bq->wall);
}

static ssize_t bq2589x_show_registers(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	u8 addr;
	u8 val;
	u8 tmpbuf[300];
	int len;
	int idx = 0;
	int ret ;
	
	idx = sprintf(buf,"%s:\n","Charger 1");
	for (addr = 0x0; addr <= 0x14; addr++) {
		ret = bq2589x_read_byte(g_bq1, &val, addr);
		if(ret == 0){
			len = sprintf(tmpbuf,"Reg[0x%.2x] = 0x%.2x\n",addr,val);
			memcpy(&buf[idx],tmpbuf,len);
			idx += len;
		}
	}

	idx += sprintf(&buf[idx],"%s:\n","Charger 2");
	for (addr = 0x0; addr <= 0x14; addr++) {
		ret = bq2589x_read_byte(g_bq2, &val, addr);
		if(ret == 0){
			len = sprintf(tmpbuf,"Reg[0x%.2x] = 0x%.2x\n",addr,val);
			memcpy(&buf[idx],tmpbuf,len);
			idx += len;
		}
	}

	return idx;
}

static DEVICE_ATTR(registers, S_IRUGO, bq2589x_show_registers, NULL);

static struct attribute *bq2589x_attributes[] = {
	&dev_attr_registers.attr,
	NULL,
};

static const struct attribute_group bq2589x_attr_group = {
	.attrs = bq2589x_attributes,
};


static int bq2589x_parse_dt(struct device *dev, struct bq2589x * bq)
{
    int ret;
    struct device_node *np = dev->of_node;

    ret = of_property_read_u32(np,"ti,bq2589x,vbus-volt-high-level",&bq->vbus_volt_high_level);
    if(ret) return ret;

    ret = of_property_read_u32(np,"ti,bq2589x,vbus-volt-low-level",&bq->vbus_volt_low_level);
    if(ret) return ret;

    ret = of_property_read_u32(np,"ti,bq2589x,vbat-min-volt-to-tuneup",&bq->vbat_min_volt_to_tuneup);
    if(ret) return ret;
 
    return 0;   
}

static int bq2589x_detect_device(struct bq2589x* bq)
{
    int ret;
    u8 data;

    ret = bq2589x_read_byte(bq,&data,BQ2589X_REG_14);
    if(ret == 0){
        bq->part_no = (data & BQ2589X_PN_MASK) >> BQ2589X_PN_SHIFT;
        bq->revision = (data & BQ2589X_DEV_REV_MASK) >> BQ2589X_DEV_REV_SHIFT;
    }

    return ret;
}

static void bq2589x_adjust_absolute_vindpm(struct bq2589x *bq)
{
	u16 vbus_volt;
	u16 vindpm_volt;
	int ret;
	
	vbus_volt = bq2589x_adc_read_vbus_volt(bq);
	if(vbus_volt < 6000)
		vindpm_volt = vbus_volt - 600;
	else	
		vindpm_volt = vbus_volt - 1200;
	ret = bq2589x_set_input_volt_limit(bq,vindpm_volt);
	if(ret < 0)
		dev_err(bq->dev,"%s:Set absolute vindpm threshold %d Failed:%d\n",__func__,vindpm_volt,ret);
	else
		dev_info(bq->dev,"%s:Set absolute vindpm threshold %d successfully\n",__func__,vindpm_volt);
	
}

static void bq2589x_adapter_in_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, adapter_in_work);
	int ret;
	
//	bq2589x_set_chargevoltage(bq,bq->charge_volt);
//	bq2589x_set_chargecurrent(bq,bq->charge_current);
	
	ret = bq2589x_enter_hiz_mode(g_bq2);
	if(ret < 0)
		dev_err(bq->dev,"%s: Charger 2 enter hiz mode failed\n",__func__);
	else{
		dev_info(bq->dev,"%s:Charger 2 enter Hiz mode successfully\n",__func__);
		g_bq2->enabled = false;
	}
	
	if(bq->vbus_type == BQ2589X_VBUS_MAXC){
		dev_info(bq->dev,"%s:HVDCP or Maxcharge adapter plugged in\n",__func__);
		schedule_delayed_work(&bq->ico_work,0);
	}
	else if(bq->vbus_type == BQ2589X_VBUS_USB_DCP){// DCP, let's check if it is PE adapter
		dev_info(bq->dev,"%s:usb dcp adapter plugged in\n",__func__);
		schedule_delayed_work(&bq->check_to_tuneup_work,0);
	}
	else{
		dev_info(bq->dev,"%s:other adapter plugged in,vbus_type is %d\n",__func__,bq->vbus_type);
		schedule_delayed_work(&bq->ico_work,0);
	}

	if(bq->use_absolute_vindpm){
		bq2589x_adjust_absolute_vindpm(bq);
	}
	
	schedule_delayed_work(&bq->monitor_work,0);
}

static void bq2589x_adapter_out_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, adapter_out_work);
	
    bq2589x_set_input_volt_limit(g_bq1,4400); 
    bq2589x_set_input_volt_limit(g_bq2,4400);

	cancel_delayed_work_sync(&bq->monitor_work);
	
}

static void bq2589x_ico_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, ico_work.work);
	int ret;
	u8 status;
	int curr;
	static bool ico_issued = false;
	
	if(!ico_issued){
		/* Read VINDPM/IINDPM status */
		ret = bq2589x_read_byte(bq, &status, BQ2589X_REG_13);
		if(ret < 0){
			schedule_delayed_work(&bq->ico_work,2*HZ);
			return;
		}
		if(status & (BQ2589X_VDPM_STAT_MASK | BQ2589X_IDPM_STAT_MASK)) //VINDPM or IINDPM
			return;
		ret = bq2589x_force_ico(bq);
		if(ret < 0){
			schedule_delayed_work(&bq->ico_work,1*HZ); // retry 1 second later
			dev_info(bq->dev,"%s:ICO command issued failed:%d\n",__func__,ret);
		}
		else{
			ico_issued = true;
			schedule_delayed_work(&bq->ico_work,3*HZ);
			dev_info(bq->dev,"%s:ICO command issued successfully\n",__func__);
		}
	}
	else{
		ico_issued = false;
		ret = bq2589x_check_force_ico_done(bq);
		if(ret){//ico done
			dev_info(bq->dev,"%s:ICO done!\n",__func__);
			ret = bq2589x_read_byte(bq,&status,BQ2589X_REG_13);
			if(ret == 0){
				curr = (status & BQ2589X_IDPM_LIM_MASK) * BQ2589X_IDPM_LIM_LSB + BQ2589X_IDPM_LIM_BASE;
				curr /= 2;
				ret = bq2589x_set_input_current_limit(g_bq2,curr);
				if(ret < 0)
					dev_info(bq->dev,"%s:Set IINDPM for charger 2:%d,failed with code:%d\n",__func__,curr,ret);
				else
					dev_info(bq->dev,"%s:Set IINDPM for charger 2:%d successfully\n",__func__,curr);

			}
		}
		schedule_delayed_work(&bq->charger2_enable_work,0);
	}
}

// check if need to eanble charger 2
static void bq2589x_charger2_enable_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, charger2_enable_work.work);
	int ret;
	
	//TODO:read rsoc, or any conditons to check if needed to enable charger 2
    if(bq->rsoc < 95){	
    	ret = bq2589x_exit_hiz_mode(g_bq2);
    	if(ret)
	    	dev_err(bq->dev,"%s: charger 2 exit hiz mode failed:%d\n",__func__,ret);
    	else{
	    	dev_info(bq->dev,"%s: charger 2 exit hiz mode successfully\n",__func__);
    		g_bq2->enabled = true;
    	}
    }
}


static void bq2589x_check_if_tuneup_workfunc(struct work_struct* work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, check_to_tuneup_work.work);
	
//	g_bq1->vbus_volt = bq2589x_adc_read_vbus_volt(g_bq1);
    g_bq1->vbat_volt = bq2589x_adc_read_battery_volt(g_bq1);
	
	if(bq->vbat_volt > g_bq1->vbat_min_volt_to_tuneup && g_bq1->vbat_volt < 4100 /*g_bq1->rsoc < 95*/){    
        dev_info(bq->dev,"%s:trying to tune up vbus voltage\n",__func__);
		voltcontrol.TuneTargetVolt = g_bq1->vbus_volt_high_level;
		voltcontrol.toTuneUpVolt = true;
		voltcontrol.toTuneDownVolt = false;
		voltcontrol.TuneVoltDone = false;
		voltcontrol.TuneCounter = 0;
		voltcontrol.TuneFail = false;	
		schedule_delayed_work(&bq->volt_tune_work,0);
	}
    else if(g_bq1->vbat_volt > 4100/*g_bq1->rsoc >95*/)
        schedule_delayed_work(&bq->ico_work,0);
	else
		schedule_delayed_work(&bq->check_to_tuneup_work, 2*HZ);
}

static void bq2589x_tune_volt_workfunc(struct work_struct * work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, volt_tune_work.work);
	int ret;
	static bool pumpx_cmd_issued = false;
	
	g_bq1->vbus_volt = bq2589x_adc_read_vbus_volt(g_bq1);

    dev_info(bq->dev,"%s:vbus voltage:%d, Tune Target Volt:%d\n",__func__,g_bq1->vbus_volt,voltcontrol.TuneTargetVolt);

	if( (voltcontrol.toTuneUpVolt && g_bq1->vbus_volt > voltcontrol.TuneTargetVolt)|| 
		(voltcontrol.toTuneDownVolt && g_bq1->vbus_volt < voltcontrol.TuneTargetVolt)){
		dev_info(bq->dev,"%s:voltage tune successfully\n",__func__);
		voltcontrol.TuneVoltDone = true;
		bq2589x_adjust_absolute_vindpm(bq);
		if(voltcontrol.toTuneUpVolt)
			schedule_delayed_work(&bq->ico_work,0);
		return;
	}
	
	if(voltcontrol.TuneCounter > 10){
		dev_info(bq->dev,"%s:voltage tune failed,reach max retry count\n",__func__);
		voltcontrol.TuneFail = true;
		bq2589x_adjust_absolute_vindpm(bq);

		if(voltcontrol.toTuneUpVolt)
			schedule_delayed_work(&bq->ico_work,0);
		return;
	}

	if(!pumpx_cmd_issued){
		if(voltcontrol.toTuneUpVolt)
			ret = bq2589x_pumpx_increase_volt(bq);
		else if(voltcontrol.toTuneDownVolt)
			ret =  bq2589x_pumpx_decrease_volt(bq);
		if(ret)
			schedule_delayed_work(&bq->volt_tune_work,HZ);//retry
		else{
			dev_info(bq->dev,"%s:pumpx command issued.\n",__func__);
			pumpx_cmd_issued = true;
			voltcontrol.TuneCounter++;
			schedule_delayed_work(&bq->volt_tune_work,3*HZ);
		}
	}
	else{
		if(voltcontrol.toTuneUpVolt)
			ret = bq2589x_pumpx_increase_volt_done(bq);
		else if(voltcontrol.toTuneDownVolt)
			ret = bq2589x_pumpx_decrease_volt_done(bq);
		if(ret == 0){//finished for one step 
			dev_info(bq->dev,"%s:pumpx command finishedd!\n",__func__);
			bq2589x_adjust_absolute_vindpm(bq);
			pumpx_cmd_issued = 0;
		}			
		schedule_delayed_work(&bq->volt_tune_work,HZ);
	}
}


static void bq2589x_monitor_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, monitor_work.work);
	int ret;

	dev_info(bq->dev,"%s\n",__func__);
	//kick watchdog
	bq2589x_reset_watchdog_timer(bq);
    //TODO:read rsoc

    g_bq1->vbus_volt = bq2589x_adc_read_vbus_volt(g_bq1);
    g_bq1->vbat_volt = bq2589x_adc_read_battery_volt(g_bq1);	
	
	
	if(g_bq1->vbus_type == BQ2589X_VBUS_USB_DCP && g_bq1->vbus_volt > g_bq1->vbus_volt_high_level
		&& g_bq1->rsoc > 95 /*g_bq1->vbat_volt > 4180*/ && !voltcontrol.toTuneDownVolt){
		ret = bq2589x_enter_hiz_mode(g_bq2);
		if(ret)
			dev_err(g_bq1->dev,"%s: charger 2 enter hiz mode failed:%d\n",__func__,ret);
		else{
			dev_info(g_bq1->dev,"%s: charger 2 enter hiz mode successfully\n",__func__);
			g_bq2->enabled = false;
		}		
		voltcontrol.toTuneDownVolt = true;
		voltcontrol.toTuneUpVolt = false;
		voltcontrol.TuneTargetVolt = g_bq1->vbus_volt_low_level;
		voltcontrol.TuneVoltDone = false;
		voltcontrol.TuneCounter = 0;
		voltcontrol.TuneFail = false;
		schedule_delayed_work(&bq->volt_tune_work,0);
	}
	
	// read temperature,or any other check if need to decrease charge current
	
	schedule_delayed_work(&bq->monitor_work,10*HZ);
}


static void bq2589x_charger1_irq_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, irq_work);
	u8 status = 0;
	u8 fault = 0;
	int ret;


	/* Read STATUS and FAULT registers */
	ret = bq2589x_read_byte(bq, &status, BQ2589X_REG_0B);
	if (ret) 
		return;

	ret = bq2589x_read_byte(bq, &fault, BQ2589X_REG_0C);
	if (ret) 
		return;
	
    if(((status & BQ2589X_VBUS_STAT_MASK) == 0) && (bq->status & BQ2589X_STATUS_PLUGIN)){// plug out
        dev_info(bq->dev, "%s:adapter removed\n",__func__);
        bq->status &= ~BQ2589X_STATUS_PLUGIN;
		schedule_work(&bq->adapter_out_work);
 
    }
    else if((status & BQ2589X_VBUS_STAT_MASK) && !(bq->status & BQ2589X_STATUS_PLUGIN)){
        dev_info(bq->dev, "%s:adapter plugged in\n",__func__);
        bq->status |= BQ2589X_STATUS_PLUGIN;
		bq->vbus_type = (status & BQ2589X_VBUS_STAT_MASK) >> BQ2589X_VBUS_STAT_SHIFT;
		schedule_work(&bq->adapter_in_work);
    }


    if((status & BQ2589X_PG_STAT_MASK) && !(bq->status & BQ2589X_STATUS_PG)){
        bq->status |= BQ2589X_STATUS_PG;
    }
    else if(!(status & BQ2589X_PG_STAT_MASK) && (bq->status & BQ2589X_STATUS_PG)){
        bq->status &=~BQ2589X_STATUS_PG;
    }

    if(fault && !(bq->status & BQ2589X_STATUS_FAULT)){
        bq->status |= BQ2589X_STATUS_FAULT;
    }
    else if(!fault &&(bq->status &BQ2589X_STATUS_FAULT)){
        bq->status &=~BQ2589X_STATUS_FAULT;
    }

    bq->interrupt = true;
}


static irqreturn_t bq2589x_charger1_interrupt(int irq, void *data)
{
	struct bq2589x *bq = data;

	schedule_work(&bq->irq_work);
	return IRQ_HANDLED;
}


#define GPIO_IRQ    80
static int bq2589x_charger1_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct bq2589x *bq;
    int irqn;

	int ret;

    bq = kzalloc(sizeof(struct bq2589x),GFP_KERNEL);
    if(!bq){
        dev_err(&client->dev,"%s: out of memory\n",__func__);
        return -ENOMEM;
    }

    bq->dev = &client->dev;
    bq->client = client;
    i2c_set_clientdata(client,bq);
    
    ret = bq2589x_detect_device(bq);
    if(ret == 0){
        if(bq->part_no == BQ25890){
			bq->status |= BQ2589X_STATUS_EXIST;
            dev_info(bq->dev,"%s: charger device bq25890 detected, revision:%d\n",__func__,bq->revision); 
        }
        else{
            dev_err(bq->dev, "%s: unexpected charger device detected\n",__func__);
            kfree(bq);
			return -ENODEV;
        }
    }
    else{
        dev_info(bq->dev,"%s: no bq25890 charger device found:%d\n",__func__,ret); 
        kfree(bq);
        return -ENODEV;
    }

    g_bq1 = bq;

    g_bq1->vbus_volt_high_level = 4400; //by default adapter output 5v, if >4.4v,it is ok after tune up
    g_bq1->vbus_volt_low_level = 5500; //by default adapter output 5v, if <5.5v,it is ok after tune down
    g_bq1->vbat_min_volt_to_tuneup = 3000; // by default, tune up adapter output only when bat is >3000

//    g_bq1->charge_voltage = 4208;
//    g_bq1->charge_current = 2048;

	if(client->dev.of_node)
		 bq2589x_parse_dt(&client->dev, g_bq1);

	ret = bq2589x_init_device(g_bq1);
	if (ret) {
		dev_err(bq->dev, "device init failure: %d\n", ret);
		goto err_0;
	}    // platform setup, irq,...

	ret = gpio_request(GPIO_IRQ, "bq2589x irq pin");
	if(ret)
	{
		dev_err(bq->dev,"%s: %d gpio request failed\n", __func__, GPIO_IRQ);
		goto err_0;
	}
	gpio_direction_input(GPIO_IRQ);

    irqn = gpio_to_irq(GPIO_IRQ);
    if(irqn < 0){
        dev_err(bq->dev,"%s:%d gpio_to_irq failed\n",__func__,irqn);
        ret = irqn;
        goto err_1;
    }
    client->irq = irqn;


    ret = bq2589x_psy_register(bq);
    if(ret) goto err_0;

	INIT_WORK(&bq->irq_work, bq2589x_charger1_irq_workfunc);
	INIT_WORK(&bq->adapter_in_work, bq2589x_adapter_in_workfunc);	
	INIT_WORK(&bq->adapter_out_work, bq2589x_adapter_out_workfunc);
	INIT_DELAYED_WORK(&bq->monitor_work, bq2589x_monitor_workfunc);
	INIT_DELAYED_WORK(&bq->ico_work, bq2589x_ico_workfunc);
	INIT_DELAYED_WORK(&bq->volt_tune_work, bq2589x_tune_volt_workfunc);
	INIT_DELAYED_WORK(&bq->check_to_tuneup_work, bq2589x_check_if_tuneup_workfunc);
	INIT_DELAYED_WORK(&bq->charger2_enable_work, bq2589x_charger2_enable_workfunc );

	
	ret = sysfs_create_group(&bq->dev->kobj, &bq2589x_attr_group);
	if (ret) {
		dev_err(bq->dev, "failed to register sysfs. err: %d\n", ret);
		goto err_irq;
	}
    // request irq
	ret = request_irq(client->irq, bq2589x_charger1_interrupt,IRQF_TRIGGER_FALLING | IRQF_ONESHOT,"bq2589x_charger1_irq", bq);
	if (ret) {
		dev_err(bq->dev, "%s:Request IRQ %d failed: %d\n", __func__,client->irq, ret);
		goto err_irq;
	}
    else{
        dev_info(bq->dev,"%s:irq = %d\n",__func__,client->irq);
    }


    voltcontrol.TuneVoltwithPEplus = true; 
    schedule_work(&bq->irq_work);//in case of adapter has been in when power off
	return 0;

err_irq:
	cancel_work_sync(&bq->irq_work);
err_1:
    gpio_free(GPIO_IRQ);
err_0:
	kfree(bq);
    g_bq1 = NULL;
	return ret;
}

static void bq2589x_charger1_shutdown(struct i2c_client *client)
{
	struct bq2589x *bq = i2c_get_clientdata(client);

    dev_info(bq->dev,"%s: shutdown\n",__func__);
	   
    bq2589x_psy_unregister(bq);

	sysfs_remove_group(&bq->dev->kobj, &bq2589x_attr_group);
	cancel_work_sync(&bq->irq_work);
	cancel_work_sync(&bq->adapter_in_work);
	cancel_work_sync(&bq->adapter_out_work);
	cancel_delayed_work_sync(&bq->monitor_work);
	cancel_delayed_work_sync(&bq->ico_work);
	cancel_delayed_work_sync(&bq->check_to_tuneup_work);
	cancel_delayed_work_sync(&bq->charger2_enable_work);
	cancel_delayed_work_sync(&bq->volt_tune_work);
	
    free_irq(bq->client->irq,NULL);
    gpio_free(GPIO_IRQ);

	kfree(bq);
    g_bq1 = NULL;
}

/* interface for other module end */
static void bq2589x_charger2_irq_workfunc(struct work_struct *work)
{
	struct bq2589x *bq = container_of(work, struct bq2589x, irq_work);
	u8 status = 0;
	u8 fault = 0;
	int ret;


	/* Read STATUS and FAULT registers */
	ret = bq2589x_read_byte(bq, &status, BQ2589X_REG_0B);
	if (ret) 
		return;

	ret = bq2589x_read_byte(bq, &fault, BQ2589X_REG_0C);
	if (ret) 
		return;

    if(((status & BQ2589X_VBUS_STAT_MASK) == 0) && (bq->status & BQ2589X_STATUS_PLUGIN)){// plug out
        bq->status &= ~BQ2589X_STATUS_PLUGIN;

    }
    else if((status & BQ2589X_VBUS_STAT_MASK) && !(bq->status & BQ2589X_STATUS_PLUGIN)){
        bq->status |= BQ2589X_STATUS_PLUGIN;
    }

    if((status & BQ2589X_PG_STAT_MASK) && !(bq->status & BQ2589X_STATUS_PG)){
        bq->status |= BQ2589X_STATUS_PG;
    }
    else if(!(status & BQ2589X_PG_STAT_MASK) && (bq->status & BQ2589X_STATUS_PG)){
        bq->status &=~BQ2589X_STATUS_PG;
    }

    if(fault && !(bq->status & BQ2589X_STATUS_FAULT)){
        bq->status |= BQ2589X_STATUS_FAULT;
    }
    else if(!fault &&(bq->status &BQ2589X_STATUS_FAULT)){
        bq->status &=~BQ2589X_STATUS_FAULT;
    }
	
    bq->interrupt = true;
	
}
#if 0
static irqreturn_t bq2589x_charger2_interrupt(int irq, void *data)
{
	struct bq2589x *bq = data;

	schedule_work(&bq->irq_work);
	return IRQ_HANDLED;
}

#endif

static int bq2589x_charger2_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct bq2589x *bq;

	int ret;

    bq = kzalloc(sizeof(struct bq2589x),GFP_KERNEL);
    if(!bq){
        dev_err(&client->dev,"%s: out of memory\n",__func__);
        return -ENOMEM;
    }

    bq->dev = &client->dev;
    bq->client = client;
    i2c_set_clientdata(client,bq);
    
    ret = bq2589x_detect_device(bq);
    if(ret == 0){
        if(bq->part_no == BQ25892){
			bq->status |= BQ2589X_STATUS_EXIST;
            dev_info(bq->dev,"%s: charger device bq25892 detected, revision:%d\n",__func__,bq->revision); 
        }
        else{
            dev_err(bq->dev, "%s: unexpected charger device detected\n",__func__);
            kfree(bq);
			return -ENODEV;
        }
    }
    else{
        dev_info(bq->dev,"%s: no charger device bq25892 found:%d\n",__func__,ret); 
        kfree(bq);
        return -ENODEV;
    }

    g_bq2 = bq;

    // initialize bq2589x, disable charger 2 by default 
	ret = bq2589x_init_device(g_bq2);
	if(ret){
		dev_err(bq->dev,"%s:Failed to initialize bq2589x charger\n",__func__);
	}
    else{
		dev_info(bq->dev,"%s: Initialize bq2589x charger successfully!\n",__func__);
	}
    // platform setup, irq,...
	INIT_WORK(&bq->irq_work, bq2589x_charger2_irq_workfunc);
    
    return 0;
}

static void bq2589x_charger2_shutdown(struct i2c_client *client)
{
	struct bq2589x *bq = i2c_get_clientdata(client);

	dev_info(bq->dev,"%s: shutdown\n",__func__);
	cancel_work_sync(&bq->irq_work);
	kfree(bq);
    g_bq2 = NULL;
}

static struct of_device_id bq2589x_charger1_match_table[] = {
    {.compatible = "ti,bq2589x-1",},
    {},
};


static const struct i2c_device_id bq2589x_charger1_id[] = {
	{ "bq2589x-1", BQ25890 },
	{},
};

MODULE_DEVICE_TABLE(i2c, bq2589x_charger1_id);

static struct i2c_driver bq2589x_charger1_driver = {
	.driver		= {
		.name	= "bq2589x-1",
        .of_match_table = bq2589x_charger1_match_table,
	},
	.id_table	= bq2589x_charger1_id,

	.probe		= bq2589x_charger1_probe,
    .shutdown   = bq2589x_charger1_shutdown,
};


static struct of_device_id bq2589x_charger2_match_table[] = {
    {.compatible = "ti,bq2589x-2",},
    {},
};

static const struct i2c_device_id bq2589x_charger2_id[] = {
	{ "bq2589x-2", BQ25892 },
	{},
};

MODULE_DEVICE_TABLE(i2c, bq2589x_charger2_id);


static struct i2c_driver bq2589x_charger2_driver = {
	.driver		= {
		.name	= "bq2589x-2",
        .of_match_table = bq2589x_charger2_match_table,
	},

	.id_table	= bq2589x_charger2_id,

	.probe		= bq2589x_charger2_probe,
    .shutdown   = bq2589x_charger2_shutdown,
};

static struct i2c_board_info __initdata i2c_bq2589x_charger1[] =
{
    { 
        I2C_BOARD_INFO("bq2589x-1",0x6A),
    },
};


static struct i2c_board_info __initdata i2c_bq2589x_charger2[] =
{
    {
        I2C_BOARD_INFO("bq2589x-2",0x6B),
    },
};


static int __init bq2589x_charger_init(void)
{

    i2c_register_board_info(0,i2c_bq2589x_charger1,ARRAY_SIZE(i2c_bq2589x_charger1));
    i2c_register_board_info(0,i2c_bq2589x_charger2,ARRAY_SIZE(i2c_bq2589x_charger2));

    if(i2c_add_driver(&bq2589x_charger2_driver)){
        printk("%s, failed to register bq2589x_charger2_driver.\n",__func__);
    }
    else{
        printk("%s, bq2589x_charger2_driver register successfully!\n",__func__);
    }


    if(i2c_add_driver(&bq2589x_charger1_driver)){
        printk("%s, failed to register bq2589x_charger1_driver.\n",__func__);
    }
    else{
        printk("%s, bq2589x_charger1_driver register successfully!\n",__func__);
    }

    return 0; 
}

static void __exit bq2589x_charger_exit(void)
{
    printk("%s:\n",__func__);
    i2c_del_driver(&bq2589x_charger1_driver);
    i2c_del_driver(&bq2589x_charger2_driver);
}

module_init(bq2589x_charger_init);
module_exit(bq2589x_charger_exit);

MODULE_DESCRIPTION("TI BQ2589x Dual Charger Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Texas Instruments");
