/* Copyright (c) 2009-2015, Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <linux/of.h>
#include <linux/dma-mapping.h>
#include <linux/clk/msm-clk.h>
#include <linux/pinctrl/consumer.h>
#include <linux/irqchip/msm-mpm-irq.h>
#include <soc/qcom/scm.h>

#include <linux/usb.h>
#include <linux/usb/otg.h>
#include <linux/usb/ulpi.h>
#include <linux/usb/gadget.h>
#include <linux/usb/hcd.h>
#include <linux/usb/quirks.h>
#include <linux/usb/msm_hsusb.h>
#include <linux/usb/msm_hsusb_hw.h>
#include <linux/usb/msm_ext_chg.h>
#include <linux/regulator/consumer.h>
#include <linux/mfd/pm8xxx/pm8921-charger.h>
#include <linux/mfd/pm8xxx/misc.h>
#include <linux/mhl_8334.h>
#include <linux/qpnp/qpnp-adc.h>

#include <linux/msm-bus.h>
#include <linux/usb/htc_info.h>
#include <linux/usb/cable_detect.h>

#define MSM_USB_BASE	(motg->regs)
#define MSM_USB_PHY_CSR_BASE (motg->phy_csr_regs)

#define DRIVER_NAME	"msm_otg"

#define ID_TIMER_FREQ		(jiffies + msecs_to_jiffies(500))
#define CHG_RECHECK_DELAY	(jiffies + msecs_to_jiffies(2000))
#define ULPI_IO_TIMEOUT_USEC	(10 * 1000)
#define USB_PHY_3P3_VOL_MIN	3050000 
#define USB_PHY_3P3_VOL_MAX	3300000 
#define USB_PHY_3P3_HPM_LOAD	50000	
#define USB_PHY_3P3_LPM_LOAD	4000	

#define USB_PHY_1P8_VOL_MIN	1800000 
#define USB_PHY_1P8_VOL_MAX	1800000 
#define USB_PHY_1P8_HPM_LOAD	50000	
#define USB_PHY_1P8_LPM_LOAD	4000	

#define USB_PHY_VDD_DIG_VOL_NONE	0 
#define USB_PHY_VDD_DIG_VOL_MIN	1045000 
#define USB_PHY_VDD_DIG_VOL_MAX	1320000 

#define USB_SUSPEND_DELAY_TIME	(500 * HZ/1000) 

#define USB_DEFAULT_SYSTEM_CLOCK 80000000	

enum msm_otg_phy_reg_mode {
	USB_PHY_REG_OFF,
	USB_PHY_REG_ON,
	USB_PHY_REG_LPM_ON,
	USB_PHY_REG_LPM_OFF,
};

int msm_otg_usb_disable = 0;
static int msm_id_backup = 1;
static int htc_vbus_active = 0; 
static char *override_phy_init;
module_param(override_phy_init, charp, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(override_phy_init,
	"Override HSUSB PHY Init Settings");

unsigned int lpm_disconnect_thresh = 1000;
module_param(lpm_disconnect_thresh , uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(lpm_disconnect_thresh,
	"Delay before entering LPM on USB disconnect");

static bool floated_charger_enable;
module_param(floated_charger_enable , bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(floated_charger_enable,
	"Whether to enable floated charger");

static unsigned int enable_dbg_log = 1;
module_param(enable_dbg_log, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(enable_dbg_log, "Debug buffer events");

static int hvdcp_max_current = IDEV_HVDCP_CHG_MAX;
module_param(hvdcp_max_current, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(hvdcp_max_current, "max current drawn for HVDCP charger");

static int dcp_max_current = IDEV_CHG_MAX;
module_param(dcp_max_current, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(dcp_max_current, "max current drawn for DCP charger");

static DECLARE_COMPLETION(pmic_vbus_init);
static struct msm_otg *the_msm_otg;
static bool debug_aca_enabled;
static bool debug_bus_voting_enabled;
static bool mhl_det_in_progress;

static struct regulator *hsusb_3p3;
static struct regulator *hsusb_1p8;
static struct regulator *hsusb_vdd;
static struct regulator *vbus_otg;
static struct regulator *mhl_usb_hs_switch;
static struct power_supply *psy;

static bool aca_id_turned_on;
static bool legacy_power_supply;
static inline bool aca_enabled(void)
{
#ifdef CONFIG_USB_MSM_ACA
	return true;
#else
	return debug_aca_enabled;
#endif
}

static int vdd_val[VDD_VAL_MAX];
static u32 bus_freqs[USB_NUM_BUS_CLOCKS];	;
static char bus_clkname[USB_NUM_BUS_CLOCKS][20] = {"bimc_clk", "snoc_clk",
						"pcnoc_clk"};
static bool bus_clk_rate_set;

static void dbg_inc(unsigned *idx)
{
	*idx = (*idx + 1) & (DEBUG_MAX_MSG-1);
}

static void
msm_otg_dbg_log_event(struct usb_phy *phy, char *event, int d1, int d2)
{
	struct msm_otg *motg = container_of(phy, struct msm_otg, phy);
	unsigned long flags;
	unsigned long long t;
	unsigned long nanosec;

	if (!enable_dbg_log)
		return;

	write_lock_irqsave(&motg->dbg_lock, flags);
	t = cpu_clock(smp_processor_id());
	nanosec = do_div(t, 1000000000)/1000;
	scnprintf(motg->buf[motg->dbg_idx], DEBUG_MSG_LEN,
			"[%5lu.%06lu]: %s :%d:%d",
			(unsigned long)t, nanosec, event, d1, d2);

	motg->dbg_idx++;
	motg->dbg_idx = motg->dbg_idx % DEBUG_MAX_MSG;
	write_unlock_irqrestore(&motg->dbg_lock, flags);
}

static int msm_hsusb_ldo_init(struct msm_otg *motg, int init)
{
	int rc = 0;

	if (init) {
		hsusb_3p3 = devm_regulator_get(motg->phy.dev, "HSUSB_3p3");
		if (IS_ERR(hsusb_3p3)) {
			dev_err(motg->phy.dev, "unable to get hsusb 3p3\n");
			return PTR_ERR(hsusb_3p3);
		}

		rc = regulator_set_voltage(hsusb_3p3, USB_PHY_3P3_VOL_MIN,
				USB_PHY_3P3_VOL_MAX);
		if (rc) {
			dev_err(motg->phy.dev, "unable to set voltage level for"
					"hsusb 3p3\n");
			return rc;
		}
		hsusb_1p8 = devm_regulator_get(motg->phy.dev, "HSUSB_1p8");
		if (IS_ERR(hsusb_1p8)) {
			dev_err(motg->phy.dev, "unable to get hsusb 1p8\n");
			rc = PTR_ERR(hsusb_1p8);
			goto put_3p3_lpm;
		}
		rc = regulator_set_voltage(hsusb_1p8, USB_PHY_1P8_VOL_MIN,
				USB_PHY_1P8_VOL_MAX);
		if (rc) {
			dev_err(motg->phy.dev, "unable to set voltage level "
					"for hsusb 1p8\n");
			goto put_1p8;
		}

		return 0;
	}

put_1p8:
	regulator_set_voltage(hsusb_1p8, 0, USB_PHY_1P8_VOL_MAX);
put_3p3_lpm:
	regulator_set_voltage(hsusb_3p3, 0, USB_PHY_3P3_VOL_MAX);
	return rc;
}

static int msm_hsusb_config_vddcx(int high)
{
	struct msm_otg *motg = the_msm_otg;
	int max_vol = vdd_val[VDD_MAX];
	int min_vol;
	int ret;

	min_vol = vdd_val[!!high];
	ret = regulator_set_voltage(hsusb_vdd, min_vol, max_vol);
	if (ret) {
		pr_err("%s: unable to set the voltage for regulator "
			"HSUSB_VDDCX\n", __func__);
		return ret;
	}

	printk(KERN_WARNING "%s: min_vol:%d max_vol:%d\n", __func__, min_vol, max_vol);
	msm_otg_dbg_log_event(&motg->phy, "CONFIG VDDCX", min_vol, max_vol);

	return ret;
}

static int msm_hsusb_ldo_enable(struct msm_otg *motg,
	enum msm_otg_phy_reg_mode mode)
{
	int ret = 0;

	if (IS_ERR(hsusb_1p8)) {
		pr_err("%s: HSUSB_1p8 is not initialized\n", __func__);
		return -ENODEV;
	}

	if (IS_ERR(hsusb_3p3)) {
		pr_err("%s: HSUSB_3p3 is not initialized\n", __func__);
		return -ENODEV;
	}

	switch (mode) {
	case USB_PHY_REG_ON:
		ret = regulator_set_optimum_mode(hsusb_1p8,
				USB_PHY_1P8_HPM_LOAD);
		if (ret < 0) {
			pr_err("%s: Unable to set HPM of the regulator "
				"HSUSB_1p8\n", __func__);
			return ret;
		}

		ret = regulator_enable(hsusb_1p8);
		if (ret) {
			dev_err(motg->phy.dev, "%s: unable to enable the hsusb 1p8\n",
				__func__);
			regulator_set_optimum_mode(hsusb_1p8, 0);
			return ret;
		}

		ret = regulator_set_optimum_mode(hsusb_3p3,
				USB_PHY_3P3_HPM_LOAD);
		if (ret < 0) {
			pr_err("%s: Unable to set HPM of the regulator "
				"HSUSB_3p3\n", __func__);
			regulator_set_optimum_mode(hsusb_1p8, 0);
			regulator_disable(hsusb_1p8);
			return ret;
		}

		ret = regulator_enable(hsusb_3p3);
		if (ret) {
			dev_err(motg->phy.dev, "%s: unable to enable the hsusb 3p3\n",
				__func__);
			regulator_set_optimum_mode(hsusb_3p3, 0);
			regulator_set_optimum_mode(hsusb_1p8, 0);
			regulator_disable(hsusb_1p8);
			return ret;
		}

		break;

	case USB_PHY_REG_OFF:

		ret = regulator_disable(hsusb_1p8);
		if (ret) {
			dev_err(motg->phy.dev, "%s: unable to disable the hsusb 1p8\n",
				 __func__);
			return ret;
		}
		ret = regulator_set_optimum_mode(hsusb_1p8, 0);
		if (ret < 0)
			pr_err("%s: Unable to set LPM of the regulatorHSUSB_1p8\n",
				__func__);

		break;

	case USB_PHY_REG_LPM_ON:
		ret = regulator_set_optimum_mode(hsusb_1p8,
				USB_PHY_1P8_LPM_LOAD);
		if (ret < 0) {
			pr_err("%s: Unable to set LPM of the regulator: HSUSB_1p8\n",
				__func__);
			return ret;
		}

		ret = regulator_set_optimum_mode(hsusb_3p3,
				USB_PHY_3P3_LPM_LOAD);
		if (ret < 0) {
			pr_err("%s: Unable to set LPM of the regulator: HSUSB_3p3\n",
				__func__);
			regulator_set_optimum_mode(hsusb_1p8, USB_PHY_REG_ON);
			return ret;
		}

		break;

	case USB_PHY_REG_LPM_OFF:
		ret = regulator_set_optimum_mode(hsusb_1p8,
				USB_PHY_1P8_HPM_LOAD);
		if (ret < 0) {
			pr_err("%s: Unable to set HPM of the regulator: HSUSB_1p8\n",
				__func__);
			return ret;
		}

		ret = regulator_set_optimum_mode(hsusb_3p3,
				USB_PHY_3P3_HPM_LOAD);
		if (ret < 0) {
			pr_err("%s: Unable to set HPM of the regulator: HSUSB_3p3\n",
				__func__);
			regulator_set_optimum_mode(hsusb_1p8, USB_PHY_REG_ON);
			return ret;
		}

		break;

	default:
		pr_err("%s: Unsupported mode (%d).", __func__, mode);
		return -ENOTSUPP;
	}

	printk(KERN_WARNING "%s: USB reg mode (%d) (OFF/HPM/LPM)\n", __func__, mode);
	msm_otg_dbg_log_event(&motg->phy, "USB REG MODE", mode, ret);
	return ret < 0 ? ret : 0;
}

static void msm_hsusb_mhl_switch_enable(struct msm_otg *motg, bool on)
{
	struct msm_otg_platform_data *pdata = motg->pdata;

	if (!pdata->mhl_enable)
		return;

	if (!mhl_usb_hs_switch) {
		pr_err("%s: mhl_usb_hs_switch is NULL.\n", __func__);
		return;
	}

	if (on) {
		if (regulator_enable(mhl_usb_hs_switch))
			pr_err("unable to enable mhl_usb_hs_switch\n");
	} else {
		regulator_disable(mhl_usb_hs_switch);
	}
}

static int ulpi_read(struct usb_phy *phy, u32 reg)
{
	struct msm_otg *motg = container_of(phy, struct msm_otg, phy);
	int cnt = 0;

	if (motg->pdata->emulation)
		return 0;

	if (motg->pdata->phy_type == QUSB_ULPI_PHY && reg > 0x3F) {
		pr_debug("%s: ULPI vendor-specific reg 0x%02x not supported\n",
			__func__, reg);
		return 0;
	}

	
	writel(ULPI_RUN | ULPI_READ | ULPI_ADDR(reg),
	       USB_ULPI_VIEWPORT);

	
	while (cnt < ULPI_IO_TIMEOUT_USEC) {
		if (!(readl(USB_ULPI_VIEWPORT) & ULPI_RUN))
			break;
		udelay(1);
		cnt++;
	}

	if (cnt >= ULPI_IO_TIMEOUT_USEC) {
		dev_err(phy->dev, "ulpi_read: timeout %08x\n",
			readl(USB_ULPI_VIEWPORT));
		dev_err(phy->dev, "PORTSC: %08x USBCMD: %08x\n",
			readl_relaxed(USB_PORTSC), readl_relaxed(USB_USBCMD));
		return -ETIMEDOUT;
	}
	return ULPI_DATA_READ(readl(USB_ULPI_VIEWPORT));
}

static int ulpi_write(struct usb_phy *phy, u32 val, u32 reg)
{
	struct msm_otg *motg = container_of(phy, struct msm_otg, phy);
	int cnt = 0;

	if (motg->pdata->emulation)
		return 0;

	if (motg->pdata->phy_type == QUSB_ULPI_PHY && reg > 0x3F) {
		pr_debug("%s: ULPI vendor-specific reg 0x%02x not supported\n",
			__func__, reg);
		return 0;
	}

	
	writel(ULPI_RUN | ULPI_WRITE |
	       ULPI_ADDR(reg) | ULPI_DATA(val),
	       USB_ULPI_VIEWPORT);

	
	while (cnt < ULPI_IO_TIMEOUT_USEC) {
		if (!(readl(USB_ULPI_VIEWPORT) & ULPI_RUN))
			break;
		udelay(1);
		cnt++;
	}

	if (cnt >= ULPI_IO_TIMEOUT_USEC) {
		dev_err(phy->dev, "ulpi_write: timeout\n");
		dev_err(phy->dev, "PORTSC: %08x USBCMD: %08x\n",
			readl_relaxed(USB_PORTSC), readl_relaxed(USB_USBCMD));
		return -ETIMEDOUT;
	}
	return 0;
}

static struct usb_phy_io_ops msm_otg_io_ops = {
	.read = ulpi_read,
	.write = ulpi_write,
};

static void ulpi_init(struct msm_otg *motg)
{
	struct msm_otg_platform_data *pdata = motg->pdata;
	int aseq[10];
	int *seq = NULL;

	if (override_phy_init) {
		printk(KERN_WARNING "%s(): HUSB PHY Init:%s\n", __func__,
				override_phy_init);
		get_options(override_phy_init, ARRAY_SIZE(aseq), aseq);
		seq = &aseq[1];
	} else {
		seq = pdata->phy_init_seq;
	}

	if (!seq)
		return;

	while (seq[0] >= 0) {
		if (override_phy_init)
			printk(KERN_WARNING "ulpi: write 0x%02x to 0x%02x\n",
					seq[0], seq[1]);

		printk(KERN_WARNING "ulpi: write 0x%02x to 0x%02x\n",
				seq[0], seq[1]);
		msm_otg_dbg_log_event(&motg->phy, "ULPI WRITE", seq[0], seq[1]);
		ulpi_write(&motg->phy, seq[0], seq[1]);
		seq += 2;
	}
}

static int msm_otg_phy_clk_reset(struct msm_otg *motg)
{
	int ret;

	if (!motg->phy_reset_clk)
		return 0;

	if (motg->sleep_clk)
		clk_disable_unprepare(motg->sleep_clk);
	if (motg->phy_csr_clk)
		clk_disable_unprepare(motg->phy_csr_clk);

	ret = clk_reset(motg->phy_reset_clk, CLK_RESET_ASSERT);
	if (ret < 0) {
		pr_err("phy_reset_clk assert failed %d\n", ret);
		return ret;
	}
	usleep_range(10, 15);
	ret = clk_reset(motg->phy_reset_clk, CLK_RESET_DEASSERT);
	if (ret < 0) {
		pr_err("phy_reset_clk de-assert failed %d\n", ret);
		return ret;
	}
	usleep_range(80, 100);

	if (motg->phy_csr_clk)
		clk_prepare_enable(motg->phy_csr_clk);
	if (motg->sleep_clk)
		clk_prepare_enable(motg->sleep_clk);

	return 0;
}

static int msm_otg_link_clk_reset(struct msm_otg *motg, bool assert)
{
	int ret;

	if (assert) {
		
		dev_dbg(motg->phy.dev, "block_reset ASSERT\n");
		clk_disable_unprepare(motg->pclk);
		clk_disable_unprepare(motg->core_clk);
		ret = clk_reset(motg->core_clk, CLK_RESET_ASSERT);
		if (ret)
			dev_err(motg->phy.dev, "usb hs_clk assert failed\n");
	} else {
		dev_dbg(motg->phy.dev, "block_reset DEASSERT\n");
		ret = clk_reset(motg->core_clk, CLK_RESET_DEASSERT);
		ndelay(200);
		ret = clk_prepare_enable(motg->core_clk);
		WARN(ret, "USB core_clk enable failed\n");
		ret = clk_prepare_enable(motg->pclk);
		WARN(ret, "USB pclk enable failed\n");
		if (ret)
			dev_err(motg->phy.dev, "usb hs_clk deassert failed\n");
	}
	return ret;
}

static int msm_otg_phy_reset(struct msm_otg *motg)
{
	u32 val;
	int ret;
	struct msm_otg_platform_data *pdata = motg->pdata;

	val = readl_relaxed(USB_AHBMODE);
	if (val & AHB2AHB_BYPASS) {
		pr_err("%s(): AHB2AHB_BYPASS SET: AHBMODE:%x\n",
						__func__, val);
		val &= ~AHB2AHB_BYPASS_BIT_MASK;
		writel_relaxed(val | AHB2AHB_BYPASS_CLEAR, USB_AHBMODE);
		pr_err("%s(): AHBMODE: %x\n", __func__,
				readl_relaxed(USB_AHBMODE));
	}

	ret = msm_otg_link_clk_reset(motg, 1);
	if (ret)
		return ret;

	msm_otg_phy_clk_reset(motg);

	
	usleep_range(1000, 1200);

	ret = msm_otg_link_clk_reset(motg, 0);
	if (ret)
		return ret;

	if (pdata && pdata->enable_sec_phy)
		writel_relaxed(readl_relaxed(USB_PHY_CTRL2) | (1<<16),
							USB_PHY_CTRL2);
	val = readl(USB_PORTSC) & ~PORTSC_PTS_MASK;
	writel(val | PORTSC_PTS_ULPI, USB_PORTSC);

	printk(KERN_WARNING "[USB] phy_reset: success\n");
	msm_otg_dbg_log_event(&motg->phy, "PHY RESET SUCCESS",
			motg->inputs, motg->phy.state);
	return 0;
}

#define LINK_RESET_TIMEOUT_USEC		(250 * 1000)
static int msm_otg_link_reset(struct msm_otg *motg)
{
	int cnt = 0;
	struct msm_otg_platform_data *pdata = motg->pdata;

	writel_relaxed(USBCMD_RESET, USB_USBCMD);
	while (cnt < LINK_RESET_TIMEOUT_USEC) {
		if (!(readl_relaxed(USB_USBCMD) & USBCMD_RESET))
			break;
		udelay(1);
		cnt++;
	}
	if (cnt >= LINK_RESET_TIMEOUT_USEC)
		return -ETIMEDOUT;

	
	writel_relaxed(0x80000000, USB_PORTSC);
	writel_relaxed(0x0, USB_AHBBURST);
	writel_relaxed(0x08, USB_AHBMODE);

	if (pdata && pdata->enable_sec_phy)
		writel_relaxed(readl_relaxed(USB_PHY_CTRL2) | (1<<16),
								USB_PHY_CTRL2);
	return 0;
}

#define QUSB2PHY_PORT_POWERDOWN		0xB4
#define QUSB2PHY_PORT_UTMI_CTRL2	0xC4

static void msm_usb_phy_reset(struct msm_otg *motg)
{
	u32 val;
	int ret, *seq;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
		
		val =  readl_relaxed(motg->usb_phy_ctrl_reg);
		val &= ~PHY_POR_BIT_MASK;
		val |= PHY_POR_ASSERT;
		writel_relaxed(val, motg->usb_phy_ctrl_reg);

		usleep_range(10, 15);

		
		val =  readl_relaxed(motg->usb_phy_ctrl_reg);
		val &= ~PHY_POR_BIT_MASK;
		val |= PHY_POR_DEASSERT;
		writel_relaxed(val, motg->usb_phy_ctrl_reg);
		break;
	case QUSB_ULPI_PHY:
		ret = clk_reset(motg->phy_reset_clk, CLK_RESET_ASSERT);
		if (ret) {
			pr_err("phy_reset_clk assert failed %d\n", ret);
			break;
		}

		
		usleep(10);

		ret = clk_reset(motg->phy_reset_clk, CLK_RESET_DEASSERT);
		if (ret) {
			pr_err("phy_reset_clk de-assert failed %d\n", ret);
			break;
		}

		
		mb();

		writel_relaxed(0x23,
				motg->phy_csr_regs + QUSB2PHY_PORT_POWERDOWN);
		writel_relaxed(0x0,
				motg->phy_csr_regs + QUSB2PHY_PORT_UTMI_CTRL2);

		
		seq = motg->pdata->phy_init_seq;
		if (seq) {
			while (seq[0] >= 0) {
				writel_relaxed(seq[1],
						motg->phy_csr_regs + seq[0]);
				seq += 2;
			}
		}

		
		wmb();
		writel_relaxed(0x22,
				motg->phy_csr_regs + QUSB2PHY_PORT_POWERDOWN);
		break;
	case SNPS_FEMTO_PHY:
		if (!motg->phy_por_clk) {
			pr_err("phy_por_clk missing\n");
			break;
		}
		ret = clk_reset(motg->phy_por_clk, CLK_RESET_ASSERT);
		if (ret) {
			pr_err("phy_por_clk assert failed %d\n", ret);
			break;
		}

		val = readb_relaxed(USB_PHY_CSR_PHY_CTRL_COMMON0);
		val &= ~SIDDQ;
		writeb_relaxed(val, USB_PHY_CSR_PHY_CTRL_COMMON0);

		usleep_range(10, 20);
		ret = clk_reset(motg->phy_por_clk, CLK_RESET_DEASSERT);
		if (ret) {
			pr_err("phy_por_clk de-assert failed %d\n", ret);
			break;
		}
		usleep_range(80, 100);
		break;
	default:
		break;
	}
	
	mb();
}

static int msm_otg_reset(struct usb_phy *phy)
{
	struct msm_otg *motg = container_of(phy, struct msm_otg, phy);
	struct msm_otg_platform_data *pdata = motg->pdata;
	int ret;
	u32 val = 0;
	u32 ulpi_val = 0;

	msm_otg_dbg_log_event(&motg->phy, "USB RESET", phy->state,
			get_pm_runtime_counter(phy->dev));
	if (motg->err_event_seen) {
		dev_info(phy->dev, "performing USB h/w reset for recovery\n");
	} else if (pdata->disable_reset_on_disconnect && motg->reset_counter) {
		return 0;
	}
	motg->reset_counter++;

	ret = msm_otg_phy_reset(motg);
	if (ret) {
		dev_err(phy->dev, "phy_reset failed\n");
		return ret;
	}

	aca_id_turned_on = false;
	ret = msm_otg_link_reset(motg);
	if (ret) {
		dev_err(phy->dev, "link reset failed\n");
		return ret;
	}

	msleep(100);

	
	msm_usb_phy_reset(motg);

	
	ulpi_init(motg);

	msm_usb_phy_reset(motg);

	if (pdata->otg_control == OTG_PHY_CONTROL) {
		val = readl_relaxed(USB_OTGSC);
		if (pdata->mode == USB_OTG) {
			ulpi_val = ULPI_INT_IDGRD | ULPI_INT_SESS_VALID;
			val |= OTGSC_IDIE | OTGSC_BSVIE;
		} else if (pdata->mode == USB_PERIPHERAL) {
			ulpi_val = ULPI_INT_SESS_VALID;
			val |= OTGSC_BSVIE;
		}
		writel_relaxed(val, USB_OTGSC);
		ulpi_write(phy, ulpi_val, ULPI_USB_INT_EN_RISE);
		ulpi_write(phy, ulpi_val, ULPI_USB_INT_EN_FALL);
	} else if (pdata->otg_control == OTG_PMIC_CONTROL) {
		ulpi_write(phy, OTG_COMP_DISABLE,
			ULPI_SET(ULPI_PWR_CLK_MNG_REG));
		
		pm8xxx_usb_id_pullup(1);
		if (motg->phy_irq)
			writeb_relaxed(USB_PHY_ID_MASK,
				USB2_PHY_USB_PHY_INTERRUPT_MASK1);
	}

	if (motg->caps & ALLOW_VDD_MIN_WITH_RETENTION_DISABLED)
		writel_relaxed(readl_relaxed(USB_OTGSC) & ~(OTGSC_IDPU),
								USB_OTGSC);

	msm_otg_dbg_log_event(&motg->phy, "USB RESET DONE", phy->state,
			get_pm_runtime_counter(phy->dev));

	if (pdata->enable_axi_prefetch)
		writel_relaxed(readl_relaxed(USB_HS_APF_CTRL) | (APF_CTRL_EN),
							USB_HS_APF_CTRL);

	msm_usb_bam_enable(CI_CTRL, phy->otg->gadget->bam2bam_func_enabled);

	return 0;
}

static const char *timer_string(int bit)
{
	switch (bit) {
	case A_WAIT_VRISE:		return "a_wait_vrise";
	case A_WAIT_VFALL:		return "a_wait_vfall";
	case B_SRP_FAIL:		return "b_srp_fail";
	case A_WAIT_BCON:		return "a_wait_bcon";
	case A_AIDL_BDIS:		return "a_aidl_bdis";
	case A_BIDL_ADIS:		return "a_bidl_adis";
	case B_ASE0_BRST:		return "b_ase0_brst";
	case A_TST_MAINT:		return "a_tst_maint";
	case B_TST_SRP:			return "b_tst_srp";
	case B_TST_CONFIG:		return "b_tst_config";
	default:			return "UNDEFINED";
	}
}

static enum hrtimer_restart msm_otg_timer_func(struct hrtimer *hrtimer)
{
	struct msm_otg *motg = container_of(hrtimer, struct msm_otg, timer);

	switch (motg->active_tmout) {
	case A_WAIT_VRISE:
		
		set_bit(A_VBUS_VLD, &motg->inputs);
		break;
	case A_TST_MAINT:
		
		set_bit(A_BUS_DROP, &motg->inputs);
		break;
	case B_TST_SRP:
		set_bit(B_BUS_REQ, &motg->inputs);
		break;
	case B_TST_CONFIG:
		clear_bit(A_CONN, &motg->inputs);
		break;
	default:
		set_bit(motg->active_tmout, &motg->tmouts);
	}

	printk(KERN_WARNING "expired %s timer\n", timer_string(motg->active_tmout));
	queue_work(motg->otg_wq, &motg->sm_work);
	return HRTIMER_NORESTART;
}

static void msm_otg_del_timer(struct msm_otg *motg)
{
	int bit = motg->active_tmout;

	printk(KERN_WARNING "deleting %s timer. remaining %lld msec\n", timer_string(bit),
			div_s64(ktime_to_us(hrtimer_get_remaining(
					&motg->timer)), 1000));
	hrtimer_cancel(&motg->timer);
	clear_bit(bit, &motg->tmouts);
}

static void msm_otg_start_timer(struct msm_otg *motg, int time, int bit)
{
	clear_bit(bit, &motg->tmouts);
	motg->active_tmout = bit;
	printk(KERN_WARNING "starting %s timer\n", timer_string(bit));
	hrtimer_start(&motg->timer,
			ktime_set(time / 1000, (time % 1000) * 1000000),
			HRTIMER_MODE_REL);
}

static void msm_otg_init_timer(struct msm_otg *motg)
{
	hrtimer_init(&motg->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	motg->timer.function = msm_otg_timer_func;
}

static int msm_otg_start_hnp(struct usb_otg *otg)
{
	struct msm_otg *motg = container_of(otg->phy, struct msm_otg, phy);

	if (otg->phy->state != OTG_STATE_A_HOST) {
		pr_err("HNP can not be initiated in %s state\n",
				usb_otg_state_string(otg->phy->state));
		return -EINVAL;
	}

	printk(KERN_WARNING "A-Host: HNP initiated\n");
	msm_otg_dbg_log_event(&motg->phy, "A_HOST: HNP INITIATED",
			motg->inputs, otg->phy->state);
	clear_bit(A_BUS_REQ, &motg->inputs);
	queue_work(motg->otg_wq, &motg->sm_work);
	return 0;
}

static int msm_otg_start_srp(struct usb_otg *otg)
{
	struct msm_otg *motg = container_of(otg->phy, struct msm_otg, phy);
	u32 val;
	int ret = 0;

	if (otg->phy->state != OTG_STATE_B_IDLE) {
		pr_err("SRP can not be initiated in %s state\n",
				usb_otg_state_string(otg->phy->state));
		ret = -EINVAL;
		goto out;
	}

	if ((jiffies - motg->b_last_se0_sess) < msecs_to_jiffies(TB_SRP_INIT)) {
		printk(KERN_WARNING "initial conditions of SRP are not met. Try again"
				"after some time\n");
		ret = -EAGAIN;
		goto out;
	}

	printk(KERN_WARNING "B-Device SRP started\n");
	msm_otg_dbg_log_event(&motg->phy, "B_DEVICE: SRP STARTED",
			motg->inputs, otg->phy->state);

	ulpi_write(otg->phy, 0x03, 0x97);
	val = readl_relaxed(USB_OTGSC);
	writel_relaxed((val & ~OTGSC_INTSTS_MASK) | OTGSC_HADP, USB_OTGSC);

	
out:
	return ret;
}

static void msm_otg_host_hnp_enable(struct usb_otg *otg, bool enable)
{
	struct usb_hcd *hcd = bus_to_hcd(otg->host);
	struct usb_device *rhub = otg->host->root_hub;

	if (enable) {
		pm_runtime_disable(&rhub->dev);
		rhub->state = USB_STATE_NOTATTACHED;
		hcd->driver->bus_suspend(hcd);
		clear_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
	} else {
		usb_remove_hcd(hcd);
		msm_otg_reset(otg->phy);
		usb_add_hcd(hcd, hcd->irq, IRQF_SHARED);
	}
}

#define HOST_SUSPEND_WQ_TIMEOUT_MS	msecs_to_jiffies(2000) 
static int msm_otg_set_suspend(struct usb_phy *phy, int suspend)
{
	struct msm_otg *motg = container_of(phy, struct msm_otg, phy);

	if (aca_enabled())
		return 0;

	pr_debug("%s(%d) in %s state\n", __func__, suspend,
				usb_otg_state_string(phy->state));
	msm_otg_dbg_log_event(phy, "SET SUSPEND", suspend, phy->state);

	if (suspend) {
		switch (phy->state) {
		case OTG_STATE_A_WAIT_BCON:
			if (TA_WAIT_BCON > 0)
				break;
			
		case OTG_STATE_A_HOST:
			printk(KERN_WARNING "host bus suspend\n");
			msm_otg_dbg_log_event(phy, "HOST BUS SUSPEND",
					motg->inputs, phy->state);
			clear_bit(A_BUS_REQ, &motg->inputs);
			if (!atomic_read(&motg->in_lpm) &&
				!test_bit(ID, &motg->inputs)) {
				queue_work(motg->otg_wq, &motg->sm_work);
				wait_event_interruptible_timeout(
					motg->host_suspend_wait,
					(atomic_read(&motg->in_lpm)
					|| test_bit(ID, &motg->inputs)),
					HOST_SUSPEND_WQ_TIMEOUT_MS);
			}
			break;
		case OTG_STATE_B_PERIPHERAL:
			printk(KERN_WARNING "peripheral bus suspend\n");
			msm_otg_dbg_log_event(phy, "PERIPHERAL BUS SUSPEND",
					motg->inputs, phy->state);
			if (!(motg->caps & ALLOW_LPM_ON_DEV_SUSPEND))
				break;
			set_bit(A_BUS_SUSPEND, &motg->inputs);
			if (!atomic_read(&motg->in_lpm))
				queue_delayed_work(motg->otg_wq,
					&motg->suspend_work,
					USB_SUSPEND_DELAY_TIME);
			break;

		default:
			break;
		}
	} else {
		switch (phy->state) {
		case OTG_STATE_A_WAIT_BCON:
			
			set_bit(A_BUS_REQ, &motg->inputs);
			
			if (atomic_read(&motg->in_lpm))
				pm_runtime_resume(phy->dev);
			break;
		case OTG_STATE_A_SUSPEND:
			
			set_bit(A_BUS_REQ, &motg->inputs);
			phy->state = OTG_STATE_A_HOST;

			
			if (atomic_read(&motg->in_lpm))
				pm_runtime_resume(phy->dev);
			break;
		case OTG_STATE_B_PERIPHERAL:
			printk(KERN_WARNING "peripheral bus resume\n");
			msm_otg_dbg_log_event(phy, "PERIPHERAL BUS RESUME",
					motg->inputs, phy->state);
			if (!(motg->caps & ALLOW_LPM_ON_DEV_SUSPEND))
				break;
			clear_bit(A_BUS_SUSPEND, &motg->inputs);
			if (atomic_read(&motg->in_lpm))
				queue_work(motg->otg_wq, &motg->sm_work);
			break;
		default:
			break;
		}
	}
	return 0;
}

static int msm_otg_bus_freq_get(struct device *dev, struct msm_otg *motg)
{
	struct device_node *np = dev->of_node;
	int len = 0;
	int i;
	int ret;

	if (!np)
		return -EINVAL;

	of_find_property(np, "qcom,bus-clk-rate", &len);
	if (!len || (len / sizeof(u32) != USB_NUM_BUS_CLOCKS)) {
		pr_err("Invalid bus clock rate parameters\n");
		return -EINVAL;
	}
	of_property_read_u32_array(np, "qcom,bus-clk-rate", bus_freqs,
		USB_NUM_BUS_CLOCKS);
	for (i = 0; i < USB_NUM_BUS_CLOCKS; i++) {
		motg->bus_clks[i] = devm_clk_get(motg->phy.dev,
				bus_clkname[i]);
		if (IS_ERR(motg->bus_clks[i])) {
			pr_err("%s get failed\n", bus_clkname[i]);
			return PTR_ERR(motg->bus_clks[i]);
		}
		ret = clk_set_rate(motg->bus_clks[i], bus_freqs[i]);
		if (ret) {
			pr_err("%s set rate failed: %d\n", bus_clkname[i],
				ret);
			return ret;
		}
		pr_debug("%s set at %lu Hz\n", bus_clkname[i],
			clk_get_rate(motg->bus_clks[i]));
		msm_otg_dbg_log_event(&motg->phy, "OTG BUS FREQ SET",
				i, bus_freqs[i]);
	}
	bus_clk_rate_set = true;
	return 0;
}

static void msm_otg_bus_clks_enable(struct msm_otg *motg)
{
	int i;
	int ret;

	if (!bus_clk_rate_set || motg->bus_clks_enabled)
		return;

	for (i = 0; i < USB_NUM_BUS_CLOCKS; i++) {
		ret = clk_prepare_enable(motg->bus_clks[i]);
		if (ret) {
			pr_err("%s enable rate failed: %d\n", bus_clkname[i],
				ret);
			goto err_clk_en;
		}
	}
	motg->bus_clks_enabled = true;
	return;
err_clk_en:
	for (--i; i >= 0; --i)
		clk_disable_unprepare(motg->bus_clks[i]);
}

static void msm_otg_bus_clks_disable(struct msm_otg *motg)
{
	int i;

	if (!bus_clk_rate_set || !motg->bus_clks_enabled)
		return;

	for (i = 0; i < USB_NUM_BUS_CLOCKS; i++)
		clk_disable_unprepare(motg->bus_clks[i]);
	motg->bus_clks_enabled = false;
}

static void msm_otg_bus_vote(struct msm_otg *motg, enum usb_bus_vote vote)
{
	int ret;
	struct msm_otg_platform_data *pdata = motg->pdata;

	msm_otg_dbg_log_event(&motg->phy, "BUS VOTE", vote, motg->phy.state);
	
	if (pdata->bus_scale_table &&
	    vote >= pdata->bus_scale_table->num_usecases)
		vote = USB_NO_PERF_VOTE;

	if (motg->bus_perf_client) {
		ret = msm_bus_scale_client_update_request(
			motg->bus_perf_client, vote);
		if (ret)
			dev_err(motg->phy.dev, "%s: Failed to vote (%d)\n"
				   "for bus bw %d\n", __func__, vote, ret);
		if (vote == USB_MAX_PERF_VOTE)
			msm_otg_bus_clks_enable(motg);
		else
			msm_otg_bus_clks_disable(motg);
	}
}

static void msm_otg_enable_phy_hv_int(struct msm_otg *motg)
{
	bool bsv_id_hv_int = false;
	bool dp_dm_hv_int = false;
	u32 val;

	if (motg->pdata->otg_control == OTG_PHY_CONTROL ||
				motg->phy_irq)
		bsv_id_hv_int = true;
	if (motg->host_bus_suspend || motg->device_bus_suspend)
		dp_dm_hv_int = true;

	if (!bsv_id_hv_int && !dp_dm_hv_int)
		return;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
		val = readl_relaxed(motg->usb_phy_ctrl_reg);
		if (bsv_id_hv_int)
			val |= (PHY_IDHV_INTEN | PHY_OTGSESSVLDHV_INTEN);
		if (dp_dm_hv_int)
			val |= PHY_CLAMP_DPDMSE_EN;
		writel_relaxed(val, motg->usb_phy_ctrl_reg);
		break;
	case SNPS_FEMTO_PHY:
		if (bsv_id_hv_int) {
			val = readb_relaxed(USB_PHY_CSR_PHY_CTRL1);
			val |= ID_HV_CLAMP_EN_N;
			writeb_relaxed(val, USB_PHY_CSR_PHY_CTRL1);
		}

		if (dp_dm_hv_int) {
			val = readb_relaxed(USB_PHY_CSR_PHY_CTRL3);
			val |= CLAMP_MPM_DPSE_DMSE_EN_N;
			writeb_relaxed(val, USB_PHY_CSR_PHY_CTRL3);
		}
	default:
		break;
	}
	pr_debug("%s: bsv_id_hv = %d dp_dm_hv_int = %d\n",
			__func__, bsv_id_hv_int, dp_dm_hv_int);
	msm_otg_dbg_log_event(&motg->phy, "PHY HV INTR ENABLED",
			bsv_id_hv_int, dp_dm_hv_int);
}

static void msm_otg_disable_phy_hv_int(struct msm_otg *motg)
{
	bool bsv_id_hv_int = false;
	bool dp_dm_hv_int = false;
	u32 val;

	if (motg->pdata->otg_control == OTG_PHY_CONTROL ||
				motg->phy_irq)
		bsv_id_hv_int = true;
	if (motg->host_bus_suspend || motg->device_bus_suspend)
		dp_dm_hv_int = true;

	if (!bsv_id_hv_int && !dp_dm_hv_int)
		return;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
		val = readl_relaxed(motg->usb_phy_ctrl_reg);
		if (bsv_id_hv_int)
			val &= ~(PHY_IDHV_INTEN | PHY_OTGSESSVLDHV_INTEN);
		if (dp_dm_hv_int)
			val &= ~PHY_CLAMP_DPDMSE_EN;
		writel_relaxed(val, motg->usb_phy_ctrl_reg);
		break;
	case SNPS_FEMTO_PHY:
		if (bsv_id_hv_int) {
			val = readb_relaxed(USB_PHY_CSR_PHY_CTRL1);
			val &= ~ID_HV_CLAMP_EN_N;
			writeb_relaxed(val, USB_PHY_CSR_PHY_CTRL1);
		}

		if (dp_dm_hv_int) {
			val = readb_relaxed(USB_PHY_CSR_PHY_CTRL3);
			val &= ~CLAMP_MPM_DPSE_DMSE_EN_N;
			writeb_relaxed(val, USB_PHY_CSR_PHY_CTRL3);
		}
		break;
	default:
		break;
	}
	pr_debug("%s: bsv_id_hv = %d dp_dm_hv_int = %d\n",
			__func__, bsv_id_hv_int, dp_dm_hv_int);
	msm_otg_dbg_log_event(&motg->phy, "PHY HV INTR DISABLED",
			bsv_id_hv_int, dp_dm_hv_int);
}

static void msm_otg_enter_phy_retention(struct msm_otg *motg)
{
	u32 val;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
		val = readl_relaxed(motg->usb_phy_ctrl_reg);
		val &= ~PHY_RETEN;
		writel_relaxed(val, motg->usb_phy_ctrl_reg);
		break;
	case SNPS_FEMTO_PHY:
		
		val = readb_relaxed(USB_PHY_CSR_PHY_CTRL_COMMON0);
		val |= SIDDQ;
		writeb_relaxed(val, USB_PHY_CSR_PHY_CTRL_COMMON0);
		break;
	default:
		break;
	}
	pr_debug("USB PHY is in retention\n");
	msm_otg_dbg_log_event(&motg->phy, "USB PHY ENTER RETENTION",
			motg->pdata->phy_type, 0);
}

static void msm_otg_exit_phy_retention(struct msm_otg *motg)
{
	int val;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
		val = readl_relaxed(motg->usb_phy_ctrl_reg);
		val |= PHY_RETEN;
		writel_relaxed(val, motg->usb_phy_ctrl_reg);
		break;
	case SNPS_FEMTO_PHY:
		msm_otg_reset(&motg->phy);
		break;
	default:
		break;
	}
	pr_debug("USB PHY is exited from retention\n");
	msm_otg_dbg_log_event(&motg->phy, "USB PHY EXIT RETENTION",
			motg->pdata->phy_type, 0);
}

static void msm_id_status_w(struct work_struct *w);
static irqreturn_t msm_otg_phy_irq_handler(int irq, void *data)
{
	struct msm_otg *motg = data;

	msm_otg_dbg_log_event(&motg->phy, "PHY ID IRQ",
			atomic_read(&motg->in_lpm), motg->phy.state);
	if (atomic_read(&motg->in_lpm)) {
		pr_debug("PHY ID IRQ in LPM\n");
		motg->phy_irq_pending = true;
		if (!atomic_read(&motg->pm_suspended))
			pm_request_resume(motg->phy.dev);
	} else {
		pr_debug("PHY ID IRQ outside LPM\n");
		msm_id_status_w(&motg->id_status_work.work);
	}

	return IRQ_HANDLED;
}

#define PHY_SUSPEND_TIMEOUT_USEC (5 * 1000)
#define PHY_DEVICE_BUS_SUSPEND_TIMEOUT_USEC 100
#define PHY_RESUME_TIMEOUT_USEC	(100 * 1000)

#define PHY_SUSPEND_RETRIES_MAX 3

static void msm_otg_set_vbus_state(int online);

#ifdef CONFIG_PM_SLEEP
static int msm_otg_suspend(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;
	struct usb_bus *bus = phy->otg->host;
	struct msm_otg_platform_data *pdata = motg->pdata;
	int cnt;
	bool host_bus_suspend, device_bus_suspend, dcp, prop_charger;
	bool floated_charger, sm_work_busy;
	u32 cmd_val;
	u32 portsc, config2;
	u32 func_ctrl;
	int phcd_retry_cnt = 0, ret;
	unsigned phy_suspend_timeout;

	cnt = 0;
	msm_otg_dbg_log_event(phy, "LPM ENTER START",
			motg->inputs, phy->state);

	if (atomic_read(&motg->in_lpm))
		return 0;

	if (!msm_bam_usb_lpm_ok(CI_CTRL)) {
		msm_otg_dbg_log_event(phy, "BAM NOT READY", 0, 0);
		pm_schedule_suspend(phy->dev, 1000);
		return -EBUSY;
	}

	disable_irq(motg->irq);
	if (motg->phy_irq)
		disable_irq(motg->phy_irq);
lpm_start:
	host_bus_suspend = !test_bit(MHL, &motg->inputs) && phy->otg->host &&
		!test_bit(ID, &motg->inputs);
	device_bus_suspend = phy->otg->gadget && test_bit(ID, &motg->inputs) &&
		test_bit(A_BUS_SUSPEND, &motg->inputs) &&
		motg->caps & ALLOW_LPM_ON_DEV_SUSPEND;

	dcp = (motg->chg_type == USB_DCP_CHARGER) && !motg->is_ext_chg_dcp;
	prop_charger = motg->chg_type == USB_PROPRIETARY_CHARGER;
	floated_charger = motg->chg_type == USB_FLOATED_CHARGER;

	
	sm_work_busy = !test_bit(B_SESS_VLD, &motg->inputs) &&
			phy->state == OTG_STATE_B_PERIPHERAL;

	
	if (motg->err_event_seen)
		msm_otg_reset(phy);

	config2 = readl_relaxed(USB_GENCONFIG2);
	if (device_bus_suspend)
		config2 |= GENCFG2_LINESTATE_DIFF_WAKEUP_EN;
	else
		config2 &= ~GENCFG2_LINESTATE_DIFF_WAKEUP_EN;
	writel_relaxed(config2, USB_GENCONFIG2);


	if ((test_bit(B_SESS_VLD, &motg->inputs) &&
		!device_bus_suspend && !dcp && !motg->is_ext_chg_dcp &&
		!prop_charger && !floated_charger) ||
		test_bit(A_BUS_REQ, &motg->inputs) || sm_work_busy) {
		msm_otg_dbg_log_event(phy, "LPM ENTER ABORTED",
				motg->inputs, motg->chg_type);
		if (test_bit(A_BUS_REQ, &motg->inputs))
			motg->pm_done = 1;
		enable_irq(motg->irq);
		if (motg->phy_irq)
			enable_irq(motg->phy_irq);
		return -EBUSY;
	}

	if (motg->caps & ALLOW_VDD_MIN_WITH_RETENTION_DISABLED ||
		(!host_bus_suspend && !device_bus_suspend)) {
		
		func_ctrl = ulpi_read(phy, ULPI_FUNC_CTRL);
		func_ctrl &= ~ULPI_FUNC_CTRL_OPMODE_MASK;
		func_ctrl |= ULPI_FUNC_CTRL_OPMODE_NONDRIVING;
		ulpi_write(phy, func_ctrl, ULPI_FUNC_CTRL);
		ulpi_write(phy, ULPI_IFC_CTRL_AUTORESUME,
						ULPI_CLR(ULPI_IFC_CTRL));
	}


phcd_retry:
	if (device_bus_suspend)
		phy_suspend_timeout = PHY_DEVICE_BUS_SUSPEND_TIMEOUT_USEC;
	else
		phy_suspend_timeout = PHY_SUSPEND_TIMEOUT_USEC;

	cnt = 0;
	portsc = readl_relaxed(USB_PORTSC);
	if (!(portsc & PORTSC_PHCD)) {
		writel_relaxed(portsc | PORTSC_PHCD,
				USB_PORTSC);
		while (cnt < phy_suspend_timeout) {
			if (readl_relaxed(USB_PORTSC) & PORTSC_PHCD)
				break;
			udelay(1);
			cnt++;
		}
	}

	if (cnt >= phy_suspend_timeout) {
		if (phcd_retry_cnt > PHY_SUSPEND_RETRIES_MAX) {
			msm_otg_dbg_log_event(phy, "PHY SUSPEND FAILED",
				phcd_retry_cnt, phy->state);
			dev_err(phy->dev, "PHY suspend failed\n");
			ret = -EBUSY;
			goto phy_suspend_fail;
		}

		if (device_bus_suspend) {
			dev_dbg(phy->dev, "PHY suspend aborted\n");
			ret = -EBUSY;
			goto phy_suspend_fail;
		} else {
			if (phcd_retry_cnt++ < PHY_SUSPEND_RETRIES_MAX) {
				dev_dbg(phy->dev, "PHY suspend retry\n");
				goto phcd_retry;
			} else {
				dev_err(phy->dev, "reset attempt during PHY suspend\n");
				phcd_retry_cnt++;
				motg->reset_counter = 0;
				msm_otg_reset(phy);
				goto lpm_start;
			}
		}
	}

	cmd_val = readl_relaxed(USB_USBCMD);
	if (host_bus_suspend || device_bus_suspend ||
		(motg->pdata->otg_control == OTG_PHY_CONTROL))
		cmd_val |= ASYNC_INTR_CTRL | ULPI_STP_CTRL;
	else
		cmd_val |= ULPI_STP_CTRL;
	writel_relaxed(cmd_val, USB_USBCMD);



	motg->host_bus_suspend = host_bus_suspend;
	motg->device_bus_suspend = device_bus_suspend;

	if (motg->caps & ALLOW_PHY_RETENTION && !device_bus_suspend && !dcp &&
		 (!host_bus_suspend || (motg->caps &
		ALLOW_BUS_SUSPEND_WITHOUT_REWORK) ||
		  ((motg->caps & ALLOW_HOST_PHY_RETENTION)
		&& (pdata->dpdm_pulldown_added || !(portsc & PORTSC_CCS))))) {
		msm_otg_enable_phy_hv_int(motg);
		if ((!host_bus_suspend || !(motg->caps &
			ALLOW_BUS_SUSPEND_WITHOUT_REWORK)) &&
			!(motg->caps & ALLOW_VDD_MIN_WITH_RETENTION_DISABLED)) {
			msm_otg_enter_phy_retention(motg);
			motg->lpm_flags |= PHY_RETENTIONED;
		}
	} else if (device_bus_suspend && !dcp &&
			(pdata->mpm_dpshv_int || pdata->mpm_dmshv_int)) {
		
		msm_otg_enable_phy_hv_int(motg);
		if (motg->caps & ALLOW_PHY_RETENTION && pdata->vddmin_gpio) {

			config2 = readl_relaxed(USB_GENCONFIG2);
			config2 &= ~GENCFG2_DPSE_DMSE_HV_INTR_EN;
			writel_relaxed(config2, USB_GENCONFIG2);

			msm_otg_enter_phy_retention(motg);
			motg->lpm_flags |= PHY_RETENTIONED;
			gpio_direction_output(pdata->vddmin_gpio, 1);
		}
	}

	
	mb();
	
	if (!(phy->state == OTG_STATE_B_PERIPHERAL &&
		test_bit(A_BUS_SUSPEND, &motg->inputs)) ||
	    !motg->pdata->core_clk_always_on_workaround) {
		clk_disable_unprepare(motg->pclk);
		clk_disable_unprepare(motg->core_clk);
		if (motg->phy_csr_clk)
			clk_disable_unprepare(motg->phy_csr_clk);
		motg->lpm_flags |= CLOCKS_DOWN;
	}

	
	if (!host_bus_suspend || (motg->caps &
		ALLOW_BUS_SUSPEND_WITHOUT_REWORK) ||
		((motg->caps & ALLOW_HOST_PHY_RETENTION) &&
		(pdata->dpdm_pulldown_added || !(portsc & PORTSC_CCS)))) {
		if (motg->xo_clk) {
			clk_disable_unprepare(motg->xo_clk);
			motg->lpm_flags |= XO_SHUTDOWN;
		}
	}

	if (motg->caps & ALLOW_PHY_POWER_COLLAPSE &&
			!host_bus_suspend && !dcp && !device_bus_suspend) {
		msm_hsusb_ldo_enable(motg, USB_PHY_REG_OFF);
		motg->lpm_flags |= PHY_PWR_COLLAPSED;
	} else if (motg->caps & ALLOW_PHY_REGULATORS_LPM &&
			!host_bus_suspend && !device_bus_suspend && !dcp) {
		msm_hsusb_ldo_enable(motg, USB_PHY_REG_LPM_ON);
		motg->lpm_flags |= PHY_REGULATORS_LPM;
	}

	if (motg->lpm_flags & PHY_RETENTIONED ||
		(motg->caps & ALLOW_VDD_MIN_WITH_RETENTION_DISABLED)) {
		msm_hsusb_config_vddcx(0);
		msm_hsusb_mhl_switch_enable(motg, 0);
	}

	if (device_may_wakeup(phy->dev)) {
		
		if (host_bus_suspend || device_bus_suspend) {
			enable_irq_wake(motg->async_irq);
			enable_irq_wake(motg->irq);
		}
		

		if (motg->phy_irq)
			enable_irq_wake(motg->phy_irq);
		if (motg->pdata->pmic_id_irq)
			enable_irq_wake(motg->pdata->pmic_id_irq);
		if (motg->ext_id_irq)
			enable_irq_wake(motg->ext_id_irq);
		if (pdata->otg_control == OTG_PHY_CONTROL &&
			pdata->mpm_otgsessvld_int)
			msm_mpm_set_pin_wake(pdata->mpm_otgsessvld_int, 1);
		if ((host_bus_suspend || device_bus_suspend) &&
				pdata->mpm_dpshv_int)
			msm_mpm_set_pin_wake(pdata->mpm_dpshv_int, 1);
		if ((host_bus_suspend || device_bus_suspend) &&
				pdata->mpm_dmshv_int)
			msm_mpm_set_pin_wake(pdata->mpm_dmshv_int, 1);
	}
	if (bus)
		clear_bit(HCD_FLAG_HW_ACCESSIBLE, &(bus_to_hcd(bus))->flags);

	msm_otg_bus_vote(motg, USB_NO_PERF_VOTE);

	atomic_set(&motg->in_lpm, 1);
	wake_up(&motg->host_suspend_wait);

	if (host_bus_suspend || device_bus_suspend) {
		
		enable_irq(motg->async_irq);
		enable_irq(motg->irq);
	}

	if (motg->phy_irq)
		enable_irq(motg->phy_irq);
	if (host_bus_suspend)
		enable_irq(motg->irq);
	

	wake_unlock(&motg->wlock);

	dev_dbg(phy->dev, "LPM caps = %lu flags = %lu\n",
			motg->caps, motg->lpm_flags);
	dev_info(phy->dev, "USB in low power mode\n");
	msm_otg_dbg_log_event(phy, "LPM ENTER DONE",
			motg->caps, motg->lpm_flags);

	if (motg->err_event_seen) {
		motg->err_event_seen = false;
		if (motg->vbus_state != test_bit(B_SESS_VLD, &motg->inputs))
			msm_otg_set_vbus_state(motg->vbus_state);
		if (motg->id_state != test_bit(ID, &motg->inputs))
			msm_id_status_w(&motg->id_status_work.work);
	}

	return 0;

phy_suspend_fail:
	enable_irq(motg->irq);
	if (motg->phy_irq)
		enable_irq(motg->phy_irq);
	return ret;
}

static int msm_otg_resume(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;
	struct usb_bus *bus = phy->otg->host;
	struct usb_hcd *hcd = bus_to_hcd(phy->otg->host);
	struct msm_otg_platform_data *pdata = motg->pdata;
	int cnt = 0;
	unsigned temp;
	unsigned ret;
	bool in_device_mode;
	bool bus_is_suspended;
	bool is_remote_wakeup;
	u32 func_ctrl;

	msm_otg_dbg_log_event(phy, "LPM EXIT START", motg->inputs, phy->state);
	if (!atomic_read(&motg->in_lpm)) {
		msm_otg_dbg_log_event(phy, "USB NOT IN LPM",
				atomic_read(&motg->in_lpm), phy->state);
		return 0;
	}
	USB_INFO("%s\n", __func__);

	msm_bam_notify_lpm_resume(CI_CTRL);

	if (motg->host_bus_suspend || motg->device_bus_suspend)
		disable_irq(motg->irq);

	wake_lock(&motg->wlock);


	if (motg->device_bus_suspend && debug_bus_voting_enabled)
		msm_otg_bus_vote(motg, USB_MAX_PERF_VOTE);
	else
		msm_otg_bus_vote(motg, USB_MIN_PERF_VOTE);
	printk(KERN_WARNING "[USB] msm_otg_resume 2\n");

	
	if (motg->lpm_flags & XO_SHUTDOWN) {
		if (motg->xo_clk)
			clk_prepare_enable(motg->xo_clk);
		motg->lpm_flags &= ~XO_SHUTDOWN;
	}

	if (motg->lpm_flags & CLOCKS_DOWN) {
		if (motg->phy_csr_clk) {
			ret = clk_prepare_enable(motg->phy_csr_clk);
			WARN(ret, "USB phy_csr_clk enable failed\n");
		}
		ret = clk_prepare_enable(motg->core_clk);
		WARN(ret, "USB core_clk enable failed\n");
		ret = clk_prepare_enable(motg->pclk);
		WARN(ret, "USB pclk enable failed\n");
		motg->lpm_flags &= ~CLOCKS_DOWN;
	}

	if (motg->lpm_flags & PHY_PWR_COLLAPSED) {
		msm_hsusb_ldo_enable(motg, USB_PHY_REG_ON);
		motg->lpm_flags &= ~PHY_PWR_COLLAPSED;
	} else if (motg->lpm_flags & PHY_REGULATORS_LPM) {
		msm_hsusb_ldo_enable(motg, USB_PHY_REG_LPM_OFF);
		motg->lpm_flags &= ~PHY_REGULATORS_LPM;
	}

	if (motg->lpm_flags & PHY_RETENTIONED ||
		(motg->caps & ALLOW_VDD_MIN_WITH_RETENTION_DISABLED)) {
		msm_hsusb_mhl_switch_enable(motg, 1);
		msm_hsusb_config_vddcx(1);
		msm_otg_disable_phy_hv_int(motg);
		msm_otg_exit_phy_retention(motg);
		motg->lpm_flags &= ~PHY_RETENTIONED;
		if (pdata->vddmin_gpio && motg->device_bus_suspend)
			gpio_direction_input(pdata->vddmin_gpio);
	} else if (motg->device_bus_suspend) {
		msm_otg_disable_phy_hv_int(motg);
	}

	temp = readl(USB_USBCMD);
	temp &= ~ASYNC_INTR_CTRL;
	temp &= ~ULPI_STP_CTRL;
	writel(temp, USB_USBCMD);

	if (!(readl_relaxed(USB_PORTSC) & PORTSC_PHCD))
		goto skip_phy_resume;

	in_device_mode =
		phy->otg->gadget &&
		test_bit(ID, &motg->inputs);

	bus_is_suspended =
		readl_relaxed(USB_PORTSC) & PORTSC_SUSP_MASK;

	is_remote_wakeup = in_device_mode && bus_is_suspended;

	if (is_remote_wakeup &&
	    (atomic_read(&(motg->set_fpr_with_lpm_exit)) ||
	     pdata->rw_during_lpm_workaround)) {
		/* In some targets there is a HW issue with remote wakeup
		 * during low-power mode. As a workaround, the FPR bit
		 * is written simultaneously with the clearing of the
		 * PHCD bit.
		 */
		writel_relaxed(
			(readl_relaxed(USB_PORTSC) & ~PORTSC_PHCD) |
			PORTSC_FPR_MASK,
			USB_PORTSC);

		atomic_set(&(motg->set_fpr_with_lpm_exit), 0);
	} else {
		writel_relaxed(readl_relaxed(USB_PORTSC) & ~PORTSC_PHCD,
			USB_PORTSC);
	}

	while (cnt < PHY_RESUME_TIMEOUT_USEC) {
		if (!(readl_relaxed(USB_PORTSC) & PORTSC_PHCD))
			break;
		udelay(1);
		cnt++;
	}

	if (cnt >= PHY_RESUME_TIMEOUT_USEC) {
		USBH_ERR("Unable to resume USB."
				"Re-plugin the cable\n");
		msm_otg_reset(phy);
	}

skip_phy_resume:
	if (motg->caps & ALLOW_VDD_MIN_WITH_RETENTION_DISABLED ||
		(!motg->host_bus_suspend && !motg->device_bus_suspend)) {
		
		func_ctrl = ulpi_read(phy, ULPI_FUNC_CTRL);
		func_ctrl &= ~ULPI_FUNC_CTRL_OPMODE_MASK;
		func_ctrl |= ULPI_FUNC_CTRL_OPMODE_NORMAL;
		ulpi_write(phy, func_ctrl, ULPI_FUNC_CTRL);
	}

	if (device_may_wakeup(phy->dev)) {
		
		if (motg->host_bus_suspend || motg->device_bus_suspend) {
			disable_irq_wake(motg->async_irq);
			disable_irq_wake(motg->irq);
		}
		

		if (motg->phy_irq)
			disable_irq_wake(motg->phy_irq);
		if (motg->pdata->pmic_id_irq)
			disable_irq_wake(motg->pdata->pmic_id_irq);
		if (motg->ext_id_irq)
			disable_irq_wake(motg->ext_id_irq);
		if (pdata->otg_control == OTG_PHY_CONTROL &&
			pdata->mpm_otgsessvld_int)
			msm_mpm_set_pin_wake(pdata->mpm_otgsessvld_int, 0);
		if ((motg->host_bus_suspend || motg->device_bus_suspend) &&
			pdata->mpm_dpshv_int)
			msm_mpm_set_pin_wake(pdata->mpm_dpshv_int, 0);
		if ((motg->host_bus_suspend || motg->device_bus_suspend) &&
			pdata->mpm_dmshv_int)
			msm_mpm_set_pin_wake(pdata->mpm_dmshv_int, 0);
	}
	if (bus)
		set_bit(HCD_FLAG_HW_ACCESSIBLE, &(bus_to_hcd(bus))->flags);

	atomic_set(&motg->in_lpm, 0);

	if (motg->async_int) {
		
		enable_irq(motg->async_int);
		motg->async_int = 0;
		if (phy->state >= OTG_STATE_A_IDLE)
			set_bit(A_BUS_REQ, &motg->inputs);
	}
	enable_irq(motg->irq);

	
	if (motg->host_bus_suspend || motg->device_bus_suspend)
		disable_irq(motg->async_irq);

	if (motg->phy_irq_pending) {
		motg->phy_irq_pending = false;
		msm_id_status_w(&motg->id_status_work.work);
	}

	if (motg->host_bus_suspend)
		usb_hcd_resume_root_hub(hcd);

	pr_info("USB exited from low power mode\n");
	msm_otg_dbg_log_event(phy, "LPM EXIT DONE",
			motg->caps, motg->lpm_flags);

	return 0;
}
#endif

static void msm_otg_notify_host_mode(struct msm_otg *motg, bool host_mode)
{
	if (!psy) {
		pr_err("No USB power supply registered!\n");
		return;
	}

	if (legacy_power_supply) {
		
		if (host_mode) {
			power_supply_set_scope(psy, POWER_SUPPLY_SCOPE_SYSTEM);
		} else {
			power_supply_set_scope(psy, POWER_SUPPLY_SCOPE_DEVICE);
			if (test_bit(ID_A, &motg->inputs))
				msleep(50);
		}
	} else {
		motg->host_mode = host_mode;
		power_supply_changed(psy);
	}
}

static int msm_otg_notify_chg_type(struct msm_otg *motg)
{
	static int charger_type;

	if (charger_type == motg->chg_type)
		return 0;

	if (motg->chg_type == USB_SDP_CHARGER)
		charger_type = POWER_SUPPLY_TYPE_USB;
	else if (motg->chg_type == USB_CDP_CHARGER)
		charger_type = POWER_SUPPLY_TYPE_USB_CDP;
	else if (motg->chg_type == USB_DCP_CHARGER ||
			motg->chg_type == USB_PROPRIETARY_CHARGER ||
			motg->chg_type == USB_FLOATED_CHARGER)
		charger_type = POWER_SUPPLY_TYPE_USB_DCP;
	else if ((motg->chg_type == USB_ACA_DOCK_CHARGER ||
		motg->chg_type == USB_ACA_A_CHARGER ||
		motg->chg_type == USB_ACA_B_CHARGER ||
		motg->chg_type == USB_ACA_C_CHARGER))
		charger_type = POWER_SUPPLY_TYPE_USB_ACA;
	else
		charger_type = POWER_SUPPLY_TYPE_UNKNOWN;

	if (!psy) {
		pr_err("No USB power supply registered!\n");
		return -EINVAL;
	}

	printk(KERN_WARNING "[%s]setting usb power supply type %d\n",__func__, charger_type);
	msm_otg_dbg_log_event(&motg->phy, "SET USB PWR SUPPLY TYPE",
			motg->chg_type, charger_type);
	power_supply_set_supply_type(psy, charger_type);
	return 0;
}

static int msm_otg_notify_power_supply(struct msm_otg *motg, unsigned mA)
{
	if (!psy) {
		dev_dbg(motg->phy.dev, "no usb power supply registered\n");
		goto psy_error;
	}

	if (motg->cur_power == 0 && mA > 2) {
		
		if (power_supply_set_online(psy, true))
			goto psy_error;
		if (power_supply_set_current_limit(psy, 1000*mA))
			goto psy_error;
	} else if (motg->cur_power >= 0 && (mA == 0 || mA == 2)) {
		
		if (power_supply_set_online(psy, false))
			goto psy_error;
		
		if (power_supply_set_current_limit(psy, 1000*mA))
			goto psy_error;
	} else {
		if (power_supply_set_online(psy, true))
			goto psy_error;
		
		if (power_supply_set_current_limit(psy, 1000*mA))
			goto psy_error;
	}

	power_supply_changed(psy);
	return 0;

psy_error:
	dev_dbg(motg->phy.dev, "power supply error when setting property\n");
	return -ENXIO;
}

static void msm_otg_set_online_status(struct msm_otg *motg)
{
	if (!psy) {
		dev_dbg(motg->phy.dev, "no usb power supply registered\n");
		return;
	}

	
	if (power_supply_set_online(psy, false))
		dev_dbg(motg->phy.dev, "error setting power supply property\n");
}

static void msm_otg_notify_charger(struct msm_otg *motg, unsigned mA)
{
	struct usb_gadget *g = motg->phy.otg->gadget;

	if (g && g->is_a_peripheral)
		return;

	if ((motg->chg_type == USB_ACA_DOCK_CHARGER ||
		motg->chg_type == USB_ACA_A_CHARGER ||
		motg->chg_type == USB_ACA_B_CHARGER ||
		motg->chg_type == USB_ACA_C_CHARGER) &&
			mA > IDEV_ACA_CHG_LIMIT)
		mA = IDEV_ACA_CHG_LIMIT;

	dev_dbg(motg->phy.dev, "Requested curr from USB = %u, max-type-c:%u\n",
					mA, motg->typec_current_max);
	
	motg->bc1p2_current_max = mA;

	
	if (motg->typec_current_max > 500 && mA < motg->typec_current_max)
		mA = motg->typec_current_max;

	if (msm_otg_notify_chg_type(motg))
		dev_err(motg->phy.dev,
			"Failed notifying %d charger type to PMIC\n",
							motg->chg_type);

	if (motg->online && motg->cur_power == 0 && mA == 0)
		msm_otg_set_online_status(motg);

	if (motg->cur_power == mA)
		return;
	
	if (mA == 500)
		motg->connect_type = CONNECT_TYPE_USB;
	

	printk(KERN_WARNING "[USB] Avail curr from USB = %u\n", mA);
	msm_otg_dbg_log_event(&motg->phy, "AVAIL CURR FROM USB",
			mA, motg->chg_type);

	if (msm_otg_notify_power_supply(motg, mA))
		pm8921_charger_vbus_draw(mA);

	motg->cur_power = mA;
}

static int msm_otg_set_power(struct usb_phy *phy, unsigned mA)
{
	struct msm_otg *motg = container_of(phy, struct msm_otg, phy);

	if (motg->chg_type == USB_SDP_CHARGER)
		msm_otg_notify_charger(motg, mA);

	return 0;
}

static void msm_otg_start_host(struct usb_otg *otg, int on)
{
	struct msm_otg *motg = container_of(otg->phy, struct msm_otg, phy);
	struct msm_otg_platform_data *pdata = motg->pdata;
	struct usb_hcd *hcd;
	u32 val;

	if (!otg->host)
		return;

	hcd = bus_to_hcd(otg->host);

	if (on) {
		dev_dbg(otg->phy->dev, "host on\n");
		msm_otg_dbg_log_event(&motg->phy, "HOST ON",
				motg->inputs, otg->phy->state);

		if (pdata->otg_control == OTG_PHY_CONTROL)
			ulpi_write(otg->phy, OTG_COMP_DISABLE,
				ULPI_SET(ULPI_PWR_CLK_MNG_REG));

		if (pdata->enable_axi_prefetch) {
			val = readl_relaxed(USB_HS_APF_CTRL);
			val &= ~APF_CTRL_EN;
			writel_relaxed(val, USB_HS_APF_CTRL);
		}
		usb_add_hcd(hcd, hcd->irq, IRQF_SHARED);
	} else {
		dev_dbg(otg->phy->dev, "host off\n");
		msm_otg_dbg_log_event(&motg->phy, "HOST OFF",
				motg->inputs, otg->phy->state);

		wake_up(&motg->host_suspend_wait);
		usb_remove_hcd(hcd);

		if (pdata->enable_axi_prefetch)
			writel_relaxed(readl_relaxed(USB_HS_APF_CTRL)
					| (APF_CTRL_EN), USB_HS_APF_CTRL);

		
		writel_relaxed(0x80000000, USB_PORTSC);

		if (pdata->otg_control == OTG_PHY_CONTROL)
			ulpi_write(otg->phy, OTG_COMP_DISABLE,
				ULPI_CLR(ULPI_PWR_CLK_MNG_REG));
	}
}

static int msm_otg_usbdev_notify(struct notifier_block *self,
			unsigned long action, void *priv)
{
	struct msm_otg *motg = container_of(self, struct msm_otg, usbdev_nb);
	struct usb_otg *otg = motg->phy.otg;
	struct usb_device *udev = priv;

	printk(KERN_WARNING "[USB]%s: action=[%lu]\n", __func__, action);
	if (action == USB_BUS_ADD || action == USB_BUS_REMOVE)
		goto out;

	if (udev->bus != otg->host)
		goto out;
	if (!udev->parent || udev->parent->parent ||
			motg->chg_type == USB_ACA_DOCK_CHARGER)
		goto out;

	switch (action) {
	case USB_DEVICE_ADD:
		if (aca_enabled())
			usb_disable_autosuspend(udev);
		if (otg->phy->state == OTG_STATE_A_WAIT_BCON) {
			printk(KERN_WARNING "B_CONN set\n");
			msm_otg_dbg_log_event(&motg->phy, "B_CONN SET",
					motg->inputs, otg->phy->state);
			set_bit(B_CONN, &motg->inputs);
			msm_otg_del_timer(motg);
			otg->phy->state = OTG_STATE_A_HOST;
			if (udev->quirks & USB_QUIRK_OTG_PET)
				msm_otg_start_timer(motg, TA_TST_MAINT,
						A_TST_MAINT);
		}
		
	case USB_DEVICE_CONFIG:
		if (udev->actconfig)
			motg->mA_port = udev->actconfig->desc.bMaxPower * 2;
		else
			motg->mA_port = IUNIT;
		if (otg->phy->state == OTG_STATE_B_HOST)
			msm_otg_del_timer(motg);
		break;
	case USB_DEVICE_REMOVE:
		if ((otg->phy->state == OTG_STATE_A_HOST) ||
			(otg->phy->state == OTG_STATE_A_SUSPEND)) {
			printk(KERN_WARNING "B_CONN clear\n");
			msm_otg_dbg_log_event(&motg->phy, "B_CONN CLEAR",
					motg->inputs, otg->phy->state);
			clear_bit(B_CONN, &motg->inputs);
			if (udev->bus->otg_vbus_off) {
				udev->bus->otg_vbus_off = 0;
				set_bit(A_BUS_DROP, &motg->inputs);
			}
			queue_work(motg->otg_wq, &motg->sm_work);
		}
	default:
		break;
	}
	if (test_bit(ID_A, &motg->inputs))
		msm_otg_notify_charger(motg, IDEV_ACA_CHG_MAX -
				motg->mA_port);
out:
	return NOTIFY_OK;
}

static void msm_hsusb_vbus_power(struct msm_otg *motg, bool on)
{
	int ret;
	static bool vbus_is_on;

	msm_otg_dbg_log_event(&motg->phy, "VBUS POWER", on, vbus_is_on);
	if (vbus_is_on == on)
		return;

	if (motg->pdata->vbus_power) {
		ret = motg->pdata->vbus_power(on);
		if (!ret)
			vbus_is_on = on;
		return;
	}

	if (!vbus_otg) {
		pr_err("vbus_otg is NULL.");
		return;
	}

	if (on) {
		msm_otg_notify_host_mode(motg, on);
		ret = regulator_enable(vbus_otg);
		if (ret) {
			pr_err("unable to enable vbus_otg\n");
			return;
		}
		vbus_is_on = true;
	} else {
		ret = regulator_disable(vbus_otg);
		if (ret) {
			pr_err("unable to disable vbus_otg\n");
			return;
		}
		msm_otg_notify_host_mode(motg, on);
		vbus_is_on = false;
	}
}

static int msm_otg_set_host(struct usb_otg *otg, struct usb_bus *host)
{
	struct msm_otg *motg = container_of(otg->phy, struct msm_otg, phy);
	struct usb_hcd *hcd;

	if (motg->pdata->mode == USB_PERIPHERAL) {
		printk(KERN_WARNING "[USB] Host mode is not supported\n");
		return -ENODEV;
	}

	if (!motg->pdata->vbus_power && host) {
		vbus_otg = devm_regulator_get(motg->phy.dev, "vbus_otg");
		if (IS_ERR(vbus_otg)) {
			msm_otg_dbg_log_event(&motg->phy,
					"UNABLE TO GET VBUS_OTG",
					otg->phy->state, 0);
			pr_err("Unable to get vbus_otg\n");
			return PTR_ERR(vbus_otg);
		}
	}

	if (!host) {
		USB_WARNING("%s: no host\n", __func__);
		if (otg->phy->state == OTG_STATE_A_HOST) {
			pm_runtime_get_sync(otg->phy->dev);
			usb_unregister_notify(&motg->usbdev_nb);
			msm_otg_start_host(otg, 0);
			msm_hsusb_vbus_power(motg, 0);
			otg->host = NULL;
			otg->phy->state = OTG_STATE_UNDEFINED;
			queue_work(motg->otg_wq, &motg->sm_work);
		} else {
			otg->host = NULL;
		}

		return 0;
	}

	hcd = bus_to_hcd(host);
	hcd->power_budget = motg->pdata->power_budget;

#ifdef CONFIG_USB_OTG
	host->otg_port = 1;
#endif
	motg->usbdev_nb.notifier_call = msm_otg_usbdev_notify;
	usb_register_notify(&motg->usbdev_nb);
	otg->host = host;
	dev_dbg(otg->phy->dev, "host driver registered w/ tranceiver\n");
	msm_otg_dbg_log_event(&motg->phy, "HOST DRIVER REGISTERED",
			hcd->power_budget, motg->pdata->mode);

	if (motg->pdata->mode == USB_HOST || otg->gadget) {
		msm_otg_dbg_log_event(&motg->phy, "PM RUNTIME: HOST GET",
				get_pm_runtime_counter(otg->phy->dev), 0);
		pm_runtime_get_sync(otg->phy->dev);
		queue_work(motg->otg_wq, &motg->sm_work);
	}

	return 0;
}

static void msm_otg_start_peripheral(struct usb_otg *otg, int on)
{
	struct msm_otg *motg = container_of(otg->phy, struct msm_otg, phy);
	struct msm_otg_platform_data *pdata = motg->pdata;
	struct pinctrl_state *set_state;
	int ret;

	if (!otg->gadget)
		return;

	if (on) {
		dev_dbg(otg->phy->dev, "gadget on\n");
		msm_otg_dbg_log_event(&motg->phy, "GADGET ON",
				motg->inputs, otg->phy->state);

		
		if (debug_bus_voting_enabled)
			msm_otg_bus_vote(motg, USB_MAX_PERF_VOTE);

		usb_gadget_vbus_connect(otg->gadget);

		if (pdata->vddmin_gpio) {
			if (motg->phy_pinctrl) {
				set_state =
					pinctrl_lookup_state(motg->phy_pinctrl,
							"hsusb_active");
				if (IS_ERR(set_state)) {
					pr_err("cannot get phy pinctrl active state\n");
					return;
				}
				pinctrl_select_state(motg->phy_pinctrl,
						set_state);
			}

			ret = gpio_request(pdata->vddmin_gpio,
					"MSM_OTG_VDD_MIN_GPIO");
			if (ret < 0) {
				dev_err(otg->phy->dev, "gpio req failed for vdd min:%d\n",
						ret);
				pdata->vddmin_gpio = 0;
			}
		}
	} else {
		dev_dbg(otg->phy->dev, "gadget off\n");
		msm_otg_dbg_log_event(&motg->phy, "GADGET OFF",
			motg->inputs, otg->phy->state);
		usb_gadget_vbus_disconnect(otg->gadget);
		
		msm_otg_bus_vote(motg, USB_MIN_PERF_VOTE);

		if (pdata->vddmin_gpio) {
			gpio_free(pdata->vddmin_gpio);
			if (motg->phy_pinctrl) {
				set_state =
					pinctrl_lookup_state(motg->phy_pinctrl,
							"hsusb_sleep");
				if (IS_ERR(set_state))
					pr_err("cannot get phy pinctrl sleep state\n");
				else
					pinctrl_select_state(motg->phy_pinctrl,
						set_state);
			}
		}
	}
}

static void msm_otg_notify_usb_disabled(void)
{
	struct msm_otg *motg = the_msm_otg;
	USB_INFO("%s\n", __func__);
	queue_work(motg->otg_wq, &motg->sm_work);
}


static int msm_otg_set_peripheral(struct usb_otg *otg,
					struct usb_gadget *gadget)
{
	struct msm_otg *motg = container_of(otg->phy, struct msm_otg, phy);

	if (motg->pdata->mode == USB_HOST) {
		printk(KERN_WARNING "[USB] Peripheral mode is not supported\n");
		return -ENODEV;
	}

	if (!gadget) {
		USB_WARNING("%s: no gadget\n", __func__);
		if (otg->phy->state == OTG_STATE_B_PERIPHERAL) {
			msm_otg_dbg_log_event(&motg->phy,
				"PM RUNTIME: PERIPHERAL GET1",
				get_pm_runtime_counter(otg->phy->dev), 0);
			pm_runtime_get_sync(otg->phy->dev);
			msm_otg_start_peripheral(otg, 0);
			otg->gadget = NULL;
			otg->phy->state = OTG_STATE_UNDEFINED;
			queue_work(motg->otg_wq, &motg->sm_work);
		} else {
			otg->gadget = NULL;
		}

		return 0;
	}
	otg->gadget = gadget;
	dev_dbg(otg->phy->dev, "peripheral driver registered w/ tranceiver\n");
	msm_otg_dbg_log_event(&motg->phy, "PERIPHERAL DRIVER REGISTERED",
			otg->phy->state, motg->pdata->mode);

	if (motg->pdata->mode == USB_PERIPHERAL || otg->host) {
		USB_WARNING("peripheral only, otg->host exist\n");
		msm_otg_dbg_log_event(&motg->phy, "PM RUNTIME: PERIPHERAL GET2",
				get_pm_runtime_counter(otg->phy->dev), 0);
		pm_runtime_get_sync(otg->phy->dev);
		queue_work(motg->otg_wq, &motg->sm_work);
	}

	return 0;
}

static bool msm_otg_read_pmic_id_state(struct msm_otg *motg)
{
	unsigned long flags;
	int id;

	if (!motg->pdata->pmic_id_irq)
		return -ENODEV;

	local_irq_save(flags);
	id = irq_read_line(motg->pdata->pmic_id_irq);
	local_irq_restore(flags);

	return !!id;
}

static bool msm_otg_read_phy_id_state(struct msm_otg *motg)
{
	u8 val;

	writeb_relaxed(USB_PHY_ID_MASK, USB2_PHY_USB_PHY_INTERRUPT_CLEAR1);

	writeb_relaxed(0x1, USB2_PHY_USB_PHY_IRQ_CMD);
	udelay(200);
	writeb_relaxed(0x0, USB2_PHY_USB_PHY_IRQ_CMD);

	val = readb_relaxed(USB2_PHY_USB_PHY_INTERRUPT_SRC_STATUS);
	if (val & USB_PHY_IDDIG_1_0)
		return false; 
	else
		return true;
}

static int msm_otg_mhl_register_callback(struct msm_otg *motg,
						void (*callback)(int on))
{
	struct usb_phy *phy = &motg->phy;
	int ret;

	if (!motg->pdata->mhl_enable) {
		dev_dbg(phy->dev, "MHL feature not enabled\n");
		return -ENODEV;
	}

	if (motg->pdata->otg_control != OTG_PMIC_CONTROL ||
			!motg->pdata->pmic_id_irq) {
		dev_dbg(phy->dev, "MHL can not be supported without PMIC Id\n");
		return -ENODEV;
	}

	if (!motg->pdata->mhl_dev_name) {
		dev_dbg(phy->dev, "MHL device name does not exist.\n");
		return -ENODEV;
	}

	if (callback)
		ret = mhl_register_callback(motg->pdata->mhl_dev_name,
								callback);
	else
		ret = mhl_unregister_callback(motg->pdata->mhl_dev_name);

	if (ret)
		dev_dbg(phy->dev, "mhl_register_callback(%s) return error=%d\n",
						motg->pdata->mhl_dev_name, ret);
	else
		motg->mhl_enabled = true;

	return ret;
}

static void msm_otg_mhl_notify_online(int on)
{
	struct msm_otg *motg = the_msm_otg;
	struct usb_phy *phy = &motg->phy;
	bool queue = false;

	dev_dbg(phy->dev, "notify MHL %s%s\n", on ? "" : "dis", "connected");

	if (on) {
		set_bit(MHL, &motg->inputs);
	} else {
		clear_bit(MHL, &motg->inputs);
		queue = true;
	}

	if (queue && phy->state != OTG_STATE_UNDEFINED)
		schedule_work(&motg->sm_work);
}

static bool msm_otg_is_mhl(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;
	int is_mhl, ret;

	ret = mhl_device_discovery(motg->pdata->mhl_dev_name, &is_mhl);
	if (ret || is_mhl != MHL_DISCOVERY_RESULT_MHL) {
		clear_bit(MHL, &motg->inputs);
		dev_dbg(phy->dev, "MHL device not found\n");
		return false;
	}

	set_bit(MHL, &motg->inputs);
	dev_dbg(phy->dev, "MHL device found\n");
	return true;
}

static bool msm_chg_mhl_detect(struct msm_otg *motg)
{
	bool ret, id;

	if (!motg->mhl_enabled)
		return false;

	id = msm_otg_read_pmic_id_state(motg);

	if (id)
		return false;

	mhl_det_in_progress = true;
	ret = msm_otg_is_mhl(motg);
	mhl_det_in_progress = false;

	return ret;
}
#define RETRY_CHECK_TIMES 5
extern void usb_set_connect_type(int);
static void msm_otg_chg_check_timer_func(unsigned long data)
{
	struct msm_otg *motg = (struct msm_otg *) data;
	struct usb_otg *otg = motg->phy.otg;

	if (atomic_read(&motg->in_lpm) ||
		!test_bit(B_SESS_VLD, &motg->inputs) ||
		otg->phy->state != OTG_STATE_B_PERIPHERAL ||
		otg->gadget->speed != USB_SPEED_UNKNOWN) {
		USB_INFO("%s: Nothing to do in chg_check_timer. atomic_read(&motg->in_lpm):%d, test_bit(B_SESS_VLD, &motg->inputs):%d, otg->phy->state:%s, otg->gadget->speed:%d, \n",
			__func__, atomic_read(&motg->in_lpm), test_bit(B_SESS_VLD, &motg->inputs), usb_otg_state_string(otg->phy->state), otg->gadget->speed);
		return;
	}

	USB_INFO("%s: count = %d, connect_type = %d, line_state = %x\n",
				__func__, motg->chg_check_count, motg->connect_type,
				(readl_relaxed(USB_PORTSC) & PORTSC_LS));

	if ((readl_relaxed(USB_PORTSC) & PORTSC_LS) == PORTSC_LS) {
		USB_INFO("DCP is detected as SDP, change type to DCP\n");
		msm_otg_dbg_log_event(&motg->phy, "DCP IS DETECTED AS SDP",
				otg->phy->state, 0);
		set_bit(B_FALSE_SDP, &motg->inputs);
		queue_work(motg->otg_wq, &motg->sm_work);
	
		motg->connect_type = CONNECT_TYPE_AC;
		usb_set_connect_type(CONNECT_TYPE_AC);
	} else if (motg->chg_check_count < RETRY_CHECK_TIMES) {
		motg->chg_check_count++;
		mod_timer(&motg->chg_check_timer, CHG_RECHECK_DELAY);
	} else {
		USB_INFO("Unknown charging is detected\n");
		motg->connect_type = CONNECT_TYPE_UNKNOWN;
		usb_set_connect_type(CONNECT_TYPE_UNKNOWN);
	}
	
}

static bool msm_chg_aca_detect(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;
	u32 int_sts;
	bool ret = false;

	if (!aca_enabled())
		goto out;

	int_sts = ulpi_read(phy, 0x87);
	switch (int_sts & 0x1C) {
	case 0x08:
		if (!test_and_set_bit(ID_A, &motg->inputs)) {
			dev_dbg(phy->dev, "ID_A\n");
			motg->chg_type = USB_ACA_A_CHARGER;
			motg->chg_state = USB_CHG_STATE_DETECTED;
			clear_bit(ID_B, &motg->inputs);
			clear_bit(ID_C, &motg->inputs);
			set_bit(ID, &motg->inputs);
			ret = true;
		}
		break;
	case 0x0C:
		if (!test_and_set_bit(ID_B, &motg->inputs)) {
			dev_dbg(phy->dev, "ID_B\n");
			motg->chg_type = USB_ACA_B_CHARGER;
			motg->chg_state = USB_CHG_STATE_DETECTED;
			clear_bit(ID_A, &motg->inputs);
			clear_bit(ID_C, &motg->inputs);
			set_bit(ID, &motg->inputs);
			ret = true;
		}
		break;
	case 0x10:
		if (!test_and_set_bit(ID_C, &motg->inputs)) {
			dev_dbg(phy->dev, "ID_C\n");
			motg->chg_type = USB_ACA_C_CHARGER;
			motg->chg_state = USB_CHG_STATE_DETECTED;
			clear_bit(ID_A, &motg->inputs);
			clear_bit(ID_B, &motg->inputs);
			set_bit(ID, &motg->inputs);
			ret = true;
		}
		break;
	case 0x04:
		if (test_and_clear_bit(ID, &motg->inputs)) {
			dev_dbg(phy->dev, "ID_GND\n");
			motg->chg_type = USB_INVALID_CHARGER;
			motg->chg_state = USB_CHG_STATE_UNDEFINED;
			clear_bit(ID_A, &motg->inputs);
			clear_bit(ID_B, &motg->inputs);
			clear_bit(ID_C, &motg->inputs);
			ret = true;
		}
		break;
	default:
		ret = test_and_clear_bit(ID_A, &motg->inputs) |
			test_and_clear_bit(ID_B, &motg->inputs) |
			test_and_clear_bit(ID_C, &motg->inputs) |
			!test_and_set_bit(ID, &motg->inputs);
		if (ret) {
			dev_dbg(phy->dev, "ID A/B/C/GND is no more\n");
			motg->chg_type = USB_INVALID_CHARGER;
			motg->chg_state = USB_CHG_STATE_UNDEFINED;
		}
	}
out:
	return ret;
}

static void msm_chg_enable_aca_det(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;

	if (!aca_enabled())
		return;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
		
		writel_relaxed(readl_relaxed(USB_OTGSC) & ~(OTGSC_IDPU |
				OTGSC_IDIE), USB_OTGSC);
		ulpi_write(phy, 0x01, 0x0C);
		ulpi_write(phy, 0x10, 0x0F);
		ulpi_write(phy, 0x10, 0x12);
		
		pm8xxx_usb_id_pullup(0);
		
		ulpi_write(phy, 0x20, 0x85);
		aca_id_turned_on = true;
		break;
	default:
		break;
	}
}

static void msm_chg_enable_aca_intr(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;

	if (!aca_enabled())
		return;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
		
		ulpi_write(phy, 0x01, 0x94);
		break;
	default:
		break;
	}
}

static void msm_chg_disable_aca_intr(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;

	if (!aca_enabled())
		return;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
		ulpi_write(phy, 0x01, 0x95);
		break;
	default:
		break;
	}
}

static bool msm_chg_check_aca_intr(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;
	bool ret = false;

	if (!aca_enabled())
		return ret;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
		if (ulpi_read(phy, 0x91) & 1) {
			dev_dbg(phy->dev, "RID change\n");
			ulpi_write(phy, 0x01, 0x92);
			ret = msm_chg_aca_detect(motg);
		}
	default:
		break;
	}
	return ret;
}

static void msm_otg_id_timer_func(unsigned long data)
{
	struct msm_otg *motg = (struct msm_otg *) data;

	if (!aca_enabled())
		return;

	if (atomic_read(&motg->in_lpm)) {
		dev_dbg(motg->phy.dev, "timer: in lpm\n");
		msm_otg_dbg_log_event(&motg->phy, "ID TIMER: IN LPM",
				motg->phy.state, 0);
		return;
	}

	if (motg->phy.state == OTG_STATE_A_SUSPEND)
		goto out;

	if (msm_chg_check_aca_intr(motg)) {
		dev_dbg(motg->phy.dev, "timer: aca work\n");
		queue_work(motg->otg_wq, &motg->sm_work);
	}

out:
	if (!test_bit(ID, &motg->inputs) || test_bit(ID_A, &motg->inputs))
		mod_timer(&motg->id_timer, ID_TIMER_FREQ);
}

static bool msm_chg_check_secondary_det(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;
	u32 chg_det;
	bool ret = false;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
	case SNPS_FEMTO_PHY:
		chg_det = ulpi_read(phy, 0x87);
		ret = chg_det & 1;
		break;
	default:
		break;
	}
	return ret;
}

static void msm_chg_enable_secondary_det(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
	case SNPS_FEMTO_PHY:
		ulpi_write(phy, 0x8, 0x85);
		ulpi_write(phy, 0x2, 0x85);
		ulpi_write(phy, 0x1, 0x85);
		break;
	default:
		break;
	}
}

static bool msm_chg_check_primary_det(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;
	u32 chg_det;
	bool ret = false;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
	case SNPS_FEMTO_PHY:
		chg_det = ulpi_read(phy, 0x87);
		ret = chg_det & 1;
		
		ulpi_write(phy, 0x3, 0x86);
		msleep(20);
		break;
	default:
		break;
	}
	return ret;
}

static void msm_chg_enable_primary_det(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
	case SNPS_FEMTO_PHY:
		ulpi_write(phy, 0x2, 0x85);
		ulpi_write(phy, 0x1, 0x85);
		break;
	default:
		break;
	}
}

static bool msm_chg_check_dcd(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;
	u32 line_state;
	bool ret = false;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
	case SNPS_FEMTO_PHY:
		line_state = ulpi_read(phy, 0x87);
		ret = line_state & 2;
		break;
	default:
		break;
	}
	return ret;
}

static void msm_chg_disable_dcd(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
		ulpi_write(phy, 0x10, 0x86);
		break;
	case SNPS_FEMTO_PHY:
		ulpi_write(phy, 0x10, 0x86);
		ulpi_write(phy, 0x04, 0x0C);
		break;
	default:
		break;
	}
}

static void msm_chg_enable_dcd(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
		
		ulpi_write(phy, 0x10, 0x85);
		break;
	case SNPS_FEMTO_PHY:
		ulpi_write(phy, 0x10, 0x85);
		ulpi_write(phy, 0x04, 0x0B);
		break;
	default:
		break;
	}
}

static void msm_chg_block_on(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;
	u32 func_ctrl;

	
	func_ctrl = ulpi_read(phy, ULPI_FUNC_CTRL);
	func_ctrl &= ~ULPI_FUNC_CTRL_OPMODE_MASK;
	func_ctrl |= ULPI_FUNC_CTRL_OPMODE_NONDRIVING;
	ulpi_write(phy, func_ctrl, ULPI_FUNC_CTRL);

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
	case SNPS_FEMTO_PHY:
		
		ulpi_write(phy, 0x6, 0xC);
		
		ulpi_write(phy, 0x1F, 0x86);
		
		ulpi_write(phy, 0x1F, 0x92);
		ulpi_write(phy, 0x1F, 0x95);
		udelay(100);
		break;
	default:
		break;
	}
}

static void msm_chg_block_off(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;
	u32 func_ctrl;

	switch (motg->pdata->phy_type) {
	case SNPS_PICO_PHY:
	case SNPS_FEMTO_PHY:
		
		ulpi_write(phy, 0x3F, 0x86);
		
		ulpi_write(phy, 0x1F, 0x92);
		ulpi_write(phy, 0x1F, 0x95);
		
		ulpi_write(phy, 0x6, 0xB);
		break;
	default:
		break;
	}

	
	func_ctrl = ulpi_read(phy, ULPI_FUNC_CTRL);
	func_ctrl &= ~ULPI_FUNC_CTRL_OPMODE_MASK;
	func_ctrl |= ULPI_FUNC_CTRL_OPMODE_NORMAL;
	ulpi_write(phy, func_ctrl, ULPI_FUNC_CTRL);
}

static const char *chg_to_string(enum usb_chg_type chg_type)
{
	switch (chg_type) {
	case USB_SDP_CHARGER:		return "USB_SDP_CHARGER";
	case USB_DCP_CHARGER:		return "USB_DCP_CHARGER";
	case USB_CDP_CHARGER:		return "USB_CDP_CHARGER";
	case USB_ACA_A_CHARGER:		return "USB_ACA_A_CHARGER";
	case USB_ACA_B_CHARGER:		return "USB_ACA_B_CHARGER";
	case USB_ACA_C_CHARGER:		return "USB_ACA_C_CHARGER";
	case USB_ACA_DOCK_CHARGER:	return "USB_ACA_DOCK_CHARGER";
	case USB_PROPRIETARY_CHARGER:	return "USB_PROPRIETARY_CHARGER";
	case USB_FLOATED_CHARGER:	return "USB_FLOATED_CHARGER";
	default:			return "INVALID_CHARGER";
	}
}

#define MSM_CHG_DCD_TIMEOUT		(750 * HZ/1000) 
#define MSM_CHG_DCD_POLL_TIME		(50 * HZ/1000) 
#define MSM_CHG_PRIMARY_DET_TIME	(50 * HZ/1000) 
#define MSM_CHG_SECONDARY_DET_TIME	(50 * HZ/1000) 
static void msm_chg_detect_work(struct work_struct *w)
{
	struct msm_otg *motg = container_of(w, struct msm_otg, chg_work.work);
	struct usb_phy *phy = &motg->phy;
	bool is_dcd = false, tmout, vout, is_aca;
	static bool dcd;
	u32 line_state, dm_vlgc;
	unsigned long delay;

	dev_dbg(phy->dev, "chg detection work\n");
	msm_otg_dbg_log_event(phy, "CHG DETECTION WORK",
			motg->chg_state, phy->state);

	if (test_bit(MHL, &motg->inputs)) {
		dev_dbg(phy->dev, "detected MHL, escape chg detection work\n");
		return;
	}

	
	pm_runtime_resume(phy->dev);
	switch (motg->chg_state) {
	case USB_CHG_STATE_UNDEFINED:
		msm_chg_block_on(motg);
		msm_chg_enable_dcd(motg);
		msm_chg_enable_aca_det(motg);
		motg->chg_state = USB_CHG_STATE_WAIT_FOR_DCD;
		motg->dcd_time = 0;
		delay = MSM_CHG_DCD_POLL_TIME;
		break;
	case USB_CHG_STATE_WAIT_FOR_DCD:
		if (msm_chg_mhl_detect(motg)) {
			msm_chg_block_off(motg);
			motg->chg_state = USB_CHG_STATE_DETECTED;
			motg->chg_type = USB_INVALID_CHARGER;
			queue_work(motg->otg_wq, &motg->sm_work);
			return;
		}
		is_aca = msm_chg_aca_detect(motg);
		if (is_aca) {
			if (test_bit(ID_A, &motg->inputs)) {
				motg->chg_state = USB_CHG_STATE_WAIT_FOR_DCD;
			} else {
				delay = 0;
				break;
			}
		}
		is_dcd = msm_chg_check_dcd(motg);
		motg->dcd_time += MSM_CHG_DCD_POLL_TIME;
		tmout = motg->dcd_time >= MSM_CHG_DCD_TIMEOUT;
		if (is_dcd || tmout) {
			if (is_dcd)
				dcd = true;
			else
				dcd = false;
			msm_chg_disable_dcd(motg);
			msm_chg_enable_primary_det(motg);
			delay = MSM_CHG_PRIMARY_DET_TIME;
			motg->chg_state = USB_CHG_STATE_DCD_DONE;
		} else {
			delay = MSM_CHG_DCD_POLL_TIME;
		}
		break;
	case USB_CHG_STATE_DCD_DONE:
		vout = msm_chg_check_primary_det(motg);
		line_state = readl_relaxed(USB_PORTSC) & PORTSC_LS;
		dm_vlgc = line_state & PORTSC_LS_DM;
		if (vout && !dm_vlgc) { 
			if (test_bit(ID_A, &motg->inputs)) {
				motg->chg_type = USB_ACA_DOCK_CHARGER;
				motg->chg_state = USB_CHG_STATE_DETECTED;
				motg->connect_type = CONNECT_TYPE_UNKNOWN;
				delay = 0;
				break;
			}
			if (line_state) { 
				motg->chg_type = USB_PROPRIETARY_CHARGER;
				motg->chg_state = USB_CHG_STATE_DETECTED;
				motg->connect_type = CONNECT_TYPE_UNKNOWN;
				delay = 0;
			} else {
				msm_chg_enable_secondary_det(motg);
				delay = MSM_CHG_SECONDARY_DET_TIME;
				motg->chg_state = USB_CHG_STATE_PRIMARY_DONE;
			}
		} else { 
			if (test_bit(ID_A, &motg->inputs)) {
				motg->chg_type = USB_ACA_A_CHARGER;
				motg->chg_state = USB_CHG_STATE_DETECTED;
				motg->connect_type = CONNECT_TYPE_UNKNOWN;
				delay = 0;
				break;
			}
			if (line_state) {
				motg->chg_type = USB_PROPRIETARY_CHARGER;
				motg->connect_type = CONNECT_TYPE_AC;
			} else if (!dcd && floated_charger_enable) {
				motg->chg_type = USB_FLOATED_CHARGER;
				motg->connect_type = CONNECT_TYPE_UNKNOWN;
			} else {
				motg->chg_type = USB_SDP_CHARGER;
				motg->connect_type = CONNECT_TYPE_UNKNOWN;
			}

			motg->chg_state = USB_CHG_STATE_DETECTED;
			delay = 0;
		}
		break;
	case USB_CHG_STATE_PRIMARY_DONE:
		vout = msm_chg_check_secondary_det(motg);
		if (vout)
			motg->chg_type = USB_DCP_CHARGER;
		else
			motg->chg_type = USB_CDP_CHARGER;
		motg->connect_type = CONNECT_TYPE_AC;
		motg->chg_state = USB_CHG_STATE_SECONDARY_DONE;
		
	case USB_CHG_STATE_SECONDARY_DONE:
		motg->chg_state = USB_CHG_STATE_DETECTED;
	case USB_CHG_STATE_DETECTED:
		if (motg->chg_type == USB_DCP_CHARGER &&
			motg->ext_chg_opened) {
				init_completion(&motg->ext_chg_wait);
				motg->ext_chg_active = DEFAULT;
		}
		msm_otg_notify_chg_type(motg);
		msm_chg_block_off(motg);
		msm_chg_enable_aca_det(motg);
		if (aca_enabled())
			udelay(100);
		msm_chg_enable_aca_intr(motg);

		
		if (motg->chg_type == USB_DCP_CHARGER)
			ulpi_write(phy, 0x2, 0x85);

		USB_INFO("chg_type = %s\n",
			chg_to_string(motg->chg_type));
		msm_otg_dbg_log_event(phy, "CHG WORK: CHG_TYPE",
				motg->chg_type, motg->inputs);
		queue_work(motg->otg_wq, &motg->sm_work);
		return;
	default:
		return;
	}

	msm_otg_dbg_log_event(phy, "CHG WORK: QUEUE", motg->chg_type, delay);
	queue_delayed_work(motg->otg_wq, &motg->chg_work, delay);
}

#define VBUS_INIT_TIMEOUT	msecs_to_jiffies(5000)

static void msm_otg_init_sm(struct msm_otg *motg)
{
	struct msm_otg_platform_data *pdata = motg->pdata;
	u32 otgsc = readl(USB_OTGSC);
	int ret;
	printk(KERN_WARNING "[USB] init_sm %d %d\n",pdata->mode,pdata->otg_control);
	switch (pdata->mode) {
	case USB_OTG:
		if (pdata->otg_control == OTG_USER_CONTROL) {
			if (pdata->default_mode == USB_HOST) {
				clear_bit(ID, &motg->inputs);
			} else if (pdata->default_mode == USB_PERIPHERAL) {
				set_bit(ID, &motg->inputs);
				set_bit(B_SESS_VLD, &motg->inputs);
			} else {
				set_bit(ID, &motg->inputs);
				clear_bit(B_SESS_VLD, &motg->inputs);
			}
		} else if (pdata->otg_control == OTG_PHY_CONTROL) {
			if (otgsc & OTGSC_ID) {
				set_bit(ID, &motg->inputs);
			} else {
				clear_bit(ID, &motg->inputs);
				set_bit(A_BUS_REQ, &motg->inputs);
			}
			if (otgsc & OTGSC_BSV)
				set_bit(B_SESS_VLD, &motg->inputs);
			else
				clear_bit(B_SESS_VLD, &motg->inputs);
		} else if (pdata->otg_control == OTG_PMIC_CONTROL) {
			if (pdata->pmic_id_irq) {
				if (msm_otg_read_pmic_id_state(motg))
					set_bit(ID, &motg->inputs);
				else
					clear_bit(ID, &motg->inputs);
			} else if (motg->ext_id_irq) {
				if (gpio_get_value(pdata->usb_id_gpio))
					set_bit(ID, &motg->inputs);
				else
					clear_bit(ID, &motg->inputs);
			} else if (motg->phy_irq) {
				if (msm_otg_read_phy_id_state(motg))
					set_bit(ID, &motg->inputs);
				else
					clear_bit(ID, &motg->inputs);
			}
			ret = wait_for_completion_timeout(&pmic_vbus_init,
							  VBUS_INIT_TIMEOUT);
			if (!ret) {
				dev_dbg(motg->phy.dev, "%s: timeout waiting for PMIC VBUS\n",
					__func__);
				msm_otg_dbg_log_event(&motg->phy,
						"PMIC VBUS WAIT TMOUT",
						motg->inputs, motg->phy.state);
				clear_bit(B_SESS_VLD, &motg->inputs);
				pmic_vbus_init.done = 1;
			}
		}
		break;
	case USB_HOST:
		clear_bit(ID, &motg->inputs);
		break;
	case USB_PERIPHERAL:
		set_bit(ID, &motg->inputs);
		if (pdata->otg_control == OTG_PHY_CONTROL) {
			if (otgsc & OTGSC_BSV)
				set_bit(B_SESS_VLD, &motg->inputs);
			else
				clear_bit(B_SESS_VLD, &motg->inputs);
		} else if (pdata->otg_control == OTG_PMIC_CONTROL) {
			ret = wait_for_completion_timeout(&pmic_vbus_init,
							  VBUS_INIT_TIMEOUT);
			if (!ret) {
				dev_dbg(motg->phy.dev, "%s: timeout waiting for PMIC VBUS\n",
					__func__);
				msm_otg_dbg_log_event(&motg->phy,
						"PMIC VBUS WAIT TMOUT",
						motg->inputs, motg->phy.state);
				clear_bit(B_SESS_VLD, &motg->inputs);
				pmic_vbus_init.done = 1;
			}
		} else if (pdata->otg_control == OTG_USER_CONTROL) {
			set_bit(ID, &motg->inputs);
			set_bit(B_SESS_VLD, &motg->inputs);
		}
		break;
	default:
		break;
	msm_otg_dbg_log_event(&motg->phy, "SM INIT", pdata->mode, motg->inputs);
	}
	motg->id_state = (test_bit(ID, &motg->inputs)) ? USB_ID_FLOAT :
							USB_ID_GROUND;
}

static void msm_otg_wait_for_ext_chg_done(struct msm_otg *motg)
{
	struct usb_phy *phy = &motg->phy;
	unsigned long t;


	if (motg->ext_chg_active == ACTIVE) {

do_wait:
		printk(KERN_WARNING "before msm_otg ext chg wait\n");
		msm_otg_dbg_log_event(&motg->phy, "EXT CHG: WAIT", 0, 0);

		t = wait_for_completion_timeout(&motg->ext_chg_wait,
				msecs_to_jiffies(3000));
		msm_otg_dbg_log_event(&motg->phy, "EXT CHG: DONE", t, 0);

		if (!t)
			pr_err("msm_otg ext chg wait timeout\n");
		else if (motg->ext_chg_active == ACTIVE)
			goto do_wait;
		else
			printk(KERN_WARNING "msm_otg ext chg wait done\n");
	}

	if (motg->ext_chg_opened) {
		if (phy->flags & ENABLE_DP_MANUAL_PULLUP) {
			ulpi_write(phy, ULPI_MISC_A_VBUSVLDEXT |
					ULPI_MISC_A_VBUSVLDEXTSEL,
					ULPI_CLR(ULPI_MISC_A));
		}
		
		ulpi_write(phy, 0x3F, 0x86);
		
		ulpi_write(phy, 0x6, 0xB);
	}
}

static void msm_otg_sm_work(struct work_struct *w)
{
	struct msm_otg *motg = container_of(w, struct msm_otg, sm_work);
	struct usb_otg *otg = motg->phy.otg;
	bool work = 0, srp_reqd, dcp;

	USB_INFO("%s begining work\n", usb_otg_state_string(otg->phy->state));
	pm_runtime_resume(otg->phy->dev);
	if (motg->pm_done) {
		msm_otg_dbg_log_event(&motg->phy, "PM RUNTIME: USBCONN GET",
			get_pm_runtime_counter(otg->phy->dev), motg->pm_done);
		pm_runtime_get_sync(otg->phy->dev);
		motg->pm_done = 0;
	}
	USB_INFO("%s work, motg->inputs=[0x%lx], otg->host=[0x%llx] \n", usb_otg_state_string(otg->phy->state), motg->inputs, (unsigned long long)otg->host);
	msm_otg_dbg_log_event(&motg->phy, "SM WORK:",
			otg->phy->state, motg->inputs);
	switch (otg->phy->state) {
	case OTG_STATE_UNDEFINED:
		msm_otg_reset(otg->phy);
		msm_bam_set_usb_dev(otg->phy->dev);
		msm_otg_init_sm(motg);
		if (!psy && legacy_power_supply) {
			psy = power_supply_get_by_name("usb");

			if (!psy)
				pr_err("couldn't get usb power supply\n");
		}

		otg->phy->state = OTG_STATE_B_IDLE;
		if (!test_bit(B_SESS_VLD, &motg->inputs) &&
				test_bit(ID, &motg->inputs)) {
			msm_otg_dbg_log_event(&motg->phy,
				"PM RUNTIME: UNDEF PUT",
				get_pm_runtime_counter(otg->phy->dev), 0);
			pm_runtime_put_noidle(otg->phy->dev);
			pm_runtime_suspend(otg->phy->dev);
			break;
		}
		
	case OTG_STATE_B_IDLE:
		if (test_bit(MHL, &motg->inputs)) {
			
			msm_otg_dbg_log_event(&motg->phy, "PM RUNTIME: MHL PUT",
				get_pm_runtime_counter(otg->phy->dev), 0);
			pm_runtime_put_noidle(otg->phy->dev);
			pm_runtime_suspend(otg->phy->dev);
		} else if ((!test_bit(ID, &motg->inputs) ||
				test_bit(ID_A, &motg->inputs)) && otg->host) {
			printk(KERN_WARNING "!id || id_A\n");
			msm_otg_dbg_log_event(&motg->phy, "!ID || ID_A",
					motg->inputs, otg->phy->state);
			if (msm_chg_mhl_detect(motg)) {
				work = 1;
				break;
			}
			clear_bit(B_BUS_REQ, &motg->inputs);
			set_bit(A_BUS_REQ, &motg->inputs);
			otg->phy->state = OTG_STATE_A_IDLE;
			work = 1;
		} else if (test_bit(B_SESS_VLD, &motg->inputs)) {
			printk(KERN_WARNING "b_sess_vld\n");
			msm_otg_dbg_log_event(&motg->phy, "B_SESS_VLD",
					motg->inputs, otg->phy->state);
			switch (motg->chg_state) {
			case USB_CHG_STATE_UNDEFINED:
				msm_chg_detect_work(&motg->chg_work.work);
				break;
			case USB_CHG_STATE_DETECTED:
				switch (motg->chg_type) {
				case USB_DCP_CHARGER:
					
				case USB_PROPRIETARY_CHARGER:
					msm_otg_notify_charger(motg,
							dcp_max_current);
					if (!motg->is_ext_chg_dcp)
						otg->phy->state =
							OTG_STATE_B_CHARGER;
					work = 0;
					msm_otg_dbg_log_event(&motg->phy,
					"PM RUNTIME: PROPCHG PUT",
					get_pm_runtime_counter(otg->phy->dev),
					0);
					pm_runtime_put_sync(otg->phy->dev);
					break;
				case USB_FLOATED_CHARGER:
					msm_otg_notify_charger(motg,
							IDEV_CHG_MAX);
					otg->phy->state =
						OTG_STATE_B_CHARGER;
					work = 0;
					msm_otg_dbg_log_event(&motg->phy,
					"PM RUNTIME: FLCHG PUT",
					get_pm_runtime_counter(otg->phy->dev),
					0);
					pm_runtime_put_noidle(otg->phy->dev);
					pm_runtime_suspend(otg->phy->dev);
					break;
				case USB_ACA_B_CHARGER:
					msm_otg_notify_charger(motg,
							IDEV_ACA_CHG_MAX);
					break;
				case USB_CDP_CHARGER:
					msm_otg_notify_charger(motg,
							IDEV_CHG_MAX);
					msm_otg_start_peripheral(otg, 1);
					otg->phy->state =
						OTG_STATE_B_PERIPHERAL;
					break;
				case USB_ACA_C_CHARGER:
					msm_otg_notify_charger(motg,
							IDEV_ACA_CHG_MAX);
					msm_otg_start_peripheral(otg, 1);
					otg->phy->state =
						OTG_STATE_B_PERIPHERAL;
					break;
				case USB_SDP_CHARGER:
					msm_otg_start_peripheral(otg, 1);
					otg->phy->state =
						OTG_STATE_B_PERIPHERAL;
					motg->chg_check_count = 0; 
					mod_timer(&motg->chg_check_timer,
							CHG_RECHECK_DELAY);
					break;
				default:
					break;
				}
				USB_INFO("b_sess_vld, chg_state %d chg_type %d usb_disable %d\n",motg->chg_state ,motg->chg_type, msm_otg_usb_disable);
				break;
			default:
				break;
			}
		} else if (test_bit(B_BUS_REQ, &motg->inputs)) {
			printk(KERN_WARNING "b_sess_end && b_bus_req\n");
			msm_otg_dbg_log_event(&motg->phy,
				"B_SESS_END && B_BUS_REQ",
				motg->inputs, otg->phy->state);
			if (msm_otg_start_srp(otg) < 0) {
				clear_bit(B_BUS_REQ, &motg->inputs);
				work = 1;
				break;
			}
			otg->phy->state = OTG_STATE_B_SRP_INIT;
			msm_otg_start_timer(motg, TB_SRP_FAIL, B_SRP_FAIL);
			break;
		} else {
			printk(KERN_WARNING "chg_work cancel");
			USB_INFO("!b_sess_vld && id\n");
			msm_otg_dbg_log_event(&motg->phy, "CHG_WORK CANCEL",
					motg->inputs, otg->phy->state);
			del_timer_sync(&motg->chg_check_timer);
			clear_bit(B_FALSE_SDP, &motg->inputs);
			clear_bit(A_BUS_REQ, &motg->inputs);
			cancel_delayed_work_sync(&motg->chg_work);
			dcp = (motg->chg_type == USB_DCP_CHARGER);
			motg->chg_state = USB_CHG_STATE_UNDEFINED;
			motg->chg_type = USB_INVALID_CHARGER;
			msm_otg_notify_charger(motg, 0);
			if (dcp) {
				if (motg->ext_chg_active == DEFAULT)
					motg->ext_chg_active = INACTIVE;
				msm_otg_wait_for_ext_chg_done(motg);
				
				ulpi_write(otg->phy, 0x2, 0x86);
			}
			msm_chg_block_off(motg);
			if (motg->connect_type != CONNECT_TYPE_NONE) {
				motg->connect_type = CONNECT_TYPE_NONE;
				
			}
			if ((motg->pdata->otg_control == OTG_PMIC_CONTROL) &&
					!msm_otg_read_pmic_id_state(motg)) {
				printk(KERN_WARNING "process missed ID intr\n");
				msm_otg_dbg_log_event(&motg->phy,
						"PROCESS MISSED ID INTR",
						motg->inputs, otg->phy->state);
				clear_bit(ID, &motg->inputs);
				work = 1;
				break;
			}
			msm_otg_dbg_log_event(&motg->phy,
					"PM RUNTIME: NOCHG PUT",
					get_pm_runtime_counter(otg->phy->dev),
					motg->pm_done);
			pm_runtime_put_noidle(otg->phy->dev);
			pm_runtime_mark_last_busy(otg->phy->dev);
			pm_runtime_autosuspend(otg->phy->dev);
			motg->pm_done = 1;
			msm_otg_dbg_log_event(&motg->phy,
					"PM RUNTIME: NOCHG PUT DONE",
					get_pm_runtime_counter(otg->phy->dev),
					motg->pm_done);
		}
		break;
	case OTG_STATE_B_SRP_INIT:
		if (!test_bit(ID, &motg->inputs) ||
				test_bit(ID_A, &motg->inputs) ||
				test_bit(ID_C, &motg->inputs) ||
				(test_bit(B_SESS_VLD, &motg->inputs) &&
				!test_bit(ID_B, &motg->inputs))) {
			printk(KERN_WARNING "!id || id_a/c || b_sess_vld+!id_b\n");
			msm_otg_dbg_log_event(&motg->phy,
					"!ID || ID_A/C || B_SESS_VLD+!ID_B",
					motg->inputs, otg->phy->state);
			msm_otg_del_timer(motg);
			otg->phy->state = OTG_STATE_B_IDLE;
			ulpi_write(otg->phy, 0x0, 0x98);
			work = 1;
		} else if (test_bit(B_SRP_FAIL, &motg->tmouts)) {
			printk(KERN_WARNING "b_srp_fail\n");
			msm_otg_dbg_log_event(&motg->phy, "B_SRP_FAIL",
					motg->inputs, otg->phy->state);
			pr_info("A-device did not respond to SRP\n");
			clear_bit(B_BUS_REQ, &motg->inputs);
			clear_bit(B_SRP_FAIL, &motg->tmouts);
			otg_send_event(otg, OTG_EVENT_NO_RESP_FOR_SRP);
			ulpi_write(otg->phy, 0x0, 0x98);
			otg->phy->state = OTG_STATE_B_IDLE;
			motg->b_last_se0_sess = jiffies;
			work = 1;
		}
		break;
	case OTG_STATE_B_PERIPHERAL:
		if (test_bit(B_SESS_VLD, &motg->inputs) &&
				test_bit(B_FALSE_SDP, &motg->inputs)) {
			printk(KERN_WARNING "B_FALSE_SDP\n");
			msm_otg_dbg_log_event(&motg->phy, "B_FALSE_SDP",
					motg->inputs, otg->phy->state);
			msm_otg_start_peripheral(otg, 0);
			motg->chg_type = USB_DCP_CHARGER;
			clear_bit(B_FALSE_SDP, &motg->inputs);
			otg->phy->state = OTG_STATE_B_IDLE;
			work = 1;
		} else if (!test_bit(ID, &motg->inputs) ||
				test_bit(ID_A, &motg->inputs) ||
				test_bit(ID_B, &motg->inputs) ||
				!test_bit(B_SESS_VLD, &motg->inputs)) {
			cancel_delayed_work_sync(&motg->chg_work);
			if (motg->connect_type != CONNECT_TYPE_NONE) {
				motg->connect_type = CONNECT_TYPE_NONE;
				
			}
			printk(KERN_WARNING "!id  || id_a/b || !b_sess_vld\n");
			msm_otg_dbg_log_event(&motg->phy,
					"!ID || ID_A/B || !B_SESS_VLD",
					motg->inputs, otg->phy->state);
			motg->chg_state = USB_CHG_STATE_UNDEFINED;
			motg->chg_type = USB_INVALID_CHARGER;
			msm_otg_notify_charger(motg, 0);
			srp_reqd = otg->gadget->otg_srp_reqd;
			msm_otg_start_peripheral(otg, 0);
			if (test_bit(ID_B, &motg->inputs))
				clear_bit(ID_B, &motg->inputs);
			clear_bit(B_BUS_REQ, &motg->inputs);
			otg->phy->state = OTG_STATE_B_IDLE;
			motg->b_last_se0_sess = jiffies;
			if (srp_reqd)
				msm_otg_start_timer(motg,
					TB_TST_SRP, B_TST_SRP);
			else
				work = 1;
		} else if (test_bit(B_BUS_REQ, &motg->inputs) &&
				otg->gadget->b_hnp_enable &&
				test_bit(A_BUS_SUSPEND, &motg->inputs)) {
			printk(KERN_WARNING "b_bus_req && b_hnp_en && a_bus_suspend\n");
			msm_otg_dbg_log_event(&motg->phy,
					"B_BUS_REQ && B_HNP_EN && A_BUS_SUSPEND",
					motg->inputs, otg->phy->state);
			msm_otg_start_timer(motg, TB_ASE0_BRST, B_ASE0_BRST);
			udelay(1000);
			msm_otg_start_peripheral(otg, 0);
			otg->phy->state = OTG_STATE_B_WAIT_ACON;
			otg->host->is_b_host = 1;
			msm_otg_start_host(otg, 1);
		} else if (test_bit(A_BUS_SUSPEND, &motg->inputs) &&
				   test_bit(B_SESS_VLD, &motg->inputs)) {
			printk(KERN_WARNING "a_bus_suspend && b_sess_vld\n");
			msm_otg_dbg_log_event(&motg->phy,
					"A_BUS_SUSPEND && B_SESS_VLD",
					motg->inputs, otg->phy->state);
			if (motg->caps & ALLOW_LPM_ON_DEV_SUSPEND) {
				msm_otg_dbg_log_event(&motg->phy,
					"PM RUNTIME: BPER PUT",
					get_pm_runtime_counter(otg->phy->dev),
					motg->pm_done);
				pm_runtime_put_noidle(otg->phy->dev);
				pm_runtime_suspend(otg->phy->dev);
				motg->pm_done = 1;
			}
		} else if (test_bit(ID_C, &motg->inputs)) {
			USB_INFO("id_c\n");
			msm_otg_notify_charger(motg, IDEV_ACA_CHG_MAX);
		} else if (test_bit(B_SESS_VLD, &motg->inputs) && msm_otg_usb_disable == 1) {
			USB_INFO("b_sess_vld && htc_usb_disable = 1\n");
			if (motg->connect_type == CONNECT_TYPE_UNKNOWN)
				del_timer_sync(&motg->chg_check_timer);
			msm_otg_start_peripheral(otg, 0);
			otg->phy->state = OTG_STATE_B_IDLE;
		}
		break;
	case OTG_STATE_B_CHARGER:
		if (test_bit(B_SESS_VLD, &motg->inputs)) {
			pr_debug("BSV set again\n");
			msm_otg_dbg_log_event(&motg->phy, "BSV SET AGAIN",
					motg->inputs, otg->phy->state);
		} else if (!test_bit(B_SESS_VLD, &motg->inputs)) {
			otg->phy->state = OTG_STATE_B_IDLE;
			work = 1;
		}
		break;
	case OTG_STATE_B_WAIT_ACON:
		if (!test_bit(ID, &motg->inputs) ||
				test_bit(ID_A, &motg->inputs) ||
				test_bit(ID_B, &motg->inputs) ||
				!test_bit(B_SESS_VLD, &motg->inputs)) {
			printk(KERN_WARNING "!id || id_a/b || !b_sess_vld\n");
			msm_otg_dbg_log_event(&motg->phy,
					"!ID || ID_A/B || !B_SESS_VLD",
					motg->inputs, otg->phy->state);
			msm_otg_del_timer(motg);
			msm_otg_start_host(otg, 0);
			otg->host->is_b_host = 0;

			clear_bit(B_BUS_REQ, &motg->inputs);
			clear_bit(A_BUS_SUSPEND, &motg->inputs);
			motg->b_last_se0_sess = jiffies;
			otg->phy->state = OTG_STATE_B_IDLE;
			msm_otg_reset(otg->phy);
			work = 1;
		} else if (test_bit(A_CONN, &motg->inputs)) {
			printk(KERN_WARNING "a_conn\n");
			msm_otg_dbg_log_event(&motg->phy, "A_CONN",
					motg->inputs, otg->phy->state);
			clear_bit(A_BUS_SUSPEND, &motg->inputs);
			otg->phy->state = OTG_STATE_B_HOST;
			msm_otg_start_timer(motg, TB_TST_CONFIG,
						B_TST_CONFIG);
		} else if (test_bit(B_ASE0_BRST, &motg->tmouts)) {
			printk(KERN_WARNING "b_ase0_brst_tmout\n");
			msm_otg_dbg_log_event(&motg->phy, "B_ASE0_BRST_TMOUT",
					motg->inputs, otg->phy->state);
			pr_info("B HNP fail:No response from A device\n");
			msm_otg_start_host(otg, 0);
			msm_otg_reset(otg->phy);
			otg->host->is_b_host = 0;
			clear_bit(B_ASE0_BRST, &motg->tmouts);
			clear_bit(A_BUS_SUSPEND, &motg->inputs);
			clear_bit(B_BUS_REQ, &motg->inputs);
			otg_send_event(otg, OTG_EVENT_HNP_FAILED);
			otg->phy->state = OTG_STATE_B_IDLE;
			work = 1;
		} else if (test_bit(ID_C, &motg->inputs)) {
			msm_otg_notify_charger(motg, IDEV_ACA_CHG_MAX);
		}
		break;
	case OTG_STATE_B_HOST:
		if (!test_bit(B_BUS_REQ, &motg->inputs) ||
				!test_bit(A_CONN, &motg->inputs) ||
				!test_bit(B_SESS_VLD, &motg->inputs)) {
			printk(KERN_WARNING "!b_bus_req || !a_conn || !b_sess_vld\n");
			msm_otg_dbg_log_event(&motg->phy,
					"!B_BUS_REQ || !A_CONN || !B_SESS_VLD",
					motg->inputs, otg->phy->state);
			clear_bit(A_CONN, &motg->inputs);
			clear_bit(B_BUS_REQ, &motg->inputs);
			msm_otg_start_host(otg, 0);
			otg->host->is_b_host = 0;
			otg->phy->state = OTG_STATE_B_IDLE;
			msm_otg_reset(otg->phy);
			work = 1;
		} else if (test_bit(ID_C, &motg->inputs)) {
			msm_otg_notify_charger(motg, IDEV_ACA_CHG_MAX);
		}
		break;
	case OTG_STATE_A_IDLE:
		otg->default_a = 1;
		if (test_bit(ID, &motg->inputs) &&
			!test_bit(ID_A, &motg->inputs)) {
			printk(KERN_WARNING "id && !id_a\n");
			msm_otg_dbg_log_event(&motg->phy, "!ID || ID_A",
					motg->inputs, otg->phy->state);
			otg->default_a = 0;
			clear_bit(A_BUS_DROP, &motg->inputs);
			otg->phy->state = OTG_STATE_B_IDLE;
			del_timer_sync(&motg->id_timer);
			msm_otg_link_reset(motg);
			msm_chg_enable_aca_intr(motg);
			msm_otg_notify_charger(motg, 0);
			work = 1;
		} else if (!test_bit(A_BUS_DROP, &motg->inputs) &&
				(test_bit(A_SRP_DET, &motg->inputs) ||
				 test_bit(A_BUS_REQ, &motg->inputs))) {
			printk(KERN_WARNING "!a_bus_drop && (a_srp_det || a_bus_req)\n");
			msm_otg_dbg_log_event(&motg->phy,
				"!A_BUS_DROP || A_SRP_DET || A_BUS_REQ",
				motg->inputs, otg->phy->state);

			clear_bit(A_SRP_DET, &motg->inputs);
			
			writel_relaxed((readl_relaxed(USB_OTGSC) &
					~OTGSC_INTSTS_MASK) &
					~OTGSC_DPIE, USB_OTGSC);

			otg->phy->state = OTG_STATE_A_WAIT_VRISE;
			usleep_range(10000, 12000);
			
			if (test_bit(ID_A, &motg->inputs))
				msm_otg_notify_charger(motg, 0);
			else
				msm_hsusb_vbus_power(motg, 1);
			msm_otg_start_timer(motg, TA_WAIT_VRISE, A_WAIT_VRISE);
		} else {
			printk(KERN_WARNING "No session requested\n");
			msm_otg_dbg_log_event(&motg->phy,
					"NO SESSION REQUESTED",
					motg->inputs, otg->phy->state);
			clear_bit(A_BUS_DROP, &motg->inputs);
			if (test_bit(ID_A, &motg->inputs)) {
					msm_otg_notify_charger(motg,
							IDEV_ACA_CHG_MAX);
			} else if (!test_bit(ID, &motg->inputs)) {
				msm_otg_notify_charger(motg, 0);
				writel_relaxed(0x13, USB_USBMODE);
				writel_relaxed((readl_relaxed(USB_OTGSC) &
						~OTGSC_INTSTS_MASK) |
						OTGSC_DPIE, USB_OTGSC);
				mb();
			}
		}
		break;
	case OTG_STATE_A_WAIT_VRISE:
		if ((test_bit(ID, &motg->inputs) &&
				!test_bit(ID_A, &motg->inputs)) ||
				test_bit(A_BUS_DROP, &motg->inputs) ||
				test_bit(A_WAIT_VRISE, &motg->tmouts)) {
			printk(KERN_WARNING "id || a_bus_drop || a_wait_vrise_tmout\n");
			msm_otg_dbg_log_event(&motg->phy,
					"ID || A_BUS_DROP || A_WAIT_VRISE_TMOUT",
					motg->inputs, otg->phy->state);
			clear_bit(A_BUS_REQ, &motg->inputs);
			msm_otg_del_timer(motg);
			msm_hsusb_vbus_power(motg, 0);
			otg->phy->state = OTG_STATE_A_WAIT_VFALL;
			msm_otg_start_timer(motg, TA_WAIT_VFALL, A_WAIT_VFALL);
		} else if (test_bit(A_VBUS_VLD, &motg->inputs)) {
			printk(KERN_WARNING "a_vbus_vld\n");
			msm_otg_dbg_log_event(&motg->phy, "A_VBUS_VLD",
					motg->inputs, otg->phy->state);
			otg->phy->state = OTG_STATE_A_WAIT_BCON;
			if (TA_WAIT_BCON > 0)
				msm_otg_start_timer(motg, TA_WAIT_BCON,
					A_WAIT_BCON);

			
			clear_bit(B_SESS_VLD, &motg->inputs);
			msm_otg_start_host(otg, 1);
			msm_chg_enable_aca_det(motg);
			msm_chg_disable_aca_intr(motg);
			mod_timer(&motg->id_timer, ID_TIMER_FREQ);
			if (msm_chg_check_aca_intr(motg))
				work = 1;
		}
		break;
	case OTG_STATE_A_WAIT_BCON:
		if ((test_bit(ID, &motg->inputs) &&
				!test_bit(ID_A, &motg->inputs)) ||
				test_bit(A_BUS_DROP, &motg->inputs) ||
				test_bit(A_WAIT_BCON, &motg->tmouts)) {
			printk(KERN_WARNING "(id && id_a/b/c) || a_bus_drop ||"
					"a_wait_bcon_tmout\n");
			msm_otg_dbg_log_event(&motg->phy,
				"(ID && ID_A/B/C )|| A_BUSS_DROP || A_WAIT_BCON_TMOUT",
				motg->inputs, otg->phy->state);
			if (test_bit(A_WAIT_BCON, &motg->tmouts)) {
				pr_info("Device No Response\n");
				otg_send_event(otg, OTG_EVENT_DEV_CONN_TMOUT);
			}
			msm_otg_del_timer(motg);
			clear_bit(A_BUS_REQ, &motg->inputs);
			clear_bit(B_CONN, &motg->inputs);
			msm_otg_start_host(otg, 0);
			if (test_bit(ID_A, &motg->inputs))
				msm_otg_notify_charger(motg, IDEV_CHG_MIN);
			else
				msm_hsusb_vbus_power(motg, 0);
			otg->phy->state = OTG_STATE_A_WAIT_VFALL;
			msm_otg_start_timer(motg, TA_WAIT_VFALL, A_WAIT_VFALL);
		} else if (!test_bit(A_VBUS_VLD, &motg->inputs)) {
			printk(KERN_WARNING "!a_vbus_vld\n");
			msm_otg_dbg_log_event(&motg->phy, "!A_VBUS_VLD",
					motg->inputs, otg->phy->state);
			clear_bit(B_CONN, &motg->inputs);
			msm_otg_del_timer(motg);
			msm_otg_start_host(otg, 0);
			otg->phy->state = OTG_STATE_A_VBUS_ERR;
			msm_otg_reset(otg->phy);
		} else if (test_bit(ID_A, &motg->inputs)) {
			msm_hsusb_vbus_power(motg, 0);
		} else if (!test_bit(A_BUS_REQ, &motg->inputs)) {
			if (TA_WAIT_BCON < 0) {
				msm_otg_dbg_log_event(&motg->phy,
					"PM RUNTIME: AWBCONN PUT",
					get_pm_runtime_counter(otg->phy->dev),
					0);
				pm_runtime_put_sync(otg->phy->dev);
			}
		} else if (!test_bit(ID, &motg->inputs)) {
			msm_hsusb_vbus_power(motg, 1);
		}
		break;
	case OTG_STATE_A_HOST:
		if ((test_bit(ID, &motg->inputs) &&
				!test_bit(ID_A, &motg->inputs)) ||
				test_bit(A_BUS_DROP, &motg->inputs)) {
			printk(KERN_WARNING "id_a/b/c || a_bus_drop\n");
			msm_otg_dbg_log_event(&motg->phy,
					"ID_A/B/C || A_VBUS_DROP",
					motg->inputs, otg->phy->state);
			clear_bit(B_CONN, &motg->inputs);
			clear_bit(A_BUS_REQ, &motg->inputs);
			msm_otg_del_timer(motg);
			otg->phy->state = OTG_STATE_A_WAIT_VFALL;
			msm_otg_start_host(otg, 0);
			if (!test_bit(ID_A, &motg->inputs))
				msm_hsusb_vbus_power(motg, 0);
			msm_otg_start_timer(motg, TA_WAIT_VFALL, A_WAIT_VFALL);
		} else if (!test_bit(A_VBUS_VLD, &motg->inputs)) {
			printk(KERN_WARNING "!a_vbus_vld\n");
			msm_otg_dbg_log_event(&motg->phy, "!A_VBUS_VLD",
					motg->inputs, otg->phy->state);
			clear_bit(B_CONN, &motg->inputs);
			msm_otg_del_timer(motg);
			otg->phy->state = OTG_STATE_A_VBUS_ERR;
			msm_otg_start_host(otg, 0);
			msm_otg_reset(otg->phy);
		} else if (!test_bit(A_BUS_REQ, &motg->inputs)) {
			printk(KERN_WARNING "!a_bus_req\n");
			msm_otg_dbg_log_event(&motg->phy, "A_BUS_REQ",
					motg->inputs, otg->phy->state);
			msm_otg_del_timer(motg);
			otg->phy->state = OTG_STATE_A_SUSPEND;
			if (otg->host->b_hnp_enable)
				msm_otg_start_timer(motg, TA_AIDL_BDIS,
						A_AIDL_BDIS);
			else {
				msm_otg_dbg_log_event(&motg->phy,
					"PM RUNTIME: AHOST PUT",
					get_pm_runtime_counter(otg->phy->dev),
					0);
				pm_runtime_put_sync(otg->phy->dev);
			}
		} else if (!test_bit(B_CONN, &motg->inputs)) {
			printk(KERN_WARNING "!b_conn\n");
			msm_otg_dbg_log_event(&motg->phy, "!B_CONN",
					motg->inputs, otg->phy->state);
			msm_otg_del_timer(motg);
			otg->phy->state = OTG_STATE_A_WAIT_BCON;
			if (TA_WAIT_BCON > 0)
				msm_otg_start_timer(motg, TA_WAIT_BCON,
					A_WAIT_BCON);
			if (msm_chg_check_aca_intr(motg))
				work = 1;
		} else if (test_bit(ID_A, &motg->inputs)) {
			msm_otg_del_timer(motg);
			msm_hsusb_vbus_power(motg, 0);
			if (motg->chg_type == USB_ACA_DOCK_CHARGER)
				msm_otg_notify_charger(motg,
						IDEV_ACA_CHG_MAX);
			else
				msm_otg_notify_charger(motg,
						IDEV_CHG_MIN - motg->mA_port);
		} else if (!test_bit(ID, &motg->inputs)) {
			motg->chg_state = USB_CHG_STATE_UNDEFINED;
			motg->chg_type = USB_INVALID_CHARGER;
			msm_otg_notify_charger(motg, 0);
			msm_hsusb_vbus_power(motg, 1);
		}
		break;
	case OTG_STATE_A_SUSPEND:
		if ((test_bit(ID, &motg->inputs) &&
				!test_bit(ID_A, &motg->inputs)) ||
				test_bit(A_BUS_DROP, &motg->inputs) ||
				test_bit(A_AIDL_BDIS, &motg->tmouts)) {
			printk(KERN_WARNING "id_a/b/c || a_bus_drop ||"
					"a_aidl_bdis_tmout\n");
			msm_otg_dbg_log_event(&motg->phy,
				"ID_A/B/C || A_BUS_DROP || A_AIDL_BDIS_TMOUT",
				motg->inputs, otg->phy->state);
			msm_otg_del_timer(motg);
			clear_bit(B_CONN, &motg->inputs);
			otg->phy->state = OTG_STATE_A_WAIT_VFALL;
			msm_otg_start_host(otg, 0);
			if (!test_bit(ID_A, &motg->inputs))
				msm_hsusb_vbus_power(motg, 0);
			msm_otg_start_timer(motg, TA_WAIT_VFALL, A_WAIT_VFALL);
		} else if (!test_bit(A_VBUS_VLD, &motg->inputs)) {
			printk(KERN_WARNING "!a_vbus_vld\n");
			msm_otg_dbg_log_event(&motg->phy, "!A_VBUS_VLD",
					motg->inputs, otg->phy->state);
			msm_otg_del_timer(motg);
			clear_bit(B_CONN, &motg->inputs);
			otg->phy->state = OTG_STATE_A_VBUS_ERR;
			msm_otg_start_host(otg, 0);
			msm_otg_reset(otg->phy);
		} else if (!test_bit(B_CONN, &motg->inputs) &&
				otg->host->b_hnp_enable) {
			printk(KERN_WARNING "!b_conn && b_hnp_enable");
			msm_otg_dbg_log_event(&motg->phy,
					"!B_CONN && B_HNP_ENABLED",
					motg->inputs, otg->phy->state);
			otg->phy->state = OTG_STATE_A_PERIPHERAL;
			msm_otg_host_hnp_enable(otg, 1);
			otg->gadget->is_a_peripheral = 1;
			msm_otg_start_peripheral(otg, 1);
		} else if (!test_bit(B_CONN, &motg->inputs) &&
				!otg->host->b_hnp_enable) {
			printk(KERN_WARNING "!b_conn && !b_hnp_enable");
			msm_otg_dbg_log_event(&motg->phy,
					"!B_CONN && !B_HNP_ENABLE",
					motg->inputs, otg->phy->state);
			set_bit(A_BUS_REQ, &motg->inputs);
			otg->phy->state = OTG_STATE_A_WAIT_BCON;
			if (TA_WAIT_BCON > 0)
				msm_otg_start_timer(motg, TA_WAIT_BCON,
					A_WAIT_BCON);
		} else if (test_bit(ID_A, &motg->inputs)) {
			msm_hsusb_vbus_power(motg, 0);
			msm_otg_notify_charger(motg,
					IDEV_CHG_MIN - motg->mA_port);
		} else if (!test_bit(ID, &motg->inputs)) {
			msm_otg_notify_charger(motg, 0);
			msm_hsusb_vbus_power(motg, 1);
		}
		break;
	case OTG_STATE_A_PERIPHERAL:
		if ((test_bit(ID, &motg->inputs) &&
				!test_bit(ID_A, &motg->inputs)) ||
				test_bit(A_BUS_DROP, &motg->inputs)) {
			printk(KERN_WARNING "id _f/b/c || a_bus_drop\n");
			msm_otg_dbg_log_event(&motg->phy, "ID_A/B/C A_BUS_DROP",
					motg->inputs, otg->phy->state);
			
			msm_otg_del_timer(motg);
			otg->phy->state = OTG_STATE_A_WAIT_VFALL;
			msm_otg_start_peripheral(otg, 0);
			otg->gadget->is_a_peripheral = 0;
			msm_otg_start_host(otg, 0);
			msm_otg_reset(otg->phy);
			if (!test_bit(ID_A, &motg->inputs))
				msm_hsusb_vbus_power(motg, 0);
			msm_otg_start_timer(motg, TA_WAIT_VFALL, A_WAIT_VFALL);
		} else if (!test_bit(A_VBUS_VLD, &motg->inputs)) {
			printk(KERN_WARNING "!a_vbus_vld\n");
			msm_otg_dbg_log_event(&motg->phy, "!A_VBUS_VLD",
					motg->inputs, otg->phy->state);
			
			msm_otg_del_timer(motg);
			otg->phy->state = OTG_STATE_A_VBUS_ERR;
			msm_otg_start_peripheral(otg, 0);
			otg->gadget->is_a_peripheral = 0;
			msm_otg_start_host(otg, 0);
		} else if (test_bit(A_BIDL_ADIS, &motg->tmouts)) {
			printk(KERN_WARNING "a_bidl_adis_tmout\n");
			msm_otg_dbg_log_event(&motg->phy, "A_BIDL_ADIS_TMOUT",
					motg->inputs, otg->phy->state);
			msm_otg_start_peripheral(otg, 0);
			otg->gadget->is_a_peripheral = 0;
			otg->phy->state = OTG_STATE_A_WAIT_BCON;
			set_bit(A_BUS_REQ, &motg->inputs);
			msm_otg_host_hnp_enable(otg, 0);
			if (TA_WAIT_BCON > 0)
				msm_otg_start_timer(motg, TA_WAIT_BCON,
					A_WAIT_BCON);
		} else if (test_bit(ID_A, &motg->inputs)) {
			msm_hsusb_vbus_power(motg, 0);
			msm_otg_notify_charger(motg,
					IDEV_CHG_MIN - motg->mA_port);
		} else if (!test_bit(ID, &motg->inputs)) {
			msm_otg_notify_charger(motg, 0);
			msm_hsusb_vbus_power(motg, 1);
		}
		break;
	case OTG_STATE_A_WAIT_VFALL:
		if (test_bit(A_WAIT_VFALL, &motg->tmouts)) {
			clear_bit(A_VBUS_VLD, &motg->inputs);
			otg->phy->state = OTG_STATE_A_IDLE;
			work = 1;
		}
		break;
	case OTG_STATE_A_VBUS_ERR:
		if ((test_bit(ID, &motg->inputs) &&
				!test_bit(ID_A, &motg->inputs)) ||
				test_bit(A_BUS_DROP, &motg->inputs) ||
				test_bit(A_CLR_ERR, &motg->inputs)) {
			otg->phy->state = OTG_STATE_A_WAIT_VFALL;
			if (!test_bit(ID_A, &motg->inputs))
				msm_hsusb_vbus_power(motg, 0);
			msm_otg_start_timer(motg, TA_WAIT_VFALL, A_WAIT_VFALL);
			motg->chg_state = USB_CHG_STATE_UNDEFINED;
			motg->chg_type = USB_INVALID_CHARGER;
			msm_otg_notify_charger(motg, 0);
		}
		break;
	default:
		break;
	}
	if (work)
		queue_work(motg->otg_wq, &motg->sm_work);
}

static void msm_otg_suspend_work(struct work_struct *w)
{
	struct msm_otg *motg =
		container_of(w, struct msm_otg, suspend_work.work);

	
	if (test_bit(A_BUS_SUSPEND, &motg->inputs))
		msm_otg_sm_work(&motg->sm_work);
}

static irqreturn_t msm_otg_irq(int irq, void *data)
{
	struct msm_otg *motg = data;
	struct usb_otg *otg = motg->phy.otg;
	u32 otgsc = 0, usbsts, pc;
	bool work = 0;
	irqreturn_t ret = IRQ_HANDLED;

	if (atomic_read(&motg->in_lpm)) {
		printk(KERN_WARNING "OTG IRQ: %d in LPM\n", irq);
		msm_otg_dbg_log_event(&motg->phy, "OTG IRQ IS IN LPM",
				irq, otg->phy->state);
		
		if (motg->async_int)
			return IRQ_HANDLED;

		disable_irq_nosync(irq);
		motg->async_int = irq;
		if (!atomic_read(&motg->pm_suspended)) {
			if (otg->phy->state >= OTG_STATE_A_IDLE)
				set_bit(A_BUS_REQ, &motg->inputs);
			pm_request_resume(otg->phy->dev);
		}
		return IRQ_HANDLED;
	}

	usbsts = readl(USB_USBSTS);
	otgsc = readl(USB_OTGSC);

	if (!(otgsc & OTG_OTGSTS_MASK) && !(usbsts & OTG_USBSTS_MASK))
		return IRQ_NONE;

	if ((otgsc & OTGSC_IDIS) && (otgsc & OTGSC_IDIE)) {
		if (otgsc & OTGSC_ID) {
			dev_dbg(otg->phy->dev, "ID set\n");
			msm_otg_dbg_log_event(&motg->phy, "ID SET",
				motg->inputs, otg->phy->state);
			set_bit(ID, &motg->inputs);
		} else {
			dev_dbg(otg->phy->dev, "ID clear\n");
			msm_otg_dbg_log_event(&motg->phy, "ID CLEAR",
					motg->inputs, otg->phy->state);
			set_bit(A_BUS_REQ, &motg->inputs);
			clear_bit(ID, &motg->inputs);
			msm_chg_enable_aca_det(motg);
		}
		writel_relaxed(otgsc, USB_OTGSC);
		work = 1;
	} else if (otgsc & OTGSC_DPIS) {
		printk(KERN_WARNING "DPIS detected\n");
		writel_relaxed(otgsc, USB_OTGSC);
		set_bit(A_SRP_DET, &motg->inputs);
		set_bit(A_BUS_REQ, &motg->inputs);
		work = 1;
	} else if ((otgsc & OTGSC_BSVIE) && (otgsc & OTGSC_BSVIS)) {
		writel_relaxed(otgsc, USB_OTGSC);
		if ((otg->phy->state >= OTG_STATE_A_IDLE) &&
			!test_bit(ID_A, &motg->inputs))
			return IRQ_HANDLED;
		if (otgsc & OTGSC_BSV) {
			dev_dbg(otg->phy->dev, "BSV set\n");
			msm_otg_dbg_log_event(&motg->phy, "BSV SET",
					motg->inputs, otg->phy->state);
			set_bit(B_SESS_VLD, &motg->inputs);
		} else {
			dev_dbg(otg->phy->dev, "BSV clear\n");
			msm_otg_dbg_log_event(&motg->phy, "BSV CLEAR",
					motg->inputs, otg->phy->state);
			clear_bit(B_SESS_VLD, &motg->inputs);
			clear_bit(A_BUS_SUSPEND, &motg->inputs);

			msm_chg_check_aca_intr(motg);
		}
		work = 1;
	} else if (usbsts & STS_PCI) {
		pc = readl_relaxed(USB_PORTSC);
		printk(KERN_WARNING "portsc = %x\n", pc);
		msm_otg_dbg_log_event(&motg->phy, "PORTSC",
				motg->inputs, otg->phy->state);
		ret = IRQ_NONE;
		work = 1;
		switch (otg->phy->state) {
		case OTG_STATE_A_SUSPEND:
			if (otg->host->b_hnp_enable && (pc & PORTSC_CSC) &&
					!(pc & PORTSC_CCS)) {
				printk(KERN_WARNING "B_CONN clear\n");
				msm_otg_dbg_log_event(&motg->phy,
						"B_CONN CLEAR",
						motg->inputs, otg->phy->state);
				clear_bit(B_CONN, &motg->inputs);
				msm_otg_del_timer(motg);
			}
			break;
		case OTG_STATE_A_PERIPHERAL:
			msm_otg_del_timer(motg);
			work = 0;
			break;
		case OTG_STATE_B_WAIT_ACON:
			if ((pc & PORTSC_CSC) && (pc & PORTSC_CCS)) {
				printk(KERN_WARNING "A_CONN set\n");
				msm_otg_dbg_log_event(&motg->phy, "A_CONN SET",
						motg->inputs, otg->phy->state);
				set_bit(A_CONN, &motg->inputs);
				
				msm_otg_del_timer(motg);
			}
			break;
		case OTG_STATE_B_HOST:
			if ((pc & PORTSC_CSC) && !(pc & PORTSC_CCS)) {
				printk(KERN_WARNING "A_CONN clear\n");
				msm_otg_dbg_log_event(&motg->phy,
						"A_CONN CLEAR",
						motg->inputs, otg->phy->state);
				clear_bit(A_CONN, &motg->inputs);
				msm_otg_del_timer(motg);
			}
			break;
		case OTG_STATE_A_WAIT_BCON:
			if (TA_WAIT_BCON < 0)
				set_bit(A_BUS_REQ, &motg->inputs);
		default:
			work = 0;
			break;
		}
	} else if (usbsts & STS_URI) {
		ret = IRQ_NONE;
		switch (otg->phy->state) {
		case OTG_STATE_A_PERIPHERAL:
			msm_otg_del_timer(motg);
			work = 0;
			break;
		default:
			work = 0;
			break;
		}
	} else if (usbsts & STS_SLI) {
		ret = IRQ_NONE;
		work = 0;
		switch (otg->phy->state) {
		case OTG_STATE_B_PERIPHERAL:
			if (otg->gadget->b_hnp_enable) {
				set_bit(A_BUS_SUSPEND, &motg->inputs);
				set_bit(B_BUS_REQ, &motg->inputs);
				work = 1;
			}
			break;
		case OTG_STATE_A_PERIPHERAL:
			msm_otg_start_timer(motg, TA_BIDL_ADIS,
					A_BIDL_ADIS);
			break;
		default:
			break;
		}
	} else if ((usbsts & PHY_ALT_INT)) {
		writel_relaxed(PHY_ALT_INT, USB_USBSTS);
		if (msm_chg_check_aca_intr(motg))
			work = 1;
		ret = IRQ_HANDLED;
	}
	if (work)
		queue_work(motg->otg_wq, &motg->sm_work);

	return ret;
}

static void msm_otg_set_vbus_state(int online)
{
	struct msm_otg *motg = the_msm_otg;
	static bool init;

	printk(KERN_INFO "[USB] %s:%d\n", __func__, online);
	motg->vbus_state = online;

	if (motg->err_event_seen)
		return;

	if (online) {
		printk(KERN_WARNING "[USB] PMIC: BSV set\n");
		msm_otg_dbg_log_event(&motg->phy, "PMIC: BSV SET",
				init, motg->inputs);
		if (test_and_set_bit(B_SESS_VLD, &motg->inputs) && init)
			return;
	} else {
		pr_debug("PMIC: BSV clear\n");
		msm_otg_dbg_log_event(&motg->phy, "PMIC: BSV CLEAR",
				init, motg->inputs);
		if (!test_and_clear_bit(B_SESS_VLD, &motg->inputs) && init)
			return;
	}

	
	if (!test_bit(ID, &motg->inputs)) {
		if (init)
			return;
	}
	printk(KERN_INFO "[USB] %s:init=[%d]\n", __func__, init);

	if (!init) {
		init = true;
		if (pmic_vbus_init.done &&
				test_bit(B_SESS_VLD, &motg->inputs)) {
			printk(KERN_WARNING "PMIC: BSV came late\n");
			msm_otg_dbg_log_event(&motg->phy, "PMIC: BSV CAME LATE",
					init, motg->inputs);
			goto out;
		}
		complete(&pmic_vbus_init);
		printk(KERN_WARNING "[USB] PMIC: BSV init complete\n");
		msm_otg_dbg_log_event(&motg->phy, "PMIC: BSV INIT COMPLETE",
				init, motg->inputs);
		return;
	}

out:
	printk(KERN_INFO "[USB] %s:out1\n", __func__);
	if (test_bit(MHL, &motg->inputs) ||
			mhl_det_in_progress) {
		printk(KERN_WARNING "PMIC: BSV interrupt ignored in MHL\n");
		return;
	}

	wake_lock_timeout(&motg->cable_detect_wlock, 3 * HZ);
	if (motg->is_ext_chg_dcp) {
		if (test_bit(B_SESS_VLD, &motg->inputs)) {
			msm_otg_notify_charger(motg, IDEV_CHG_MAX);
		} else {
			motg->is_ext_chg_dcp = false;
			motg->chg_state = USB_CHG_STATE_UNDEFINED;
			motg->chg_type = USB_INVALID_CHARGER;
			msm_otg_notify_charger(motg, 0);
		}
		return;
	}

	msm_otg_dbg_log_event(&motg->phy, "CHECK VBUS EVENT DURING SUSPEND",
			atomic_read(&motg->pm_suspended),
			motg->sm_work_pending);
	if (atomic_read(&motg->pm_suspended)) {
		motg->sm_work_pending = true;
	} else if (!motg->sm_work_pending) {
	printk(KERN_INFO "[USB] %s: queue_work motg->sm_work\n", __func__);
		
		queue_work(motg->otg_wq, &motg->sm_work);
	}
	printk(KERN_INFO "[USB] %s:end\n", __func__);
}
void msm_otg_set_disable_usb(int disable_usb)
{
	struct msm_otg *motg = the_msm_otg;
	msm_otg_usb_disable = disable_usb;

	printk(KERN_INFO "[USB] %s: chg_type =%d, connect_type=%d\n", __func__,motg->chg_type,motg->connect_type);
	if (motg->chg_state == USB_CHG_STATE_DETECTED &&
		(motg->chg_type == USB_DCP_CHARGER ||
		motg->chg_type == USB_PROPRIETARY_CHARGER ||
		motg->chg_type == USB_FLOATED_CHARGER)) {
		return;
	}

	if(disable_usb == 1) {
		if (!test_bit(ID, &motg->inputs))
			set_bit(ID, &motg->inputs);
		else if (motg->connect_type == CONNECT_TYPE_AC || motg->connect_type == CONNECT_TYPE_NONE)
			return;
	} else {
 if (motg->connect_type == CONNECT_TYPE_AC || motg->connect_type == CONNECT_TYPE_UNKNOWN  || motg->connect_type == CONNECT_TYPE_NONE)
			return;
	}

	queue_work(motg->otg_wq, &motg->sm_work);
}
static void msm_id_status_w(struct work_struct *w)
{
	struct msm_otg *motg = container_of(w, struct msm_otg,
						id_status_work.work);
	int work = 0;

	USB_INFO("ID status_w\n");

	if (motg->phy.state != OTG_STATE_UNDEFINED) {
		if (atomic_read(&motg->pm_suspended)) {
			 motg->sm_work_pending = true;
		} else if (!motg->sm_work_pending) {
			
			queue_work(motg->otg_wq, &motg->sm_work);
		}
	}
	
	return;

	if (motg->pdata->pmic_id_irq)
		motg->id_state = msm_otg_read_pmic_id_state(motg);
	else if (motg->ext_id_irq)
		motg->id_state = gpio_get_value(motg->pdata->usb_id_gpio);
	else if (motg->phy_irq)
		motg->id_state = msm_otg_read_phy_id_state(motg);

	if (motg->err_event_seen)
		return;

	if (motg->id_state) {
		if (gpio_is_valid(motg->pdata->switch_sel_gpio))
			gpio_direction_input(motg->pdata->switch_sel_gpio);
		if (!test_and_set_bit(ID, &motg->inputs)) {
			printk(KERN_WARNING "PMIC: ID set\n");
			msm_otg_dbg_log_event(&motg->phy, "ID SET",
					motg->inputs, motg->phy.state);
			work = 1;
		}
	} else {
		if (gpio_is_valid(motg->pdata->switch_sel_gpio))
			gpio_direction_output(motg->pdata->switch_sel_gpio, 1);
		if (test_and_clear_bit(ID, &motg->inputs)) {
			printk(KERN_WARNING "PMIC: ID clear\n");
			msm_otg_dbg_log_event(&motg->phy, "ID CLEAR",
					motg->inputs, motg->phy.state);
			set_bit(A_BUS_REQ, &motg->inputs);
			work = 1;
		}
	}

	if (work && (motg->phy.state != OTG_STATE_UNDEFINED)) {
		msm_otg_dbg_log_event(&motg->phy,
				"CHECK ID EVENT DURING SUSPEND",
				atomic_read(&motg->pm_suspended),
				motg->sm_work_pending);
		if (atomic_read(&motg->pm_suspended)) {
			motg->sm_work_pending = true;
		} else if (!motg->sm_work_pending) {
			
			queue_work(motg->otg_wq, &motg->sm_work);
		}
	}

}

#define MSM_ID_STATUS_DELAY	5 
static irqreturn_t msm_id_irq(int irq, void *data)
{
	struct msm_otg *motg = data;

	if (test_bit(MHL, &motg->inputs) ||
			mhl_det_in_progress) {
		pr_debug("PMIC: Id interrupt ignored in MHL\n");
		return IRQ_HANDLED;
	}

	if (!aca_id_turned_on)
		
		queue_delayed_work(motg->otg_wq, &motg->id_status_work,
				msecs_to_jiffies(MSM_ID_STATUS_DELAY));

	return IRQ_HANDLED;
}

int msm_otg_pm_notify(struct notifier_block *notify_block,
					unsigned long mode, void *unused)
{
	struct msm_otg *motg = container_of(
		notify_block, struct msm_otg, pm_notify);

	dev_dbg(motg->phy.dev, "OTG PM notify:%lx, sm_pending:%u\n", mode,
					motg->sm_work_pending);
	msm_otg_dbg_log_event(&motg->phy, "PM NOTIFY",
			mode, motg->sm_work_pending);

	switch (mode) {
	case PM_POST_SUSPEND:
		
		atomic_set(&motg->pm_suspended, 0);

		
		if (motg->sm_work_pending) {
			motg->sm_work_pending = false;
			queue_work(motg->otg_wq, &motg->sm_work);
		}
		break;

	default:
		break;
	}

	return NOTIFY_OK;
}

static int msm_otg_mode_show(struct seq_file *s, void *unused)
{
	struct msm_otg *motg = s->private;
	struct usb_otg *otg = motg->phy.otg;

	switch (otg->phy->state) {
	case OTG_STATE_A_WAIT_BCON:
	case OTG_STATE_A_HOST:
	case OTG_STATE_A_SUSPEND:
		seq_printf(s, "host\n");
		break;
	case OTG_STATE_B_IDLE:
	case OTG_STATE_B_PERIPHERAL:
		seq_printf(s, "peripheral\n");
		break;
	default:
		seq_printf(s, "none\n");
		break;
	}

	return 0;
}

static int msm_otg_mode_open(struct inode *inode, struct file *file)
{
	return single_open(file, msm_otg_mode_show, inode->i_private);
}

static ssize_t msm_otg_mode_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct msm_otg *motg = s->private;
	char buf[16];
	struct usb_phy *phy = &motg->phy;
	int status = count;
	enum usb_mode_type req_mode;

	memset(buf, 0x00, sizeof(buf));

	if (copy_from_user(&buf, ubuf, min_t(size_t, sizeof(buf) - 1, count))) {
		status = -EFAULT;
		goto out;
	}

	if (!strncmp(buf, "host", 4)) {
		req_mode = USB_HOST;
	} else if (!strncmp(buf, "peripheral", 10)) {
		req_mode = USB_PERIPHERAL;
	} else if (!strncmp(buf, "none", 4)) {
		req_mode = USB_NONE;
	} else {
		status = -EINVAL;
		goto out;
	}

	switch (req_mode) {
	case USB_NONE:
		switch (phy->state) {
		case OTG_STATE_A_WAIT_BCON:
		case OTG_STATE_A_HOST:
		case OTG_STATE_A_SUSPEND:
		case OTG_STATE_B_PERIPHERAL:
			set_bit(ID, &motg->inputs);
			clear_bit(B_SESS_VLD, &motg->inputs);
			break;
		default:
			goto out;
		}
		break;
	case USB_PERIPHERAL:
		switch (phy->state) {
		case OTG_STATE_B_IDLE:
		case OTG_STATE_A_WAIT_BCON:
		case OTG_STATE_A_HOST:
		case OTG_STATE_A_SUSPEND:
			set_bit(ID, &motg->inputs);
			set_bit(B_SESS_VLD, &motg->inputs);
			break;
		default:
			goto out;
		}
		break;
	case USB_HOST:
		switch (phy->state) {
		case OTG_STATE_B_IDLE:
		case OTG_STATE_B_PERIPHERAL:
			clear_bit(ID, &motg->inputs);
			break;
		default:
			goto out;
		}
		break;
	default:
		goto out;
	}

	motg->id_state = (test_bit(ID, &motg->inputs)) ? USB_ID_FLOAT :
							USB_ID_GROUND;
	pm_runtime_resume(phy->dev);
	queue_work(motg->otg_wq, &motg->sm_work);
out:
	return status;
}

const struct file_operations msm_otg_mode_fops = {
	.open = msm_otg_mode_open,
	.read = seq_read,
	.write = msm_otg_mode_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int msm_otg_show_otg_state(struct seq_file *s, void *unused)
{
	struct msm_otg *motg = s->private;
	struct usb_phy *phy = &motg->phy;

	seq_printf(s, "%s\n", usb_otg_state_string(phy->state));
	return 0;
}

static int msm_otg_otg_state_open(struct inode *inode, struct file *file)
{
	return single_open(file, msm_otg_show_otg_state, inode->i_private);
}

const struct file_operations msm_otg_state_fops = {
	.open = msm_otg_otg_state_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int msm_otg_show_chg_type(struct seq_file *s, void *unused)
{
	struct msm_otg *motg = s->private;

	seq_printf(s, "%s\n", chg_to_string(motg->chg_type));
	return 0;
}

static int msm_otg_chg_open(struct inode *inode, struct file *file)
{
	return single_open(file, msm_otg_show_chg_type, inode->i_private);
}

const struct file_operations msm_otg_chg_fops = {
	.open = msm_otg_chg_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int msm_otg_aca_show(struct seq_file *s, void *unused)
{
	if (debug_aca_enabled)
		seq_printf(s, "enabled\n");
	else
		seq_printf(s, "disabled\n");

	return 0;
}

static int msm_otg_aca_open(struct inode *inode, struct file *file)
{
	return single_open(file, msm_otg_aca_show, inode->i_private);
}

static ssize_t msm_otg_aca_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char buf[8];
	struct msm_otg *motg = the_msm_otg;

	if (motg->pdata->phy_type == SNPS_FEMTO_PHY) {
		pr_err("ACA is not supported on Femto PHY\n");
		return -ENODEV;
	}

	memset(buf, 0x00, sizeof(buf));

	if (copy_from_user(&buf, ubuf, min_t(size_t, sizeof(buf) - 1, count)))
		return -EFAULT;

	if (!strncmp(buf, "enable", 6))
		debug_aca_enabled = true;
	else
		debug_aca_enabled = false;

	return count;
}

const struct file_operations msm_otg_aca_fops = {
	.open = msm_otg_aca_open,
	.read = seq_read,
	.write = msm_otg_aca_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int msm_otg_bus_show(struct seq_file *s, void *unused)
{
	if (debug_bus_voting_enabled)
		seq_printf(s, "enabled\n");
	else
		seq_printf(s, "disabled\n");

	return 0;
}

static int msm_otg_bus_open(struct inode *inode, struct file *file)
{
	return single_open(file, msm_otg_bus_show, inode->i_private);
}

static ssize_t msm_otg_bus_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char buf[8];
	struct seq_file *s = file->private_data;
	struct msm_otg *motg = s->private;

	memset(buf, 0x00, sizeof(buf));

	if (copy_from_user(&buf, ubuf, min_t(size_t, sizeof(buf) - 1, count)))
		return -EFAULT;

	if (!strncmp(buf, "enable", 6)) {
		
		debug_bus_voting_enabled = true;
	} else {
		debug_bus_voting_enabled = false;
		msm_otg_bus_vote(motg, USB_MIN_PERF_VOTE);
	}

	return count;
}

static int msm_otg_dbg_buff_show(struct seq_file *s, void *unused)
{
	struct msm_otg *motg = s->private;
	unsigned long	flags;
	unsigned	i;

	read_lock_irqsave(&motg->dbg_lock, flags);

	i = motg->dbg_idx;
	if (strnlen(motg->buf[i], DEBUG_MSG_LEN))
		seq_printf(s, "%s\n", motg->buf[i]);
	for (dbg_inc(&i); i != motg->dbg_idx;  dbg_inc(&i)) {
		if (!strnlen(motg->buf[i], DEBUG_MSG_LEN))
			continue;
		seq_printf(s, "%s\n", motg->buf[i]);
	}
	read_unlock_irqrestore(&motg->dbg_lock, flags);

	return 0;
}

static int msm_otg_dbg_buff_open(struct inode *inode, struct file *file)
{
	return single_open(file, msm_otg_dbg_buff_show, inode->i_private);
}

const struct file_operations msm_otg_dbg_buff_fops = {
	.open = msm_otg_dbg_buff_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int
otg_get_prop_usbin_voltage_now(struct msm_otg *motg)
{
	int rc = 0;
	struct qpnp_vadc_result results;

	if (IS_ERR_OR_NULL(motg->vadc_dev)) {
		motg->vadc_dev = qpnp_get_vadc(motg->phy.dev, "usbin");
		if (IS_ERR(motg->vadc_dev))
			return PTR_ERR(motg->vadc_dev);
	}

	rc = qpnp_vadc_read(motg->vadc_dev, USBIN, &results);
	if (rc) {
		pr_err("Unable to read usbin rc=%d\n", rc);
		return 0;
	} else {
		return results.physical;
	}
}

static int msm_otg_pmic_dp_dm(struct msm_otg *motg, int value)
{
	int ret = 0;

	switch (value) {
	case POWER_SUPPLY_DP_DM_DPF_DMF:
		if (!motg->rm_pulldown) {
			ret = msm_hsusb_ldo_enable(motg, USB_PHY_REG_ON);
			if (!ret) {
				motg->rm_pulldown = true;
				msm_otg_dbg_log_event(&motg->phy, "RM Pulldown",
						motg->rm_pulldown, 0);
			}
		}
		break;
	case POWER_SUPPLY_DP_DM_DPR_DMR:
		if (motg->rm_pulldown) {
			ret = msm_hsusb_ldo_enable(motg, USB_PHY_REG_OFF);
			if (!ret) {
				motg->rm_pulldown = false;
				msm_otg_dbg_log_event(&motg->phy, "RM Pulldown",
						motg->rm_pulldown, 0);
			}
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int otg_power_get_property_usb(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct msm_otg *motg = container_of(psy, struct msm_otg, usb_psy);
	switch (psp) {
	case POWER_SUPPLY_PROP_SCOPE:
		if (motg->host_mode)
			val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		else
			val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = motg->voltage_max;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = motg->current_max;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_MAX:
		val->intval = motg->typec_current_max;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = !!test_bit(B_SESS_VLD, &motg->inputs);
		break;
	case POWER_SUPPLY_PROP_DP_DM:
		val->intval = motg->rm_pulldown;
		break;
	
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = motg->online;
		break;
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = psy->type;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = motg->usbin_health;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = otg_get_prop_usbin_voltage_now(motg);
		break;
	case POWER_SUPPLY_PROP_USB_OTG:
		val->intval = !motg->id_state;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int otg_power_set_property_usb(struct power_supply *psy,
				  enum power_supply_property psp,
				  const union power_supply_propval *val)
{
	struct msm_otg *motg = container_of(psy, struct msm_otg, usb_psy);

	msm_otg_dbg_log_event(&motg->phy, "SET PWR PROPERTY", psp, psy->type);
	switch (psp) {
	case POWER_SUPPLY_PROP_USB_OTG:
		if (!test_and_set_bit(ID, &motg->inputs))
			printk(KERN_WARNING "PMIC: ID set\n");
		
		
		break;
	
	case POWER_SUPPLY_PROP_DP_DM:
		msm_otg_pmic_dp_dm(motg, val->intval);
		break;
	
	case POWER_SUPPLY_PROP_PRESENT:
		USB_INFO("check_vbus_in: %d -> %d\n", htc_vbus_active, val->intval);
		htc_vbus_active = val->intval;
		msm_otg_set_vbus_state(val->intval);
		break;
	
	case POWER_SUPPLY_PROP_ONLINE:
		USB_INFO("%s: online %d -> %d\n", __func__, motg->online, val->intval);
		motg->online = val->intval;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		motg->voltage_max = val->intval;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		motg->current_max = val->intval;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_MAX:
		motg->typec_current_max = val->intval;
		msm_otg_dbg_log_event(&motg->phy, "type-c charger",
					val->intval, motg->bc1p2_current_max);
		
		if (motg->chg_type != USB_INVALID_CHARGER) {
			dev_dbg(motg->phy.dev, "update type-c charger\n");
			msm_otg_notify_charger(motg, motg->bc1p2_current_max);
		}
		break;
	case POWER_SUPPLY_PROP_TYPE:
		psy->type = val->intval;

		if (motg->chg_state == USB_CHG_STATE_DETECTED)
			break;

		switch (psy->type) {
		case POWER_SUPPLY_TYPE_USB:
			motg->chg_type = USB_SDP_CHARGER;
			break;
		case POWER_SUPPLY_TYPE_USB_DCP:
			motg->chg_type = USB_DCP_CHARGER;
			break;
		case POWER_SUPPLY_TYPE_USB_HVDCP:
			motg->chg_type = USB_DCP_CHARGER;
			msm_otg_notify_charger(motg, hvdcp_max_current);
			break;
		case POWER_SUPPLY_TYPE_USB_CDP:
			motg->chg_type = USB_CDP_CHARGER;
			break;
		case POWER_SUPPLY_TYPE_USB_ACA:
			motg->chg_type = USB_PROPRIETARY_CHARGER;
			break;
		default:
			motg->chg_type = USB_INVALID_CHARGER;
			break;
		}

		if (motg->chg_type != USB_INVALID_CHARGER) {
			if (motg->chg_type == USB_DCP_CHARGER)
				motg->is_ext_chg_dcp = true;
			motg->chg_state = USB_CHG_STATE_DETECTED;
		}
		printk(KERN_WARNING "msm_otg ext chg open\n");
		USB_INFO("%s: charger type = %s\n", __func__,
			chg_to_string(motg->chg_type));
		msm_otg_dbg_log_event(&motg->phy, "SET CHARGER TYPE ",
				motg->chg_type, psy->type);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		motg->usbin_health = val->intval;
		break;
	default:
		return -EINVAL;
	}

	power_supply_changed(&motg->usb_psy);
	return 0;
}

static int otg_power_property_is_writeable_usb(struct power_supply *psy,
						enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_HEALTH:
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
	case POWER_SUPPLY_PROP_DP_DM:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_MAX:
	case POWER_SUPPLY_PROP_USB_OTG:
		return 1;
	default:
		break;
	}

	return 0;
}

static char *otg_pm_power_supplied_to[] = {
	"battery",
};

static enum power_supply_property otg_pm_power_props_usb[] = {
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_MAX,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_DP_DM,
	POWER_SUPPLY_PROP_USB_OTG,
};

const struct file_operations msm_otg_bus_fops = {
	.open = msm_otg_bus_open,
	.read = seq_read,
	.write = msm_otg_bus_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct dentry *msm_otg_dbg_root;

static int msm_otg_debugfs_init(struct msm_otg *motg)
{
	struct dentry *msm_otg_dentry;
	struct msm_otg_platform_data *pdata = motg->pdata;

	msm_otg_dbg_root = debugfs_create_dir("msm_otg", NULL);

	if (!msm_otg_dbg_root || IS_ERR(msm_otg_dbg_root))
		return -ENODEV;

	if ((pdata->mode == USB_OTG || pdata->mode == USB_PERIPHERAL) &&
		pdata->otg_control == OTG_USER_CONTROL) {

		msm_otg_dentry = debugfs_create_file("mode", S_IRUGO |
			S_IWUSR, msm_otg_dbg_root, motg,
			&msm_otg_mode_fops);

		if (!msm_otg_dentry) {
			debugfs_remove(msm_otg_dbg_root);
			msm_otg_dbg_root = NULL;
			return -ENODEV;
		}
	}

	msm_otg_dentry = debugfs_create_file("chg_type", S_IRUGO,
		msm_otg_dbg_root, motg,
		&msm_otg_chg_fops);

	if (!msm_otg_dentry) {
		debugfs_remove_recursive(msm_otg_dbg_root);
		return -ENODEV;
	}

	msm_otg_dentry = debugfs_create_file("aca", S_IRUGO | S_IWUSR,
		msm_otg_dbg_root, motg,
		&msm_otg_aca_fops);

	if (!msm_otg_dentry) {
		debugfs_remove_recursive(msm_otg_dbg_root);
		return -ENODEV;
	}

	msm_otg_dentry = debugfs_create_file("bus_voting", S_IRUGO | S_IWUSR,
		msm_otg_dbg_root, motg,
		&msm_otg_bus_fops);

	if (!msm_otg_dentry) {
		debugfs_remove_recursive(msm_otg_dbg_root);
		return -ENODEV;
	}

	msm_otg_dentry = debugfs_create_file("otg_state", S_IRUGO,
				msm_otg_dbg_root, motg, &msm_otg_state_fops);

	if (!msm_otg_dentry) {
		debugfs_remove_recursive(msm_otg_dbg_root);
		return -ENODEV;
	}

	msm_otg_dentry = debugfs_create_file("dbg_buff", S_IRUGO,
		msm_otg_dbg_root, motg, &msm_otg_dbg_buff_fops);

	if (!msm_otg_dentry) {
		debugfs_remove_recursive(msm_otg_dbg_root);
		return -ENODEV;
	}
	return 0;
}

static void msm_otg_debugfs_cleanup(void)
{
	debugfs_remove_recursive(msm_otg_dbg_root);
}

#define MSM_OTG_CMD_ID		0x09
#define MSM_OTG_DEVICE_ID	0x04
#define MSM_OTG_VMID_IDX	0xFF
#define MSM_OTG_MEM_TYPE	0x02
struct msm_otg_scm_cmd_buf {
	unsigned int device_id;
	unsigned int vmid_idx;
	unsigned int mem_type;
} __attribute__ ((__packed__));

static void msm_otg_pnoc_errata_fix(struct msm_otg *motg)
{
	int ret;
	struct msm_otg_platform_data *pdata = motg->pdata;
	struct msm_otg_scm_cmd_buf cmd_buf;

	if (!pdata->pnoc_errata_fix)
		return;

	dev_dbg(motg->phy.dev, "applying fix for pnoc h/w issue\n");

	cmd_buf.device_id = MSM_OTG_DEVICE_ID;
	cmd_buf.vmid_idx = MSM_OTG_VMID_IDX;
	cmd_buf.mem_type = MSM_OTG_MEM_TYPE;

	ret = scm_call(SCM_SVC_MP, MSM_OTG_CMD_ID, &cmd_buf,
				sizeof(cmd_buf), NULL, 0);

	if (ret)
		dev_err(motg->phy.dev, "scm command failed to update VMIDMT\n");
}

void msm_otg_set_id_state(int id)
{
	struct msm_otg *motg = the_msm_otg;
	msm_id_backup = id;
	if (id) {
		USB_INFO("PMIC: ID set\n");
		if(msm_otg_usb_disable) 
			return;
		set_bit(ID, &motg->inputs);
	} else {
		USB_INFO("PMIC: ID clear\n");
		if(msm_otg_usb_disable) 
			return;
		clear_bit(ID, &motg->inputs);
	}

	if (motg->phy.state != OTG_STATE_UNDEFINED) {
		
		wake_lock_timeout(&motg->cable_detect_wlock, 3 * HZ);

		queue_delayed_work(motg->otg_wq, &motg->id_status_work,
				msecs_to_jiffies(MSM_ID_STATUS_DELAY));

	}
}

static void usb_host_cable_detect(bool cable_in)
{
	if (cable_in)
		msm_otg_set_id_state(0);
	else
		msm_otg_set_id_state(1);
}

static struct t_usb_host_status_notifier usb_host_status_notifier = {
	.name = "usb_host",
	.func = usb_host_cable_detect,
};
static u64 msm_otg_dma_mask = DMA_BIT_MASK(32);
static struct platform_device *msm_otg_add_pdev(
		struct platform_device *ofdev, const char *name)
{
	struct platform_device *pdev;
	const struct resource *res = ofdev->resource;
	unsigned int num = ofdev->num_resources;
	int retval;
	struct ci13xxx_platform_data ci_pdata;
	struct msm_otg_platform_data *otg_pdata;
	struct msm_otg *motg;

	pdev = platform_device_alloc(name, -1);
	if (!pdev) {
		retval = -ENOMEM;
		goto error;
	}

	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	pdev->dev.dma_mask = &msm_otg_dma_mask;

	if (num) {
		retval = platform_device_add_resources(pdev, res, num);
		if (retval)
			goto error;
	}

	if (!strcmp(name, "msm_hsusb")) {
		otg_pdata =
			(struct msm_otg_platform_data *)
				ofdev->dev.platform_data;
		motg = platform_get_drvdata(ofdev);
		ci_pdata.log2_itc = otg_pdata->log2_itc;
		ci_pdata.usb_core_id = 0;
		ci_pdata.l1_supported = otg_pdata->l1_supported;
		ci_pdata.enable_ahb2ahb_bypass =
				otg_pdata->enable_ahb2ahb_bypass;
		ci_pdata.system_clk = otg_pdata->system_clk;
		ci_pdata.pclk = otg_pdata->pclk;
		ci_pdata.enable_streaming = otg_pdata->enable_streaming;
		ci_pdata.enable_axi_prefetch = otg_pdata->enable_axi_prefetch;
		ci_pdata.max_nominal_system_clk_rate =
					motg->max_nominal_system_clk_rate;
		ci_pdata.default_system_clk_rate = motg->core_clk_rate;
		retval = platform_device_add_data(pdev, &ci_pdata,
			sizeof(ci_pdata));
		if (retval)
			goto error;
	}

	retval = platform_device_add(pdev);
	if (retval)
		goto error;

	return pdev;

error:
	platform_device_put(pdev);
	return ERR_PTR(retval);
}

static int msm_otg_setup_devices(struct platform_device *ofdev,
		enum usb_mode_type mode, bool init)
{
	const char *gadget_name = "msm_hsusb";
	const char *host_name = "msm_hsusb_host";
	static struct platform_device *gadget_pdev;
	static struct platform_device *host_pdev;
	int retval = 0;

	if (!init) {
		if (gadget_pdev)
			platform_device_unregister(gadget_pdev);
		if (host_pdev)
			platform_device_unregister(host_pdev);
		return 0;
	}

	switch (mode) {
	case USB_OTG:
		
	case USB_PERIPHERAL:
		gadget_pdev = msm_otg_add_pdev(ofdev, gadget_name);
		if (IS_ERR(gadget_pdev)) {
			retval = PTR_ERR(gadget_pdev);
			break;
		}
		if (mode == USB_PERIPHERAL)
			break;
		
	case USB_HOST:
		host_pdev = msm_otg_add_pdev(ofdev, host_name);
		if (IS_ERR(host_pdev)) {
			retval = PTR_ERR(host_pdev);
			if (mode == USB_OTG)
				platform_device_unregister(gadget_pdev);
		}
		break;
	default:
		break;
	}

	return retval;
}

static int msm_otg_register_power_supply(struct platform_device *pdev,
					struct msm_otg *motg)
{
	int ret;

	ret = power_supply_register(&pdev->dev, &motg->usb_psy);
	if (ret < 0) {
		dev_err(motg->phy.dev,
			"%s:power_supply_register usb failed\n",
			__func__);
		return ret;
	}

	legacy_power_supply = false;
	return 0;
}

static int msm_otg_ext_chg_open(struct inode *inode, struct file *file)
{
	struct msm_otg *motg = the_msm_otg;

	printk(KERN_WARNING "msm_otg ext chg open\n");
	msm_otg_dbg_log_event(&motg->phy, "EXT CHG: OPEN",
			motg->inputs, motg->phy.state);

	motg->ext_chg_opened = true;
	file->private_data = (void *)motg;
	return 0;
}

static long
msm_otg_ext_chg_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct msm_otg *motg = file->private_data;
	struct msm_usb_chg_info info = {0};
	int ret = 0, val;

	msm_otg_dbg_log_event(&motg->phy, "EXT CHG: IOCTL", cmd, 0);
	switch (cmd) {
	case MSM_USB_EXT_CHG_INFO:
		info.chg_block_type = USB_CHG_BLOCK_ULPI;
		info.page_offset = motg->io_res->start & ~PAGE_MASK;
		
		info.length = PAGE_SIZE;

		if (copy_to_user((void __user *)arg, &info, sizeof(info))) {
			pr_err("%s: copy to user failed\n\n", __func__);
			ret = -EFAULT;
		}
		break;
	case MSM_USB_EXT_CHG_BLOCK_LPM:
		if (get_user(val, (int __user *)arg)) {
			pr_err("%s: get_user failed\n\n", __func__);
			ret = -EFAULT;
			break;
		}
		printk(KERN_WARNING "%s: LPM block request %d\n", __func__, val);
		msm_otg_dbg_log_event(&motg->phy, "LPM BLOCK REQ", val, 0);
		if (val) { 
			if (motg->chg_type == USB_DCP_CHARGER) {
				motg->ext_chg_active = ACTIVE;
				if (pm_runtime_suspended(motg->phy.dev))
					pm_runtime_resume(motg->phy.dev);
				else {
					msm_otg_dbg_log_event(&motg->phy,
					"PM RUNTIME: EXT_CHG GET",
					get_pm_runtime_counter(motg->phy.dev),
					0);
					pm_runtime_get_sync(motg->phy.dev);
				}
			} else {
				motg->ext_chg_active = INACTIVE;
				complete(&motg->ext_chg_wait);
				ret = -ENODEV;
			}
		} else {
			motg->ext_chg_active = INACTIVE;
			complete(&motg->ext_chg_wait);
			flush_work(&motg->sm_work);
			msm_otg_dbg_log_event(&motg->phy,
					"PM RUNTIME: EXT_CHG PUT",
					get_pm_runtime_counter(motg->phy.dev),
					motg->pm_done);
			pm_runtime_put_noidle(motg->phy.dev);
			motg->pm_done = 1;
			pm_runtime_suspend(motg->phy.dev);
		}
		break;
	case MSM_USB_EXT_CHG_VOLTAGE_INFO:
		if (get_user(val, (int __user *)arg)) {
			pr_err("%s: get_user failed\n\n", __func__);
			ret = -EFAULT;
			break;
		}
		msm_otg_dbg_log_event(&motg->phy, "EXT CHG: VOL REQ", cmd, val);

		if (val == USB_REQUEST_5V)
			printk(KERN_WARNING "%s:voting 5V voltage request\n", __func__);
		else if (val == USB_REQUEST_9V)
			printk(KERN_WARNING "%s:voting 9V voltage request\n", __func__);
		break;
	case MSM_USB_EXT_CHG_RESULT:
		if (get_user(val, (int __user *)arg)) {
			pr_err("%s: get_user failed\n\n", __func__);
			ret = -EFAULT;
			break;
		}
		msm_otg_dbg_log_event(&motg->phy, "EXT CHG: VOL REQ", cmd, val);

		if (!val)
			printk(KERN_WARNING "%s:voltage request successful\n", __func__);
		else
			printk(KERN_WARNING "%s:voltage request failed\n", __func__);
		break;
	case MSM_USB_EXT_CHG_TYPE:
		if (get_user(val, (int __user *)arg)) {
			pr_err("%s: get_user failed\n\n", __func__);
			ret = -EFAULT;
			break;
		}
		msm_otg_dbg_log_event(&motg->phy, "EXT CHG: VOL REQ", cmd, val);

		if (val)
			printk(KERN_WARNING "%s:charger is external charger\n", __func__);
		else
			printk(KERN_WARNING "%s:charger is not ext charger\n", __func__);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int msm_otg_ext_chg_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct msm_otg *motg = file->private_data;
	unsigned long vsize = vma->vm_end - vma->vm_start;
	int ret;

	if (vma->vm_pgoff || vsize > PAGE_SIZE)
		return -EINVAL;

	vma->vm_pgoff = __phys_to_pfn(motg->io_res->start);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	ret = io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
				 vsize, vma->vm_page_prot);
	if (ret < 0) {
		pr_err("%s: failed with return val %d\n", __func__, ret);
		return ret;
	}

	return 0;
}

static int msm_otg_ext_chg_release(struct inode *inode, struct file *file)
{
	struct msm_otg *motg = file->private_data;

	printk(KERN_WARNING "msm_otg ext chg release\n");
	msm_otg_dbg_log_event(&motg->phy, "EXT CHG: RELEASE",
			motg->inputs, motg->phy.state);

	motg->ext_chg_opened = false;

	return 0;
}

static const struct file_operations msm_otg_ext_chg_fops = {
	.owner = THIS_MODULE,
	.open = msm_otg_ext_chg_open,
	.unlocked_ioctl = msm_otg_ext_chg_ioctl,
	.mmap = msm_otg_ext_chg_mmap,
	.release = msm_otg_ext_chg_release,
};

static int msm_otg_setup_ext_chg_cdev(struct msm_otg *motg)
{
	int ret;

	if (motg->pdata->enable_sec_phy || motg->pdata->mode == USB_HOST ||
			motg->pdata->otg_control != OTG_PMIC_CONTROL ||
			psy != &motg->usb_psy) {
		printk(KERN_WARNING "usb ext chg is not supported by msm otg\n");
		return -ENODEV;
	}

	ret = alloc_chrdev_region(&motg->ext_chg_dev, 0, 1, "usb_ext_chg");
	if (ret < 0) {
		pr_err("Fail to allocate usb ext char dev region\n");
		return ret;
	}
	motg->ext_chg_class = class_create(THIS_MODULE, "msm_ext_chg");
	if (ret < 0) {
		pr_err("Fail to create usb ext chg class\n");
		goto unreg_chrdev;
	}
	cdev_init(&motg->ext_chg_cdev, &msm_otg_ext_chg_fops);
	motg->ext_chg_cdev.owner = THIS_MODULE;

	ret = cdev_add(&motg->ext_chg_cdev, motg->ext_chg_dev, 1);
	if (ret < 0) {
		pr_err("Fail to add usb ext chg cdev\n");
		goto destroy_class;
	}
	motg->ext_chg_device = device_create(motg->ext_chg_class,
					NULL, motg->ext_chg_dev, NULL,
					"usb_ext_chg");
	if (IS_ERR(motg->ext_chg_device)) {
		pr_err("Fail to create usb ext chg device\n");
		ret = PTR_ERR(motg->ext_chg_device);
		motg->ext_chg_device = NULL;
		goto del_cdev;
	}

	init_completion(&motg->ext_chg_wait);
	printk(KERN_WARNING "msm otg ext chg cdev setup success\n");
	return 0;

del_cdev:
	cdev_del(&motg->ext_chg_cdev);
destroy_class:
	class_destroy(motg->ext_chg_class);
unreg_chrdev:
	unregister_chrdev_region(motg->ext_chg_dev, 1);

	return ret;
}

static ssize_t dpdm_pulldown_enable_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct msm_otg *motg = the_msm_otg;
	struct msm_otg_platform_data *pdata = motg->pdata;

	return snprintf(buf, PAGE_SIZE, "%s\n", pdata->dpdm_pulldown_added ?
							"enabled" : "disabled");
}

static ssize_t dpdm_pulldown_enable_store(struct device *dev,
		struct device_attribute *attr, const char
		*buf, size_t size)
{
	struct msm_otg *motg = the_msm_otg;
	struct msm_otg_platform_data *pdata = motg->pdata;

	if (!strnicmp(buf, "enable", 6)) {
		pdata->dpdm_pulldown_added = true;
		return size;
	} else if (!strnicmp(buf, "disable", 7)) {
		pdata->dpdm_pulldown_added = false;
		return size;
	}

	return -EINVAL;
}

static DEVICE_ATTR(dpdm_pulldown_enable, S_IRUGO | S_IWUSR,
		dpdm_pulldown_enable_show, dpdm_pulldown_enable_store);

struct msm_otg_platform_data *msm_otg_dt_to_pdata(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct msm_otg_platform_data *pdata;
	int len = 0;
	int res_gpio;

	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		pr_err("unable to allocate platform data\n");
		return NULL;
	}
	of_get_property(node, "qcom,hsusb-otg-phy-init-seq", &len);
	if (len) {
		pdata->phy_init_seq = devm_kzalloc(&pdev->dev, len, GFP_KERNEL);
		if (!pdata->phy_init_seq)
			return NULL;
		of_property_read_u32_array(node, "qcom,hsusb-otg-phy-init-seq",
				pdata->phy_init_seq,
				len/sizeof(*pdata->phy_init_seq));
	}
	of_property_read_u32(node, "qcom,hsusb-otg-power-budget",
				&pdata->power_budget);
	of_property_read_u32(node, "qcom,hsusb-otg-mode",
				&pdata->mode);
	of_property_read_u32(node, "qcom,hsusb-otg-otg-control",
				&pdata->otg_control);
	of_property_read_u32(node, "qcom,hsusb-otg-default-mode",
				&pdata->default_mode);
	of_property_read_u32(node, "qcom,hsusb-otg-phy-type",
				&pdata->phy_type);
	pdata->disable_reset_on_disconnect = of_property_read_bool(node,
				"qcom,hsusb-otg-disable-reset");
	pdata->pnoc_errata_fix = of_property_read_bool(node,
				"qcom,hsusb-otg-pnoc-errata-fix");
	pdata->enable_lpm_on_dev_suspend = of_property_read_bool(node,
				"qcom,hsusb-otg-lpm-on-dev-suspend");
	pdata->core_clk_always_on_workaround = of_property_read_bool(node,
				"qcom,hsusb-otg-clk-always-on-workaround");
	pdata->delay_lpm_on_disconnect = of_property_read_bool(node,
				"qcom,hsusb-otg-delay-lpm");
	pdata->dp_manual_pullup = of_property_read_bool(node,
				"qcom,dp-manual-pullup");
	pdata->enable_sec_phy = of_property_read_bool(node,
					"qcom,usb2-enable-hsphy2");
	of_property_read_u32(node, "qcom,hsusb-log2-itc",
				&pdata->log2_itc);

	of_property_read_u32(node, "qcom,hsusb-otg-mpm-dpsehv-int",
				&pdata->mpm_dpshv_int);
	of_property_read_u32(node, "qcom,hsusb-otg-mpm-dmsehv-int",
				&pdata->mpm_dmshv_int);
	pdata->pmic_id_irq = platform_get_irq_byname(pdev, "pmic_id_irq");
	if (pdata->pmic_id_irq < 0)
		pdata->pmic_id_irq = 0;

	pdata->hub_reset_gpio = of_get_named_gpio(
			node, "qcom,hub-reset-gpio", 0);
	if (pdata->hub_reset_gpio < 0)
		pr_debug("hub_reset_gpio is not available\n");

	pdata->switch_sel_gpio =
			of_get_named_gpio(node, "qcom,sw-sel-gpio", 0);
	if (pdata->switch_sel_gpio < 0)
		pr_debug("switch_sel_gpio is not available\n");

	pdata->usb_id_gpio =
			of_get_named_gpio(node, "qcom,usbid-gpio", 0);
	if (pdata->usb_id_gpio < 0)
		pr_debug("usb_id_gpio is not available\n");

	pdata->l1_supported = of_property_read_bool(node,
				"qcom,hsusb-l1-supported");
	pdata->enable_ahb2ahb_bypass = of_property_read_bool(node,
				"qcom,ahb-async-bridge-bypass");
	pdata->disable_retention_with_vdd_min = of_property_read_bool(node,
				"qcom,disable-retention-with-vdd-min");
	pdata->phy_dvdd_always_on = of_property_read_bool(node,
				"qcom,phy-dvdd-always-on");

	res_gpio = of_get_named_gpio(node, "qcom,hsusb-otg-vddmin-gpio", 0);
	if (res_gpio < 0)
		res_gpio = 0;
	pdata->vddmin_gpio = res_gpio;

	pdata->rw_during_lpm_workaround = of_property_read_bool(node,
				"qcom,hsusb-otg-rw-during-lpm-workaround");

	pdata->emulation = of_property_read_bool(node,
						"qcom,emulation");

	pdata->enable_streaming = of_property_read_bool(node,
					"qcom,boost-sysclk-with-streaming");

	pdata->enable_axi_prefetch = of_property_read_bool(node,
						"qcom,axi-prefetch-enable");
	return pdata;
}

static int msm_otg_probe(struct platform_device *pdev)
{
	int ret = 0;
	int len = 0;
	u32 tmp[3];
	struct resource *res;
	struct msm_otg *motg;
	struct usb_phy *phy;
	struct msm_otg_platform_data *pdata;
	void __iomem *tcsr;
	int id_irq = 0;

	dev_info(&pdev->dev, "msm_otg probe\n");

	motg = kzalloc(sizeof(struct msm_otg), GFP_KERNEL);
	if (!motg) {
		dev_err(&pdev->dev, "unable to allocate msm_otg\n");
		ret = -ENOMEM;
		return ret;
	}

	motg->core_clk = clk_get(&pdev->dev, "core_clk");
	if (IS_ERR(motg->core_clk)) {
		ret = PTR_ERR(motg->core_clk);
		motg->core_clk = NULL;
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "failed to get core_clk\n");
		goto free_motg;
	}

	if (of_property_read_u32(pdev->dev.of_node,
					"qcom,max-nominal-sysclk-rate",
					&motg->max_nominal_system_clk_rate)) {
		ret = -EINVAL;
		goto put_core_clk;
	}

	if (of_property_read_bool(pdev->dev.of_node,
					"qcom,boost-sysclk-with-streaming"))
		motg->core_clk_rate = clk_round_rate(motg->core_clk,
					motg->max_nominal_system_clk_rate);
	else
		motg->core_clk_rate = clk_round_rate(motg->core_clk,
						USB_DEFAULT_SYSTEM_CLOCK);
	if (IS_ERR_VALUE(motg->core_clk_rate)) {
		dev_err(&pdev->dev, "fail to get core clk max freq.\n");
	} else {
		ret = clk_set_rate(motg->core_clk, motg->core_clk_rate);
		if (ret)
			dev_err(&pdev->dev, "fail to set core_clk freq:%d\n",
									ret);
	}

	motg->pclk = clk_get(&pdev->dev, "iface_clk");
	if (IS_ERR(motg->pclk)) {
		ret = PTR_ERR(motg->pclk);
		motg->pclk = NULL;
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "failed to get iface_clk\n");
		goto put_core_clk;
	}

	motg->xo_clk = clk_get(&pdev->dev, "xo");
	if (IS_ERR(motg->xo_clk)) {
		ret = PTR_ERR(motg->xo_clk);
		motg->xo_clk = NULL;
		if (ret == -EPROBE_DEFER)
			goto put_pclk;
	}

	motg->sleep_clk = devm_clk_get(&pdev->dev, "sleep_clk");
	if (IS_ERR(motg->sleep_clk)) {
		ret = PTR_ERR(motg->sleep_clk);
		motg->sleep_clk = NULL;
		if (ret == -EPROBE_DEFER)
			goto put_xo_clk;
		else
			dev_dbg(&pdev->dev, "failed to get sleep_clk\n");
	} else {
		ret = clk_prepare_enable(motg->sleep_clk);
		if (ret) {
			dev_err(&pdev->dev, "%s failed to vote sleep_clk%d\n",
						__func__, ret);
			goto put_xo_clk;
		}
	}


	if (of_property_match_string(pdev->dev.of_node,
			"clock-names", "phy_reset_clk") >= 0) {
		motg->phy_reset_clk = devm_clk_get(&pdev->dev, "phy_reset_clk");
		if (IS_ERR(motg->phy_reset_clk)) {
			ret = PTR_ERR(motg->phy_reset_clk);
			goto disable_sleep_clk;
		}
	}

	if (of_property_match_string(pdev->dev.of_node,
				"clock-names", "phy_por_clk") >= 0) {
		motg->phy_por_clk = devm_clk_get(&pdev->dev, "phy_por_clk");
		if (IS_ERR(motg->phy_por_clk)) {
			ret = PTR_ERR(motg->phy_por_clk);
			goto disable_sleep_clk;
		}
	}

	if (of_property_match_string(pdev->dev.of_node,
				"clock-names", "phy_csr_clk") >= 0) {
		motg->phy_csr_clk = devm_clk_get(&pdev->dev, "phy_csr_clk");
		if (IS_ERR(motg->phy_csr_clk)) {
			ret = PTR_ERR(motg->phy_csr_clk);
			goto disable_sleep_clk;
		} else {
			ret = clk_prepare_enable(motg->phy_csr_clk);
			if (ret) {
				dev_err(&pdev->dev,
					"fail to enable phy csr clk %d\n", ret);
				goto disable_sleep_clk;
			}
		}
	}

	if (pdev->dev.of_node) {
		dev_dbg(&pdev->dev, "device tree enabled\n");
		pdata = msm_otg_dt_to_pdata(pdev);
		if (!pdata) {
			ret = -ENOMEM;
			goto disable_phy_csr_clk;
		}

		pdata->bus_scale_table = msm_bus_cl_get_pdata(pdev);
		if (!pdata->bus_scale_table)
			dev_dbg(&pdev->dev, "bus scaling is disabled\n");

		pdev->dev.platform_data = pdata;
	} else if (!pdev->dev.platform_data) {
		dev_err(&pdev->dev, "No platform data given. Bailing out\n");
		ret = -ENODEV;
		goto disable_phy_csr_clk;
	} else {
		pdata = pdev->dev.platform_data;
	}

	if (pdata->phy_type == QUSB_ULPI_PHY) {
		if (of_property_match_string(pdev->dev.of_node,
					"clock-names", "phy_ref_clk") >= 0) {
			motg->phy_ref_clk = devm_clk_get(&pdev->dev,
						"phy_ref_clk");
			if (IS_ERR(motg->phy_ref_clk)) {
				ret = PTR_ERR(motg->phy_ref_clk);
				goto disable_phy_csr_clk;
			} else {
				ret = clk_prepare_enable(motg->phy_ref_clk);
				if (ret) {
					dev_err(&pdev->dev,
						"fail to enable phy ref clk %d\n",
						ret);
					goto disable_phy_csr_clk;
				}
			}
		}
	}

	motg->phy.otg = devm_kzalloc(&pdev->dev, sizeof(struct usb_otg),
							GFP_KERNEL);
	if (!motg->phy.otg) {
		dev_err(&pdev->dev, "unable to allocate usb_otg\n");
		ret = -ENOMEM;
		goto otg_remove_devices;
	}

	the_msm_otg = motg;
	motg->pdata = pdata;
	phy = &motg->phy;
	phy->dev = &pdev->dev;
	motg->dbg_idx = 0;
	motg->dbg_lock = __RW_LOCK_UNLOCKED(lck);

	if (motg->pdata->bus_scale_table) {
		motg->bus_perf_client =
		    msm_bus_scale_register_client(motg->pdata->bus_scale_table);
		if (!motg->bus_perf_client) {
			dev_err(motg->phy.dev, "%s: Failed to register BUS\n"
						"scaling client!!\n", __func__);
		} else {
			debug_bus_voting_enabled = true;
			
			msm_otg_bus_vote(motg, USB_MIN_PERF_VOTE);
		}
	}

	pdata->system_clk = motg->core_clk;
	pdata->pclk = motg->pclk;

	ret = msm_otg_bus_freq_get(motg->phy.dev, motg);
	if (ret)
		pr_err("failed to vote for explicit noc rates: %d\n", ret);

	if (aca_enabled() && motg->pdata->otg_control != OTG_PMIC_CONTROL) {
		dev_err(&pdev->dev, "ACA can not be enabled without PMIC\n");
		ret = -EINVAL;
		goto devote_bus_bw;
	}

	
	motg->reset_counter = 0;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "core");
	if (!res) {
		dev_err(&pdev->dev, "failed to get core iomem resource\n");
		ret = -ENODEV;
		goto devote_bus_bw;
	}

	motg->io_res = res;
	motg->regs = ioremap(res->start, resource_size(res));
	if (!motg->regs) {
		dev_err(&pdev->dev, "core iomem ioremap failed\n");
		ret = -ENOMEM;
		goto devote_bus_bw;
	}
	printk(KERN_WARNING "[USB] OTG regs = %p\n", motg->regs);

	if (pdata->enable_sec_phy) {
		res = platform_get_resource_byname(pdev,
				IORESOURCE_MEM, "tcsr");
		if (!res) {
			dev_dbg(&pdev->dev, "missing TCSR memory resource\n");
		} else {
			tcsr = devm_ioremap_nocache(&pdev->dev, res->start,
				resource_size(res));
			if (!tcsr) {
				dev_dbg(&pdev->dev, "tcsr ioremap failed\n");
			} else {
				
				writel_relaxed(0x1, tcsr);
				mb();
			}
		}
	}

	if (pdata->enable_sec_phy)
		motg->usb_phy_ctrl_reg = USB_PHY_CTRL2;
	else
		motg->usb_phy_ctrl_reg = USB_PHY_CTRL;

	if (motg->pdata->phy_type == SNPS_FEMTO_PHY ||
		pdata->phy_type == QUSB_ULPI_PHY) {
		res = platform_get_resource_byname(pdev,
				IORESOURCE_MEM, "phy_csr");
		if (!res) {
			dev_err(&pdev->dev, "PHY CSR IOMEM missing!\n");
			ret = -ENODEV;
			goto free_regs;
		}
		motg->phy_csr_regs = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(motg->phy_csr_regs)) {
			ret = PTR_ERR(motg->phy_csr_regs);
			dev_err(&pdev->dev, "PHY CSR ioremap failed!\n");
			goto free_regs;
		}
		motg->usb_phy_ctrl_reg = 0;
	}

	motg->irq = platform_get_irq(pdev, 0);
	if (!motg->irq) {
		dev_err(&pdev->dev, "platform_get_irq failed\n");
		ret = -ENODEV;
		goto free_regs;
	}

	motg->async_irq = platform_get_irq_byname(pdev, "async_irq");
	if (motg->async_irq < 0) {
		dev_err(&pdev->dev, "platform_get_irq for async_int failed\n");
		motg->async_irq = 0;
		goto free_regs;
	}

	if (motg->xo_clk) {
		ret = clk_prepare_enable(motg->xo_clk);
		if (ret) {
			dev_err(&pdev->dev,
				"%s failed to vote for TCXO %d\n",
					__func__, ret);
			goto free_xo_handle;
		}
	}


	clk_prepare_enable(motg->pclk);

	hsusb_vdd = devm_regulator_get(motg->phy.dev, "hsusb_vdd_dig");
	if (IS_ERR(hsusb_vdd)) {
		hsusb_vdd = devm_regulator_get(motg->phy.dev, "HSUSB_VDDCX");
		if (IS_ERR(hsusb_vdd)) {
			dev_err(motg->phy.dev, "unable to get hsusb vddcx\n");
			ret = PTR_ERR(hsusb_vdd);
			goto devote_xo_handle;
		}
	}

	if (of_get_property(pdev->dev.of_node,
			"qcom,vdd-voltage-level",
			&len)){
		if (len == sizeof(tmp)) {
			of_property_read_u32_array(pdev->dev.of_node,
					"qcom,vdd-voltage-level",
					tmp, len/sizeof(*tmp));
			vdd_val[0] = tmp[0];
			vdd_val[1] = tmp[1];
			vdd_val[2] = tmp[2];
		} else {
			dev_dbg(&pdev->dev,
				"Using default hsusb vdd config.\n");
			goto devote_xo_handle;
		}
	} else {
		goto devote_xo_handle;
	}

	ret = msm_hsusb_config_vddcx(1);
	if (ret) {
		dev_err(&pdev->dev, "hsusb vddcx configuration failed\n");
		goto devote_xo_handle;
	}

	ret = regulator_enable(hsusb_vdd);
	if (ret) {
		dev_err(&pdev->dev, "unable to enable the hsusb vddcx\n");
		goto free_config_vddcx;
	}

	ret = msm_hsusb_ldo_init(motg, 1);
	if (ret) {
		dev_err(&pdev->dev, "hsusb vreg configuration failed\n");
		goto free_hsusb_vdd;
	}

	
	motg->phy_pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(motg->phy_pinctrl)) {
		if (of_property_read_bool(pdev->dev.of_node, "pinctrl-names")) {
			printk("Error encountered while getting pinctrl\n");
			ret = PTR_ERR(motg->phy_pinctrl);
			goto free_ldo_init;
		}
		dev_dbg(&pdev->dev, "Target does not use pinctrl\n");
		motg->phy_pinctrl = NULL;
	}

	if (motg->phy_pinctrl != NULL) {
		motg->gpio_state_init = pinctrl_lookup_state(motg->phy_pinctrl, "usbid_default_init");
		if (IS_ERR_OR_NULL(motg->gpio_state_init)) {
			printk("[USB] %s: can't get the pinctrl state\n", __func__);
			ret = PTR_ERR(motg->gpio_state_init);
			motg->phy_pinctrl = NULL;
		} else {
			ret = pinctrl_select_state(motg->phy_pinctrl, motg->gpio_state_init);
			if (ret)
				printk("[USB] %s: can't init GPIO!\n",__func__);
		}
	}

	if (pdata->mhl_enable) {
		mhl_usb_hs_switch = devm_regulator_get(motg->phy.dev,
							"mhl_usb_hs_switch");
		if (IS_ERR(mhl_usb_hs_switch)) {
			printk("Unable to get mhl_usb_hs_switch\n");
			ret = PTR_ERR(mhl_usb_hs_switch);
			goto free_ldo_init;
		}
	}

	ret = msm_hsusb_ldo_enable(motg, USB_PHY_REG_ON);
	if (ret) {
		dev_err(&pdev->dev, "hsusb vreg enable failed\n");
		goto free_ldo_init;
	}
	clk_prepare_enable(motg->core_clk);

	
	msm_otg_pnoc_errata_fix(motg);

	writel(0, USB_USBINTR);
	writel(0, USB_OTGSC);
	
	mb();

	motg->id_state = USB_ID_FLOAT;
	ret = msm_otg_mhl_register_callback(motg, msm_otg_mhl_notify_online);
	if (ret)
		dev_dbg(&pdev->dev, "MHL can not be supported\n");
	wake_lock_init(&motg->wlock, WAKE_LOCK_SUSPEND, "msm_otg");
	wake_lock_init(&motg->cable_detect_wlock, WAKE_LOCK_SUSPEND, "msm_usb_cable");
	msm_otg_init_timer(motg);
	INIT_WORK(&motg->sm_work, msm_otg_sm_work);
	INIT_DELAYED_WORK(&motg->chg_work, msm_chg_detect_work);
	INIT_DELAYED_WORK(&motg->id_status_work, msm_id_status_w);
	INIT_DELAYED_WORK(&motg->suspend_work, msm_otg_suspend_work);
	setup_timer(&motg->id_timer, msm_otg_id_timer_func,
				(unsigned long) motg);
	setup_timer(&motg->chg_check_timer, msm_otg_chg_check_timer_func,
				(unsigned long) motg);
	motg->otg_wq = alloc_ordered_workqueue("k_otg", 0);
	if (!motg->otg_wq) {
		pr_err("%s: Unable to create workqueue otg_wq\n",
			__func__);
		goto destroy_wlock;
	}

	ret = request_irq(motg->irq, msm_otg_irq, IRQF_SHARED,
					"msm_otg", motg);
	if (ret) {
		dev_err(&pdev->dev, "request irq failed\n");
		goto destroy_wlock;
	}

	motg->phy_irq = platform_get_irq_byname(pdev, "phy_irq");
	if (motg->phy_irq < 0) {
		dev_dbg(&pdev->dev, "phy_irq is not present\n");
		motg->phy_irq = 0;
	} else {

		
		writeb_relaxed(0xFF, USB2_PHY_USB_PHY_INTERRUPT_CLEAR0);
		writeb_relaxed(0xFF, USB2_PHY_USB_PHY_INTERRUPT_CLEAR1);

		writeb_relaxed(0x1, USB2_PHY_USB_PHY_IRQ_CMD);
		udelay(200);
		writeb_relaxed(0x0, USB2_PHY_USB_PHY_IRQ_CMD);

		ret = request_irq(motg->phy_irq, msm_otg_phy_irq_handler,
				IRQF_TRIGGER_RISING, "msm_otg_phy_irq", motg);
		if (ret < 0) {
			dev_err(&pdev->dev, "phy_irq request fail %d\n", ret);
			goto free_irq;
		}
	}

	ret = request_irq(motg->async_irq, msm_otg_irq,
				IRQF_TRIGGER_RISING, "msm_otg", motg);
	if (ret) {
		dev_err(&pdev->dev, "request irq failed (ASYNC INT)\n");
		goto free_phy_irq;
	}
	disable_irq(motg->async_irq);

	if (pdata->otg_control == OTG_PHY_CONTROL && pdata->mpm_otgsessvld_int)
		msm_mpm_enable_pin(pdata->mpm_otgsessvld_int, 1);

	if (pdata->mpm_dpshv_int)
		msm_mpm_enable_pin(pdata->mpm_dpshv_int, 1);
	if (pdata->mpm_dmshv_int)
		msm_mpm_enable_pin(pdata->mpm_dmshv_int, 1);

	phy->init = msm_otg_reset;
	phy->set_power = msm_otg_set_power;
	phy->set_suspend = msm_otg_set_suspend;
	phy->notify_usb_disabled = msm_otg_notify_usb_disabled;
	phy->dbg_event = msm_otg_dbg_log_event;

	phy->io_ops = &msm_otg_io_ops;

	phy->otg->phy = &motg->phy;
	phy->otg->set_host = msm_otg_set_host;
	phy->otg->set_peripheral = msm_otg_set_peripheral;
	phy->otg->start_hnp = msm_otg_start_hnp;
	phy->otg->start_srp = msm_otg_start_srp;
	if (pdata->dp_manual_pullup)
		phy->flags |= ENABLE_DP_MANUAL_PULLUP;

	if (pdata->enable_sec_phy)
		phy->flags |= ENABLE_SECONDARY_PHY;

	ret = usb_add_phy(&motg->phy, USB_PHY_TYPE_USB2);
	if (ret) {
		dev_err(&pdev->dev, "usb_add_phy failed\n");
		goto free_async_irq;
	}

	if (motg->pdata->mode == USB_OTG &&
		motg->pdata->otg_control == OTG_PMIC_CONTROL &&
		!motg->phy_irq) {

		if (gpio_is_valid(motg->pdata->usb_id_gpio)) {
			
			ret = gpio_request(motg->pdata->usb_id_gpio,
							"USB_ID_GPIO");
			if (ret < 0) {
				dev_err(&pdev->dev, "gpio req failed for id\n");
				motg->pdata->usb_id_gpio = 0;
				goto remove_phy;
			}

			if (gpio_is_valid(motg->pdata->hub_reset_gpio))
				ret = devm_gpio_request(&pdev->dev,
						motg->pdata->hub_reset_gpio,
						"qcom,hub-reset-gpio");
				if (ret < 0) {
					dev_err(&pdev->dev, "gpio req failed for hub reset\n");
					goto remove_phy;
				}
				gpio_direction_output(
					motg->pdata->hub_reset_gpio, 1);

			if (gpio_is_valid(motg->pdata->switch_sel_gpio)) {
				ret = devm_gpio_request(&pdev->dev,
						motg->pdata->switch_sel_gpio,
						"qcom,sw-sel-gpio");
				if (ret < 0) {
					dev_err(&pdev->dev, "gpio req failed for switch sel\n");
					goto remove_phy;
				}
				if (gpio_get_value(motg->pdata->usb_id_gpio))
					gpio_direction_input(
						motg->pdata->switch_sel_gpio);

				else
					gpio_direction_output(
					    motg->pdata->switch_sel_gpio,
					    1);
			}

			
			id_irq = gpio_to_irq(motg->pdata->usb_id_gpio);
			motg->ext_id_irq = id_irq;
		} else if (motg->pdata->pmic_id_irq) {
			id_irq = motg->pdata->pmic_id_irq;
		}

		if (id_irq) {
			ret = request_irq(id_irq,
					  msm_id_irq,
					  IRQF_TRIGGER_RISING |
					  IRQF_TRIGGER_FALLING,
					  "msm_otg", motg);
			if (ret) {
				dev_err(&pdev->dev, "request irq failed for ID\n");
				goto remove_phy;
			}
		} else {
			dev_dbg(&pdev->dev, "PMIC does ID detection\n");
		}
	}

	msm_hsusb_mhl_switch_enable(motg, 1);

	platform_set_drvdata(pdev, motg);
	device_init_wakeup(&pdev->dev, 1);
	motg->mA_port = IUNIT;

	ret = msm_otg_debugfs_init(motg);
	if (ret)
		dev_dbg(&pdev->dev, "mode debugfs file is"
			"not available\n");

	if (motg->pdata->otg_control == OTG_PMIC_CONTROL &&
			(!(motg->pdata->mode == USB_OTG) ||
			 motg->pdata->pmic_id_irq || motg->ext_id_irq ||
								!motg->phy_irq))
		motg->caps = ALLOW_PHY_POWER_COLLAPSE | ALLOW_PHY_RETENTION;

	if (motg->pdata->otg_control == OTG_PHY_CONTROL || motg->phy_irq)
		motg->caps = ALLOW_PHY_RETENTION | ALLOW_PHY_REGULATORS_LPM;

	if (motg->pdata->mpm_dpshv_int || motg->pdata->mpm_dmshv_int)
		motg->caps |= ALLOW_HOST_PHY_RETENTION;

	device_create_file(&pdev->dev, &dev_attr_dpdm_pulldown_enable);

	if (motg->pdata->enable_lpm_on_dev_suspend)
		motg->caps |= ALLOW_LPM_ON_DEV_SUSPEND;

	if (motg->pdata->disable_retention_with_vdd_min)
		motg->caps |= ALLOW_VDD_MIN_WITH_RETENTION_DISABLED;

	if (motg->pdata->phy_type == SNPS_FEMTO_PHY)
		motg->caps |= ALLOW_BUS_SUSPEND_WITHOUT_REWORK;

	wake_lock(&motg->wlock);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	if (motg->pdata->delay_lpm_on_disconnect) {
		pm_runtime_set_autosuspend_delay(&pdev->dev,
			lpm_disconnect_thresh);
		pm_runtime_use_autosuspend(&pdev->dev);
	}

	motg->usb_psy.name = "usb";
	motg->usb_psy.type = POWER_SUPPLY_TYPE_USB;
	motg->usb_psy.supplied_to = otg_pm_power_supplied_to;
	motg->usb_psy.num_supplicants = ARRAY_SIZE(otg_pm_power_supplied_to);
	motg->usb_psy.properties = otg_pm_power_props_usb;
	motg->usb_psy.num_properties = ARRAY_SIZE(otg_pm_power_props_usb);
	motg->usb_psy.get_property = otg_power_get_property_usb;
	motg->usb_psy.set_property = otg_power_set_property_usb;
	motg->usb_psy.property_is_writeable
		= otg_power_property_is_writeable_usb;

	if (!pm8921_charger_register_vbus_sn(NULL)) {
		
		dev_dbg(motg->phy.dev, "%s: legacy support\n", __func__);
		legacy_power_supply = true;
	} else {
		
		if (!msm_otg_register_power_supply(pdev, motg))
			psy = &motg->usb_psy;
	}

	if (legacy_power_supply && pdata->otg_control == OTG_PMIC_CONTROL)
		pm8921_charger_register_vbus_sn(&msm_otg_set_vbus_state);

	usb_host_detect_register_notifier(&usb_host_status_notifier);
	ret = msm_otg_setup_ext_chg_cdev(motg);
	if (ret)
		dev_dbg(&pdev->dev, "fail to setup cdev\n");

	if (pdev->dev.of_node) {
		ret = msm_otg_setup_devices(pdev, pdata->mode, true);
		if (ret) {
			dev_err(&pdev->dev, "devices setup failed\n");
			goto remove_cdev;
		}
	}

	init_waitqueue_head(&motg->host_suspend_wait);
	motg->pm_notify.notifier_call = msm_otg_pm_notify;
	register_pm_notifier(&motg->pm_notify);
	msm_otg_dbg_log_event(phy, "OTG PROBE", motg->caps, motg->lpm_flags);

	return 0;

remove_cdev:
	if (!motg->ext_chg_device) {
		device_destroy(motg->ext_chg_class, motg->ext_chg_dev);
		cdev_del(&motg->ext_chg_cdev);
		class_destroy(motg->ext_chg_class);
		unregister_chrdev_region(motg->ext_chg_dev, 1);
	}
	if (psy)
		power_supply_unregister(psy);
remove_phy:
	usb_remove_phy(&motg->phy);
free_async_irq:
	free_irq(motg->async_irq, motg);
free_phy_irq:
	if (motg->phy_irq)
		free_irq(motg->phy_irq, motg);
free_irq:
	free_irq(motg->irq, motg);
destroy_wlock:
	wake_lock_destroy(&motg->wlock);
	wake_lock_destroy(&motg->cable_detect_wlock);
	clk_disable_unprepare(motg->core_clk);
	msm_hsusb_ldo_enable(motg, USB_PHY_REG_OFF);
	destroy_workqueue(motg->otg_wq);
free_ldo_init:
	msm_hsusb_ldo_init(motg, 0);
free_hsusb_vdd:
	regulator_disable(hsusb_vdd);
free_config_vddcx:
	regulator_set_voltage(hsusb_vdd,
		vdd_val[VDD_NONE],
		vdd_val[VDD_MAX]);
devote_xo_handle:
	clk_disable_unprepare(motg->pclk);
	if (motg->xo_clk)
		clk_disable_unprepare(motg->xo_clk);
free_xo_handle:
	if (motg->xo_clk) {
		clk_put(motg->xo_clk);
		motg->xo_clk = NULL;
	}
free_regs:
	iounmap(motg->regs);
devote_bus_bw:
	if (motg->bus_perf_client) {
		msm_otg_bus_vote(motg, USB_NO_PERF_VOTE);
		msm_bus_scale_unregister_client(motg->bus_perf_client);
	}
otg_remove_devices:
	if (pdev->dev.of_node)
		msm_otg_setup_devices(pdev, motg->pdata->mode, false);
disable_phy_csr_clk:
	if (motg->phy_csr_clk)
		clk_disable_unprepare(motg->phy_csr_clk);
disable_sleep_clk:
	if (motg->sleep_clk)
		clk_disable_unprepare(motg->sleep_clk);
put_xo_clk:
	if (motg->xo_clk)
		clk_put(motg->xo_clk);
put_pclk:
	if (motg->pclk)
		clk_put(motg->pclk);
put_core_clk:
	if (motg->core_clk)
		clk_put(motg->core_clk);
free_motg:
	kfree(motg);
	return ret;
}

static int msm_otg_remove(struct platform_device *pdev)
{
	struct msm_otg *motg = platform_get_drvdata(pdev);
	struct usb_phy *phy = &motg->phy;
	int cnt = 0;

	if (phy->otg->host || phy->otg->gadget)
		return -EBUSY;

	unregister_pm_notifier(&motg->pm_notify);

	if (!motg->ext_chg_device) {
		device_destroy(motg->ext_chg_class, motg->ext_chg_dev);
		cdev_del(&motg->ext_chg_cdev);
		class_destroy(motg->ext_chg_class);
		unregister_chrdev_region(motg->ext_chg_dev, 1);
	}

	if (pdev->dev.of_node)
		msm_otg_setup_devices(pdev, motg->pdata->mode, false);
	if (motg->pdata->otg_control == OTG_PMIC_CONTROL)
		pm8921_charger_unregister_vbus_sn(0);
	if (psy)
		power_supply_unregister(psy);
	msm_otg_mhl_register_callback(motg, NULL);
	msm_otg_debugfs_cleanup();
	cancel_delayed_work_sync(&motg->chg_work);
	cancel_delayed_work_sync(&motg->id_status_work);
	cancel_delayed_work_sync(&motg->suspend_work);
	cancel_work_sync(&motg->sm_work);
	destroy_workqueue(motg->otg_wq);

	pm_runtime_resume(&pdev->dev);

	device_init_wakeup(&pdev->dev, 0);
	pm_runtime_disable(&pdev->dev);
	wake_lock_destroy(&motg->wlock);
	wake_lock_destroy(&motg->cable_detect_wlock);

	msm_hsusb_mhl_switch_enable(motg, 0);
	if (motg->phy_irq)
		free_irq(motg->phy_irq, motg);
	if (motg->pdata->pmic_id_irq)
		free_irq(motg->pdata->pmic_id_irq, motg);
	usb_remove_phy(phy);
	free_irq(motg->irq, motg);

	if (motg->pdata->mpm_dpshv_int || motg->pdata->mpm_dmshv_int)
		device_remove_file(&pdev->dev,
				&dev_attr_dpdm_pulldown_enable);
	if (motg->pdata->otg_control == OTG_PHY_CONTROL &&
		motg->pdata->mpm_otgsessvld_int)
		msm_mpm_enable_pin(motg->pdata->mpm_otgsessvld_int, 0);

	if (motg->pdata->mpm_dpshv_int)
		msm_mpm_enable_pin(motg->pdata->mpm_dpshv_int, 0);
	if (motg->pdata->mpm_dmshv_int)
		msm_mpm_enable_pin(motg->pdata->mpm_dmshv_int, 0);

	ulpi_read(phy, 0x14);
	ulpi_write(phy, 0x08, 0x09);

	writel(readl(USB_PORTSC) | PORTSC_PHCD, USB_PORTSC);
	while (cnt < PHY_SUSPEND_TIMEOUT_USEC) {
		if (readl(USB_PORTSC) & PORTSC_PHCD)
			break;
		udelay(1);
		cnt++;
	}
	if (cnt >= PHY_SUSPEND_TIMEOUT_USEC)
		dev_err(phy->dev, "Unable to suspend PHY\n");

	clk_disable_unprepare(motg->pclk);
	clk_disable_unprepare(motg->core_clk);
	if (motg->phy_csr_clk)
		clk_disable_unprepare(motg->phy_csr_clk);
	if (motg->xo_clk) {
		clk_disable_unprepare(motg->xo_clk);
		clk_put(motg->xo_clk);
	}

	if (!IS_ERR(motg->sleep_clk))
		clk_disable_unprepare(motg->sleep_clk);

	msm_hsusb_ldo_enable(motg, USB_PHY_REG_OFF);
	msm_hsusb_ldo_init(motg, 0);
	regulator_disable(hsusb_vdd);
	regulator_set_voltage(hsusb_vdd,
		vdd_val[VDD_NONE],
		vdd_val[VDD_MAX]);

	iounmap(motg->regs);
	pm_runtime_set_suspended(&pdev->dev);

	clk_put(motg->pclk);
	clk_put(motg->core_clk);

	if (motg->bus_perf_client) {
		msm_otg_bus_vote(motg, USB_NO_PERF_VOTE);
		msm_bus_scale_unregister_client(motg->bus_perf_client);
	}

	return 0;
}

static void msm_otg_shutdown(struct platform_device *pdev)
{
	struct msm_otg *motg = platform_get_drvdata(pdev);

	dev_dbg(&pdev->dev, "OTG shutdown\n");
	msm_hsusb_vbus_power(motg, 0);
}

#ifdef CONFIG_PM_RUNTIME
static int msm_otg_runtime_idle(struct device *dev)
{
	struct msm_otg *motg = dev_get_drvdata(dev);
	struct usb_phy *phy = &motg->phy;

	dev_dbg(dev, "OTG runtime idle\n");
	msm_otg_dbg_log_event(phy, "RUNTIME IDLE",
			phy->state, motg->ext_chg_active);

	if (phy->state == OTG_STATE_UNDEFINED)
		return -EAGAIN;

	if (motg->ext_chg_active == DEFAULT) {
		dev_dbg(dev, "Deferring LPM\n");
		pm_schedule_suspend(dev, 3000);
		return -EAGAIN;
	}

	return 0;
}

static int msm_otg_runtime_suspend(struct device *dev)
{
	struct msm_otg *motg = dev_get_drvdata(dev);

	dev_dbg(dev, "OTG runtime suspend\n");
	msm_otg_dbg_log_event(&motg->phy, "RUNTIME SUSPEND",
			get_pm_runtime_counter(dev), 0);
	return msm_otg_suspend(motg);
}

static int msm_otg_runtime_resume(struct device *dev)
{
	struct msm_otg *motg = dev_get_drvdata(dev);

	dev_dbg(dev, "OTG runtime resume\n");
	msm_otg_dbg_log_event(&motg->phy, "RUNTIME RESUME",
			get_pm_runtime_counter(dev), motg->pm_done);
	pm_runtime_get_noresume(dev);
	motg->pm_done = 0;
	msm_otg_dbg_log_event(&motg->phy, "RUNTIME RESUME DONE",
			get_pm_runtime_counter(dev), motg->pm_done);
	return msm_otg_resume(motg);
}
#endif

#ifdef CONFIG_PM_SLEEP
static int msm_otg_pm_suspend(struct device *dev)
{
	int ret = 0;
	struct msm_otg *motg = dev_get_drvdata(dev);

	dev_dbg(dev, "OTG PM suspend\n");
	msm_otg_dbg_log_event(&motg->phy, "PM SUSPEND START",
			get_pm_runtime_counter(dev),
			atomic_read(&motg->pm_suspended));

	atomic_set(&motg->pm_suspended, 1);
	ret = msm_otg_suspend(motg);
	if (ret)
		atomic_set(&motg->pm_suspended, 0);

	return ret;
}

static int msm_otg_pm_resume(struct device *dev)
{
	int ret = 0;
	struct msm_otg *motg = dev_get_drvdata(dev);

	dev_dbg(dev, "OTG PM resume\n");
	msm_otg_dbg_log_event(&motg->phy, "PM RESUME START",
			get_pm_runtime_counter(dev), motg->pm_done);

	motg->pm_done = 0;

	if (motg->async_int || motg->sm_work_pending ||
			motg->phy_irq_pending ||
			!pm_runtime_suspended(dev)) {
		msm_otg_dbg_log_event(&motg->phy, "PM RESUME BY USB",
				motg->async_int, motg->phy_irq_pending);
		pm_runtime_get_noresume(dev);
		ret = msm_otg_resume(motg);

		
		pm_runtime_disable(dev);
		pm_runtime_set_active(dev);
		pm_runtime_enable(dev);

		
	}
	msm_otg_dbg_log_event(&motg->phy, "PM RESUME DONE",
			get_pm_runtime_counter(dev), motg->pm_done);

	return ret;
}
#endif

#ifdef CONFIG_PM
static const struct dev_pm_ops msm_otg_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(msm_otg_pm_suspend, msm_otg_pm_resume)
	SET_RUNTIME_PM_OPS(msm_otg_runtime_suspend, msm_otg_runtime_resume,
				msm_otg_runtime_idle)
};
#endif

static struct of_device_id msm_otg_dt_match[] = {
	{	.compatible = "qcom,hsusb-otg",
	},
	{}
};

static struct platform_driver msm_otg_driver = {
	.probe = msm_otg_probe,
	.remove = msm_otg_remove,
	.shutdown = msm_otg_shutdown,
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &msm_otg_dev_pm_ops,
#endif
		.of_match_table = msm_otg_dt_match,
	},
};

module_platform_driver(msm_otg_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MSM USB transceiver driver");
