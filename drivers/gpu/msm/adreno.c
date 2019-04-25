/* Copyright (c) 2002,2007-2015, The Linux Foundation. All rights reserved.
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
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/of_coresight.h>
#include <linux/input.h>

#include <linux/msm-bus-board.h>
#include <linux/msm-bus.h>

#include "kgsl.h"
#include "kgsl_pwrscale.h"
#include "kgsl_cffdump.h"
#include "kgsl_sharedmem.h"
#include "kgsl_iommu.h"
#include "kgsl_trace.h"

#include "adreno.h"
#include "adreno_compat.h"
#include "adreno_pm4types.h"
#include "adreno_trace.h"

#include "a3xx_reg.h"
#include "adreno_snapshot.h"

#include "adreno-gpulist.h"

#define DRIVER_VERSION_MAJOR   3
#define DRIVER_VERSION_MINOR   1

#define NUM_TIMES_RESET_RETRY 5

#define KGSL_LOG_LEVEL_DEFAULT 3

#define QFPROM_CORR_PTE2_OFFSET 0xC

#define SECVID_PROGRAM_PATH_UNKNOWN	0x0
#define SECVID_PROGRAM_PATH_GPU		0x1
#define SECVID_PROGRAM_PATH_CPU		0x2

static void adreno_input_work(struct work_struct *work);

static struct devfreq_msm_adreno_tz_data adreno_tz_data = {
	.bus = {
		.max = 350,
	},
	.device_id = KGSL_DEVICE_3D0,
};

static const struct kgsl_functable adreno_functable;

static struct adreno_device device_3d0 = {
	.dev = {
		KGSL_DEVICE_COMMON_INIT(device_3d0.dev),
		.pwrscale = KGSL_PWRSCALE_INIT(&adreno_tz_data),
		.name = DEVICE_3D0_NAME,
		.id = KGSL_DEVICE_3D0,
		.pwrctrl = {
			.irq_name = KGSL_3D0_IRQ,
		},
		.iomemname = KGSL_3D0_REG_MEMORY,
		.shadermemname = KGSL_3D0_SHADER_MEMORY,
		.ftbl = &adreno_functable,
		.cmd_log = KGSL_LOG_LEVEL_DEFAULT,
		.ctxt_log = KGSL_LOG_LEVEL_DEFAULT,
		.drv_log = KGSL_LOG_LEVEL_DEFAULT,
		.mem_log = KGSL_LOG_LEVEL_DEFAULT,
		.pwr_log = KGSL_LOG_LEVEL_DEFAULT,
	},
	.gmem_size = SZ_256K,
	.pfp_fw = NULL,
	.pm4_fw = NULL,
	.ft_policy = KGSL_FT_DEFAULT_POLICY,
	.ft_pf_policy = KGSL_FT_PAGEFAULT_DEFAULT_POLICY,
	.fast_hang_detect = 1,
	.long_ib_detect = 1,
	.input_work = __WORK_INITIALIZER(device_3d0.input_work,
		adreno_input_work),
	.pwrctrl_flag = BIT(ADRENO_SPTP_PC_CTRL) | BIT(ADRENO_PPD_CTRL),
	.profile.enabled = false,
};

unsigned int *adreno_ft_regs;
unsigned int adreno_ft_regs_num;
unsigned int *adreno_ft_regs_val;
static unsigned int adreno_ft_regs_default[] = {
	ADRENO_REG_RBBM_STATUS,
	ADRENO_REG_CP_RB_RPTR,
	ADRENO_REG_CP_IB1_BASE,
	ADRENO_REG_CP_IB1_BUFSZ,
	ADRENO_REG_CP_IB2_BASE,
	ADRENO_REG_CP_IB2_BUFSZ
};

int adreno_wake_nice = -7;

unsigned int adreno_wake_timeout = 100;

void adreno_readreg64(struct adreno_device *adreno_dev,
		enum adreno_regs lo, enum adreno_regs hi, uint64_t *val)
{
	struct adreno_gpudev *gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	unsigned int val_lo = 0, val_hi = 0;
	struct kgsl_device *device = &adreno_dev->dev;

	if (adreno_checkreg_off(adreno_dev, lo))
		kgsl_regread(device, gpudev->reg_offsets->offsets[lo], &val_lo);
	if (adreno_checkreg_off(adreno_dev, hi))
		kgsl_regread(device, gpudev->reg_offsets->offsets[hi], &val_hi);

	*val = (val_lo | ((uint64_t)val_hi << 32));
}

/**
 * adreno_writereg64() - Write a 64bit register by getting its offset from the
 * offset array defined in gpudev node
 * @adreno_dev:	Pointer to the the adreno device
 * @lo:	lower 32bit register enum that is to be written
 * @hi:	higher 32bit register enum that is to be written
 * @val: 64 bit value to write
 */
void adreno_writereg64(struct adreno_device *adreno_dev,
		enum adreno_regs lo, enum adreno_regs hi, uint64_t val)
{
	struct adreno_gpudev *gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	struct kgsl_device *device = &adreno_dev->dev;

	if (adreno_checkreg_off(adreno_dev, lo))
		kgsl_regwrite(device, gpudev->reg_offsets->offsets[lo],
			((unsigned int)val));
	if (adreno_checkreg_off(adreno_dev, hi))
		kgsl_regwrite(device, gpudev->reg_offsets->offsets[hi],
			((uint64_t)(val) >> 32));
}

static inline int adreno_of_read_property(struct device_node *node,
	const char *prop, unsigned int *ptr)
{
	int ret = of_property_read_u32(node, prop, ptr);
	if (ret)
		KGSL_CORE_ERR("Unable to read '%s'\n", prop);
	return ret;
}

static struct kgsl_device_iommu_data iommu_pdev_data;

static int adreno_iommu_cb_probe(struct platform_device *pdev)
{
	static int ctx;
	int ret = 0;

	if (ctx >=  iommu_pdev_data.iommu_ctx_count)
		return -ENOMEM;

	iommu_pdev_data.iommu_ctxs[ctx].dev = &pdev->dev;
	ret = of_property_read_string(pdev->dev.of_node, "label",
			&iommu_pdev_data.iommu_ctxs[ctx].iommu_ctx_name);

	if (!strcmp("gfx3d_user",
		iommu_pdev_data.iommu_ctxs[ctx].iommu_ctx_name)) {
			iommu_pdev_data.iommu_ctxs[ctx].ctx_id =
				KGSL_IOMMU_CONTEXT_USER;
	} else if (!strcmp("gfx3d_secure",
		iommu_pdev_data.iommu_ctxs[ctx].iommu_ctx_name)) {
			iommu_pdev_data.iommu_ctxs[ctx].ctx_id =
				KGSL_IOMMU_CONTEXT_SECURE;
	} else {
		KGSL_CORE_ERR("dt: IOMMU context %s is invalid\n",
			iommu_pdev_data.iommu_ctxs[ctx].iommu_ctx_name);
		return -EINVAL;
	}

	ctx++;
	return 0;
}

static struct of_device_id iommu_match_table[] = {
	{ .compatible = "qcom,kgsl-smmu-v2", },
	{ .compatible = "qcom,smmu-kgsl-cb", },
	{}
};

static int kgsl_iommu_pdev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const char *cname;
	struct property *prop;
	struct kgsl_device_iommu_data *data = &iommu_pdev_data;
	struct kgsl_iommu_ctx *ctxs = NULL;
	u32 reg_val[2];
	int result = -EINVAL, i = 0;

	if (of_device_is_compatible(dev->of_node, "qcom,smmu-kgsl-cb"))
		return adreno_iommu_cb_probe(pdev);

	if (of_property_read_u32_array(pdev->dev.of_node, "reg", reg_val, 2)) {
		KGSL_CORE_ERR("dt: Unable to read KGSL IOMMU register range\n");
		goto err;
	}

	data->regstart = reg_val[0];
	data->regsize = reg_val[1];

	data->features |= KGSL_MMU_DMA_API;

	result = adreno_of_read_property(pdev->dev.of_node, "num_cb",
					&data->iommu_ctx_count);
	if (result)
		goto err;

	if (!data->iommu_ctx_count) {
		KGSL_CORE_ERR(
			"dt: KGSL IOMMU context bank count cannot be zero\n");
		goto err;
	}

	ctxs = kzalloc(data->iommu_ctx_count * sizeof(struct kgsl_iommu_ctx),
					GFP_KERNEL);

	if (ctxs == NULL) {
		result = -ENOMEM;
		goto err;
	}

	data->iommu_ctxs = ctxs;

	of_property_for_each_string(dev->of_node, "clock-names", prop, cname) {
		struct clk *c = devm_clk_get(dev, cname);
		if (IS_ERR(c)) {
			KGSL_CORE_ERR("dt: Couldn't get clock: %s\n", cname);
			result = -ENODEV;
			goto err;
		}
		data->clks[i] = c;
		++i;
	}

	if (of_property_read_bool(pdev->dev.of_node, "retention"))
		data->features |= KGSL_MMU_RETENTION;

	if (of_property_read_bool(pdev->dev.of_node, "qcom,global_pt"))
		data->features |= KGSL_MMU_GLOBAL_PAGETABLE;

	if (of_property_read_bool(pdev->dev.of_node, "qcom,hyp_secure_alloc"))
		data->features |= KGSL_MMU_HYP_SECURE_ALLOC;

	result = of_platform_populate(pdev->dev.of_node, iommu_match_table,
				NULL, &pdev->dev);
	if (!result)
		return 0;

err:
	kfree(ctxs);
	for (; i >= 0 && data->clks[i]; i--)
		devm_clk_put(dev, data->clks[i]);

	return result;
}

static struct platform_driver kgsl_iommu_platform_driver = {
	.probe = kgsl_iommu_pdev_probe,
	.driver = {
		.owner = THIS_MODULE,
		.name = "kgsl-iommu",
		.of_match_table = iommu_match_table,
	}
};

static int __init kgsl_iommu_pdev_init(void)
{
	return platform_driver_register(&kgsl_iommu_platform_driver);
}

static void __exit kgsl_iommu_pdev_exit(void)
{
	platform_driver_unregister(&kgsl_iommu_platform_driver);
}

module_init(kgsl_iommu_pdev_init);
module_exit(kgsl_iommu_pdev_exit);

static int _get_counter(struct adreno_device *adreno_dev,
		int group, int countable, unsigned int *lo,
		unsigned int *hi)
{
	int ret = 0;

	if (*lo == 0) {

		ret = adreno_perfcounter_get(adreno_dev, group, countable,
			lo, hi, PERFCOUNTER_FLAG_KERNEL);

		if (ret) {
			struct kgsl_device *device = &adreno_dev->dev;

			KGSL_DRV_ERR(device,
				"Unable to allocate fault detect performance counter %d/%d\n",
				group, countable);
			KGSL_DRV_ERR(device,
				"GPU fault detect will be less reliable\n");
		}
	}

	return ret;
}

static inline void _put_counter(struct adreno_device *adreno_dev,
		int group, int countable, unsigned int *lo,
		unsigned int *hi)
{
	if (*lo != 0)
		adreno_perfcounter_put(adreno_dev, group, countable,
			PERFCOUNTER_FLAG_KERNEL);

	*lo = 0;
	*hi = 0;
}

void adreno_fault_detect_start(struct adreno_device *adreno_dev)
{
	struct adreno_gpudev *gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	unsigned int i, j = ARRAY_SIZE(adreno_ft_regs_default);

	if (!test_bit(ADRENO_DEVICE_SOFT_FAULT_DETECT, &adreno_dev->priv))
		return;

	if (adreno_dev->fast_hang_detect == 1)
		return;

	for (i = 0; i < gpudev->ft_perf_counters_count; i++) {
		_get_counter(adreno_dev, gpudev->ft_perf_counters[i].counter,
			 gpudev->ft_perf_counters[i].countable,
			 &adreno_ft_regs[j + (i * 2)],
			 &adreno_ft_regs[j + ((i * 2) + 1)]);
	}

	adreno_dev->fast_hang_detect = 1;
}

void adreno_fault_detect_stop(struct adreno_device *adreno_dev)
{
	struct adreno_gpudev *gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	unsigned int i, j = ARRAY_SIZE(adreno_ft_regs_default);

	if (!test_bit(ADRENO_DEVICE_SOFT_FAULT_DETECT, &adreno_dev->priv))
		return;

	if (!adreno_dev->fast_hang_detect)
		return;

	for (i = 0; i < gpudev->ft_perf_counters_count; i++) {
		_put_counter(adreno_dev, gpudev->ft_perf_counters[i].counter,
			 gpudev->ft_perf_counters[i].countable,
			 &adreno_ft_regs[j + (i * 2)],
			 &adreno_ft_regs[j + ((i * 2) + 1)]);

	}

	adreno_dev->fast_hang_detect = 0;
}

static void adreno_input_work(struct work_struct *work)
{
	struct adreno_device *adreno_dev = container_of(work,
			struct adreno_device, input_work);
	struct kgsl_device *device = &adreno_dev->dev;

	mutex_lock(&device->mutex);

	device->flags |= KGSL_FLAG_WAKE_ON_TOUCH;

	kgsl_pwrctrl_change_state(device, KGSL_STATE_ACTIVE);

	mod_timer(&device->idle_timer,
		jiffies + msecs_to_jiffies(adreno_wake_timeout));
	mutex_unlock(&device->mutex);
}

static void adreno_input_event(struct input_handle *handle, unsigned int type,
		unsigned int code, int value)
{
	struct kgsl_device *device = handle->handler->private;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	
	if (type != EV_ABS)
		return;


	if (device->flags & KGSL_FLAG_WAKE_ON_TOUCH)
		return;


	if (device->state == KGSL_STATE_NAP) {

		device->flags |= KGSL_FLAG_WAKE_ON_TOUCH;

		mod_timer(&device->idle_timer,
			jiffies + device->pwrctrl.interval_timeout);
	} else if (device->state == KGSL_STATE_SLUMBER) {
		schedule_work(&adreno_dev->input_work);
	}
}

#ifdef CONFIG_INPUT
static int adreno_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (handle == NULL)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = handler->name;

	ret = input_register_handle(handle);
	if (ret) {
		kfree(handle);
		return ret;
	}

	ret = input_open_device(handle);
	if (ret) {
		input_unregister_handle(handle);
		kfree(handle);
	}

	return ret;
}

static void adreno_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}
#else
static int adreno_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	return 0;
}
static void adreno_input_disconnect(struct input_handle *handle) {}
#endif

static const struct input_device_id adreno_input_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
				BIT_MASK(ABS_MT_POSITION_X) |
				BIT_MASK(ABS_MT_POSITION_Y) },
	},
	{ },
};

static struct input_handler adreno_input_handler = {
	.event = adreno_input_event,
	.connect = adreno_input_connect,
	.disconnect = adreno_input_disconnect,
	.name = "kgsl",
	.id_table = adreno_input_ids,
};

static int adreno_soft_reset(struct kgsl_device *device);

void _soft_reset(struct adreno_device *adreno_dev)
{
	struct adreno_gpudev *gpudev  = ADRENO_GPU_DEVICE(adreno_dev);
	unsigned int reg;

	if (adreno_is_a530v1(adreno_dev)) {
		adreno_writereg(adreno_dev, ADRENO_REG_RBBM_BLOCK_SW_RESET_CMD,
						 0xFFDFFC0);
		adreno_writereg(adreno_dev, ADRENO_REG_RBBM_BLOCK_SW_RESET_CMD2,
						0x1FFFFFFF);
	} else {

		adreno_writereg(adreno_dev, ADRENO_REG_RBBM_SW_RESET_CMD, 1);
		adreno_readreg(adreno_dev, ADRENO_REG_RBBM_SW_RESET_CMD, &reg);
		adreno_writereg(adreno_dev, ADRENO_REG_RBBM_SW_RESET_CMD, 0);
	}

	

	if (gpudev->regulator_enable)
		gpudev->regulator_enable(adreno_dev);
}


void adreno_irqctrl(struct adreno_device *adreno_dev, int state)
{
	struct adreno_gpudev *gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	unsigned int mask = state ? gpudev->irq->mask : 0;

	adreno_writereg(adreno_dev, ADRENO_REG_RBBM_INT_0_MASK, mask);
}

void adreno_hang_int_callback(struct adreno_device *adreno_dev, int bit)
{
	struct kgsl_device *device = &adreno_dev->dev;

	KGSL_DRV_CRIT(device, "MISC: GPU hang detected\n");
	adreno_irqctrl(adreno_dev, 0);

	
	adreno_set_gpu_fault(ADRENO_DEVICE(device), ADRENO_HARD_FAULT);
	adreno_dispatcher_schedule(device);
}

void adreno_cp_callback(struct adreno_device *adreno_dev, int bit)
{
	struct kgsl_device *device = &adreno_dev->dev;

	queue_work(device->work_queue, &device->event_work);
	adreno_dispatcher_schedule(device);
}

static irqreturn_t adreno_irq_handler(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_gpudev *gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	struct adreno_irq *irq_params = gpudev->irq;
	irqreturn_t ret = IRQ_NONE;
	unsigned int status = 0, tmp = 0;
	int i;

	adreno_readreg(adreno_dev, ADRENO_REG_RBBM_INT_0_STATUS, &status);

	
	for (tmp = status, i = 0; tmp &&
		 i < irq_params->funcs_count; i++) {
		if (tmp & 1) {
			if (irq_params->funcs[i].func != NULL) {
				irq_params->funcs[i].func(adreno_dev, i);
				ret = IRQ_HANDLED;
			} else
			KGSL_DRV_CRIT(device,
					"Unhandled interrupt bit %x\n", i);
		}
		tmp >>= 1;
	}

	gpudev->irq_trace(adreno_dev, status);

	if (status)
		adreno_writereg(adreno_dev, ADRENO_REG_RBBM_INT_CLEAR_CMD,
				status);
	return ret;

}

static inline bool _rev_match(unsigned int id, unsigned int entry)
{
	return (entry == ANY_ID || entry == id);
}

static inline const struct adreno_gpu_core *_get_gpu_core(unsigned int chipid)
{
	unsigned int core = ADRENO_CHIPID_CORE(chipid);
	unsigned int major = ADRENO_CHIPID_MAJOR(chipid);
	unsigned int minor = ADRENO_CHIPID_MINOR(chipid);
	unsigned int patchid = ADRENO_CHIPID_PATCH(chipid);
	int i;

	for (i = 0; i < ARRAY_SIZE(adreno_gpulist); i++) {
		if (core == adreno_gpulist[i].core &&
		    _rev_match(major, adreno_gpulist[i].major) &&
		    _rev_match(minor, adreno_gpulist[i].minor) &&
		    _rev_match(patchid, adreno_gpulist[i].patchid))
			return &adreno_gpulist[i];
	}

	return NULL;
}

static void
adreno_identify_gpu(struct adreno_device *adreno_dev)
{
	const struct adreno_reg_offsets *reg_offsets;
	struct adreno_gpudev *gpudev;
	int i;

	if (kgsl_property_read_u32(&adreno_dev->dev, "qcom,chipid",
		&adreno_dev->chipid))
		KGSL_DRV_FATAL(&adreno_dev->dev,
			"No GPU chip ID was specified\n");

	adreno_dev->gpucore = _get_gpu_core(adreno_dev->chipid);

	if (adreno_dev->gpucore == NULL)
		KGSL_DRV_FATAL(&adreno_dev->dev, "Unknown GPU chip ID %8.8X\n",
			adreno_dev->chipid);


	adreno_dev->gmem_size = adreno_dev->gpucore->gmem_size;


	gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	reg_offsets = gpudev->reg_offsets;

	for (i = 0; i < ADRENO_REG_REGISTER_MAX; i++) {
		if (reg_offsets->offset_0 != i && !reg_offsets->offsets[i])
			reg_offsets->offsets[i] = ADRENO_REG_UNUSED;
	}
	if (gpudev->gpudev_init)
		gpudev->gpudev_init(adreno_dev);
}

static const struct platform_device_id adreno_id_table[] = {
	{ DEVICE_3D0_NAME, (unsigned long) &device_3d0, },
	{},
};

MODULE_DEVICE_TABLE(platform, adreno_id_table);

static const struct of_device_id adreno_match_table[] = {
	{ .compatible = "qcom,kgsl-3d0", .data = &device_3d0 },
	{}
};

static struct device_node *adreno_of_find_subnode(struct device_node *parent,
	const char *name)
{
	struct device_node *child;

	for_each_child_of_node(parent, child) {
		if (of_device_is_compatible(child, name))
			return child;
	}

	return NULL;
}

static int adreno_of_get_bus_data(struct platform_device *pdev,
		struct device_node *node,
		struct kgsl_device_platform_data *pdata)
{
	struct device_node *parent =  pdev->dev.of_node;
	int ret=0, num_usecases = 0, num_paths, len;
	const uint32_t *vec_arr = NULL;
	const char *name;

	if (node != parent) {
		ret = of_property_read_string(node, "qcom,msm-bus,name",
				&name);
		if (ret)
			goto use_parent;

		ret = of_property_read_u32(node, "qcom,msm-bus,num-cases",
				&num_usecases);
		if (ret)
			goto use_parent;

		ret = of_property_read_u32(node, "qcom,msm-bus,num-paths",
				&num_paths);
		if (ret)
			goto use_parent;

		vec_arr = of_get_property(node, "qcom,msm-bus,vectors-KBps",
				&len);
		if (vec_arr == NULL)
			goto use_parent;
		pdev->dev.of_node = node;
	}

	pdata->bus_scale_table = msm_bus_cl_get_pdata(pdev);

	if (node != parent)
		pdev->dev.of_node = parent;

	if (IS_ERR_OR_NULL(pdata->bus_scale_table)) {
		ret = PTR_ERR(pdata->bus_scale_table);
		if (!ret)
			ret = -EINVAL;
	}

	return ret;

use_parent:
	ret = 0;
	pdata->bus_scale_table = msm_bus_cl_get_pdata(pdev);

	if (IS_ERR_OR_NULL(pdata->bus_scale_table)) {
		ret = PTR_ERR(pdata->bus_scale_table);
		if (!ret)
			ret = -EINVAL;
	}

	return ret;
}

static int adreno_of_get_pwrlevels(struct device_node *parent,
	struct kgsl_device_platform_data *pdata)
{
	struct device_node *node, *child;
	int ret = -EINVAL;

	node = of_find_node_by_name(parent, "qcom,gpu-pwrlevels");

	if (node == NULL) {
		KGSL_CORE_ERR("Unable to find 'qcom,gpu-pwrlevels'\n");
		return -EINVAL;
	}

	pdata->num_levels = 0;

	for_each_child_of_node(node, child) {
		unsigned int index;
		struct kgsl_pwrlevel *level;

		if (adreno_of_read_property(child, "reg", &index))
			goto done;

		if (index >= KGSL_MAX_PWRLEVELS) {
			KGSL_CORE_ERR("Pwrlevel index %d is out of range\n",
				index);
			continue;
		}

		if (index >= pdata->num_levels)
			pdata->num_levels = index + 1;

		level = &pdata->pwrlevel[index];

		if (adreno_of_read_property(child, "qcom,gpu-freq",
			&level->gpu_freq))
			goto done;

		if (adreno_of_read_property(child, "qcom,bus-freq",
			&level->bus_freq))
			goto done;

		if (of_property_read_u32(child, "qcom,bus-min",
			&level->bus_min))
			level->bus_min = level->bus_freq;

		if (of_property_read_u32(child, "qcom,bus-max",
			&level->bus_max))
			level->bus_max = level->bus_freq;
	}

	if (of_property_read_u32(parent, "qcom,initial-pwrlevel",
		&pdata->init_level))
		pdata->init_level = 1;

	if (pdata->init_level < 0 || pdata->init_level > pdata->num_levels) {
		KGSL_CORE_ERR("Initial power level out of range\n");
		pdata->init_level = 1;
	}

	ret = 0;
done:
	return ret;

}

static inline struct adreno_device *adreno_get_dev(struct platform_device *pdev)
{
	const struct of_device_id *of_id =
		of_match_device(adreno_match_table, &pdev->dev);

	return of_id ? (struct adreno_device *) of_id->data : NULL;
}

static int adreno_of_get_iommu(struct platform_device *pdev,
	struct kgsl_device_platform_data *pdata)
{
	struct device_node *parent = pdev->dev.of_node;
	struct adreno_device *adreno_dev;
	int result = -EINVAL;
	struct device_node *node, *child;
	struct kgsl_device_iommu_data *data = NULL;
	struct kgsl_iommu_ctx *ctxs = NULL;
	u32 reg_val[2];
	u32 secure_id;
	int ctx_index = 0;

	node = of_parse_phandle(parent, "iommu", 0);
	if (node == NULL)
		return -EINVAL;

	adreno_dev = adreno_get_dev(pdev);
	if (adreno_dev == NULL)
		return -EINVAL;

	adreno_dev->dev.mmu.secured =
		(of_property_read_u32(node, "qcom,iommu-secure-id",
			&secure_id) == 0) ?  true : false;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (data == NULL) {
		result = -ENOMEM;
		goto err;
	}

	if (of_property_read_u32_array(node, "reg", reg_val, 2))
		goto err;

	data->regstart = reg_val[0];
	data->regsize = reg_val[1];

	if (of_property_read_bool(node, "qcom,global_pt"))
		data->features |= KGSL_MMU_GLOBAL_PAGETABLE;

	data->iommu_ctx_count = 0;

	for_each_child_of_node(node, child)
		data->iommu_ctx_count++;

	ctxs = kzalloc(data->iommu_ctx_count * sizeof(struct kgsl_iommu_ctx),
		GFP_KERNEL);

	if (ctxs == NULL) {
		result = -ENOMEM;
		goto err;
	}

	for_each_child_of_node(node, child) {
		int ret = of_property_read_string(child, "label",
				&ctxs[ctx_index].iommu_ctx_name);

		if (ret) {
			KGSL_CORE_ERR("Unable to read KGSL IOMMU 'label'\n");
			goto err;
		}

		if (!strcmp("gfx3d_user", ctxs[ctx_index].iommu_ctx_name)) {
			ctxs[ctx_index].ctx_id = 0;
		} else if (!strcmp("gfx3d_priv",
					ctxs[ctx_index].iommu_ctx_name)) {
			ctxs[ctx_index].ctx_id = 1;
		} else if (!strcmp("gfx3d_spare",
					ctxs[ctx_index].iommu_ctx_name)) {
			ctxs[ctx_index].ctx_id = 2;
		} else if (!strcmp("gfx3d_secure",
					ctxs[ctx_index].iommu_ctx_name)) {
			ctxs[ctx_index].ctx_id = 2;
		} else {
			KGSL_CORE_ERR("dt: IOMMU context %s is invalid\n",
				ctxs[ctx_index].iommu_ctx_name);
			goto err;
		}

		ctx_index++;
	}

	data->iommu_ctxs = ctxs;

	pdata->iommu_data = data;

	return 0;

err:
	kfree(ctxs);
	kfree(data);

	return result;
}

static struct device_node *get_gpu_speed_config_data(struct platform_device
		*pdev)
{
	struct resource *res;
	void __iomem *base;
	u32 pte_reg_val;
	int speed_bin, speed_config;
	char prop_name[32];

	
	if (of_property_read_u32(pdev->dev.of_node,
			"qcom,gpu-speed-config", &speed_config))
		return pdev->dev.of_node;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
			"qfprom_memory");
	if (!res)
		return NULL;

	base = ioremap(res->start, resource_size(res));

	if (!base)
		return NULL;

	pte_reg_val = __raw_readl(base + QFPROM_CORR_PTE2_OFFSET);

	iounmap(base);

	speed_bin = (pte_reg_val >> 0x2) & 0x7;
	if (speed_bin == speed_config) {
		snprintf(prop_name, ARRAY_SIZE(prop_name), "%s%d",
				"gpu-speed-config@", speed_config);
		return adreno_of_find_subnode(pdev->dev.of_node, prop_name);
	}

	return pdev->dev.of_node;
}

static int adreno_of_get_pdata(struct platform_device *pdev)
{
	struct kgsl_device_platform_data *pdata = NULL;
	struct device_node *node;
	int ret = -EINVAL;

	if (of_property_read_string(pdev->dev.of_node, "label", &pdev->name)) {
		KGSL_CORE_ERR("Unable to read 'label'\n");
		goto err;
	}

	if (adreno_of_read_property(pdev->dev.of_node, "qcom,id", &pdev->id))
		goto err;

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (pdata == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	
	node = get_gpu_speed_config_data(pdev);
	if (node == NULL)
		goto err;

	
	ret = adreno_of_get_pwrlevels(node, pdata);
	if (ret)
		goto err;

	
	if (of_property_read_u32(pdev->dev.of_node,
		"qcom,pm-qos-active-latency",
		&pdata->pm_qos_active_latency))
		pdata->pm_qos_active_latency = 501;

	
	if (of_property_read_u32(pdev->dev.of_node,
		"qcom,pm-qos-wakeup-latency",
		&pdata->pm_qos_wakeup_latency))
		pdata->pm_qos_wakeup_latency = 101;

	if (of_property_read_u32(pdev->dev.of_node, "qcom,idle-timeout",
		&pdata->idle_timeout))
		pdata->idle_timeout = HZ/12;

	pdata->strtstp_sleepwake = of_property_read_bool(pdev->dev.of_node,
						"qcom,strtstp-sleepwake");

	pdata->bus_control = of_property_read_bool(pdev->dev.of_node,
						"qcom,bus-control");

	pdata->popp_enable = of_property_read_bool(pdev->dev.of_node,
						"qcom,popp-enable");

	if (adreno_of_read_property(pdev->dev.of_node, "qcom,clk-map",
		&pdata->clk_map))
		goto err;

	
	ret = adreno_of_get_bus_data(pdev, node, pdata);
	if (ret)
		goto err;

	
	if (of_parse_phandle(pdev->dev.of_node, "iommu", 0)) {
		ret = adreno_of_get_iommu(pdev, pdata);
		if (ret)
			goto err;
	} else
		pdata->iommu_data = &iommu_pdev_data;


	pdata->coresight_pdata = of_get_coresight_platform_data(&pdev->dev,
			pdev->dev.of_node);

	pdev->dev.platform_data = pdata;
	return 0;

err:
	if (pdata) {
		if (pdata->iommu_data)
			kfree(pdata->iommu_data->iommu_ctxs);

		kfree(pdata->iommu_data);
	}

	kfree(pdata);

	return ret;
}

#ifdef CONFIG_MSM_OCMEM
static int
adreno_ocmem_malloc(struct adreno_device *adreno_dev)
{
	if (!ADRENO_FEATURE(adreno_dev, ADRENO_USES_OCMEM))
		return 0;

	if (adreno_dev->ocmem_hdl == NULL) {
		adreno_dev->ocmem_hdl =
			ocmem_allocate(OCMEM_GRAPHICS, adreno_dev->gmem_size);
		if (IS_ERR_OR_NULL(adreno_dev->ocmem_hdl)) {
			adreno_dev->ocmem_hdl = NULL;
			return -ENOMEM;
		}

		adreno_dev->gmem_size = adreno_dev->ocmem_hdl->len;
		adreno_dev->gmem_base = adreno_dev->ocmem_hdl->addr;
	}

	return 0;
}

static void
adreno_ocmem_free(struct adreno_device *adreno_dev)
{
	if (adreno_dev->ocmem_hdl != NULL) {
		ocmem_free(OCMEM_GRAPHICS, adreno_dev->ocmem_hdl);
		adreno_dev->ocmem_hdl = NULL;
	}
}
#else
static int
adreno_ocmem_malloc(struct adreno_device *adreno_dev)
{
	return 0;
}

static void
adreno_ocmem_free(struct adreno_device *adreno_dev)
{
}
#endif

int adreno_probe(struct platform_device *pdev)
{
	struct kgsl_device *device;
	struct adreno_device *adreno_dev;
	int status;

	
	if (!of_parse_phandle(pdev->dev.of_node, "iommu", 0) &&
			(iommu_pdev_data.regstart == 0))
		return -EPROBE_DEFER;

	adreno_dev = adreno_get_dev(pdev);

	if (adreno_dev == NULL) {
		pr_err("adreno: qcom,kgsl-3d0 does not exist in the device tree");
		return -ENODEV;
	}

	device = &adreno_dev->dev;
	device->pdev = pdev;

	status = adreno_of_get_pdata(pdev);
	if (status) {
		device->pdev = NULL;
		return status;
	}

	
	adreno_identify_gpu(adreno_dev);


	if (!ADRENO_FEATURE(adreno_dev, ADRENO_CONTENT_PROTECTION))
		device->mmu.secured = false;

	status = kgsl_device_platform_probe(device);
	if (status) {
		device->pdev = NULL;
		return status;
	}

	status = adreno_ringbuffer_init(device);
	if (status)
		goto out;

	status = adreno_dispatcher_init(adreno_dev);
	if (status)
		goto out;

	adreno_debugfs_init(adreno_dev);
	adreno_profile_init(adreno_dev);

	adreno_sysfs_init(device);

	kgsl_pwrscale_init(&pdev->dev, CONFIG_MSM_ADRENO_DEFAULT_GOVERNOR);

	adreno_input_handler.private = device;

#ifdef CONFIG_INPUT
	if (input_register_handler(&adreno_input_handler))
		KGSL_DRV_ERR(device, "Unable to register the input handler\n");
#endif
out:
	if (status) {
		adreno_ringbuffer_close(adreno_dev);
		kgsl_device_platform_remove(device);
		device->pdev = NULL;
	}

	return status;
}

static int adreno_remove(struct platform_device *pdev)
{
	struct adreno_device *adreno_dev = adreno_get_dev(pdev);
	struct kgsl_device *device;

	if (adreno_dev == NULL)
		return 0;

	device = &adreno_dev->dev;

	if (test_bit(ADRENO_DEVICE_CMDBATCH_PROFILE, &adreno_dev->priv))
		kgsl_free_global(&adreno_dev->cmdbatch_profile_buffer);

#ifdef CONFIG_INPUT
	input_unregister_handler(&adreno_input_handler);
#endif
	adreno_sysfs_close(device);

	adreno_coresight_remove(adreno_dev);
	adreno_profile_close(adreno_dev);

	kgsl_pwrscale_close(device);

	adreno_dispatcher_close(adreno_dev);
	adreno_ringbuffer_close(adreno_dev);

	adreno_fault_detect_stop(adreno_dev);

	kfree(adreno_ft_regs);
	adreno_ft_regs = NULL;

	kfree(adreno_ft_regs_val);
	adreno_ft_regs_val = NULL;

	adreno_perfcounter_close(adreno_dev);
	kgsl_device_platform_remove(device);

	if (test_bit(ADRENO_DEVICE_PWRON_FIXUP, &adreno_dev->priv)) {
		kgsl_free_global(&adreno_dev->pwron_fixup);
		clear_bit(ADRENO_DEVICE_PWRON_FIXUP, &adreno_dev->priv);
	}
	clear_bit(ADRENO_DEVICE_INITIALIZED, &adreno_dev->priv);

	return 0;
}

static void adreno_fault_detect_init(struct adreno_device *adreno_dev)
{
	struct adreno_gpudev *gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	int i, val = adreno_dev->fast_hang_detect;

	
	adreno_dev->fast_hang_detect = 0;

	adreno_ft_regs_num = (ARRAY_SIZE(adreno_ft_regs_default) +
		gpudev->ft_perf_counters_count*2);

	adreno_ft_regs = kzalloc(adreno_ft_regs_num * sizeof(unsigned int),
		GFP_KERNEL);
	adreno_ft_regs_val = kzalloc(adreno_ft_regs_num * sizeof(unsigned int),
		GFP_KERNEL);

	if (adreno_ft_regs == NULL || adreno_ft_regs_val == NULL) {
		kfree(adreno_ft_regs);
		kfree(adreno_ft_regs_val);

		adreno_ft_regs = NULL;
		adreno_ft_regs_val = NULL;

		return;
	}

	for (i = 0; i < ARRAY_SIZE(adreno_ft_regs_default); i++)
		adreno_ft_regs[i] = adreno_getreg(adreno_dev,
			adreno_ft_regs_default[i]);

	set_bit(ADRENO_DEVICE_SOFT_FAULT_DETECT, &adreno_dev->priv);

	if (val)
		adreno_fault_detect_start(adreno_dev);
}

static int adreno_init(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_gpudev *gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	int ret;

	kgsl_pwrctrl_change_state(device, KGSL_STATE_INIT);
	if (test_bit(ADRENO_DEVICE_INITIALIZED, &adreno_dev->priv))
		return 0;


	ret = gpudev->microcode_read(adreno_dev);
	if (ret)
		return ret;

	
	ret = kgsl_pwrctrl_change_state(device, KGSL_STATE_AWARE);
	if (ret)
		return ret;

	ret = adreno_iommu_init(adreno_dev);
	if (ret)
		return ret;

	
	adreno_coresight_init(adreno_dev);

	adreno_perfcounter_init(adreno_dev);
	adreno_fault_detect_init(adreno_dev);

	
	kgsl_pwrctrl_change_state(device, KGSL_STATE_INIT);

	if (adreno_is_a3xx(adreno_dev))
		adreno_a3xx_pwron_fixup_init(adreno_dev);
	else if ((adreno_is_a405(adreno_dev)) || (adreno_is_a420(adreno_dev)))
		adreno_a4xx_pwron_fixup_init(adreno_dev);

	set_bit(ADRENO_DEVICE_INITIALIZED, &adreno_dev->priv);

	
	if (adreno_dev->gpucore->shader_offset &&
					adreno_dev->gpucore->shader_size) {

		if (device->shader_mem_phys || device->shader_mem_virt)
			KGSL_DRV_ERR(device,
			"Shader memory already specified in device tree\n");
		else {
			device->shader_mem_phys = device->reg_phys +
					adreno_dev->gpucore->shader_offset;
			device->shader_mem_virt = device->reg_virt +
					adreno_dev->gpucore->shader_offset;
			device->shader_mem_len =
					adreno_dev->gpucore->shader_size;
		}
	}

	
	if ((adreno_is_a330(adreno_dev) || adreno_is_a305b(adreno_dev))) {
		gpudev->snapshot_data->sect_sizes->cp_pfp =
					A320_SNAPSHOT_CP_STATE_SECTION_SIZE;
		gpudev->snapshot_data->sect_sizes->roq =
					A320_SNAPSHOT_ROQ_SECTION_SIZE;
		gpudev->snapshot_data->sect_sizes->cp_merciu =
					A320_SNAPSHOT_CP_MERCIU_SECTION_SIZE;
	}


	if (!adreno_is_a3xx(adreno_dev)) {
		int r = kgsl_allocate_global(&adreno_dev->dev,
			&adreno_dev->cmdbatch_profile_buffer, PAGE_SIZE, 0, 0);

		adreno_dev->cmdbatch_profile_index = 0;

		if (r == 0)
			set_bit(ADRENO_DEVICE_CMDBATCH_PROFILE,
				&adreno_dev->priv);
	}

	if (ADRENO_FEATURE(adreno_dev, ADRENO_PREEMPTION))
		set_bit(ADRENO_DEVICE_PREEMPTION, &adreno_dev->priv);

	
	if (kgsl_mmu_get_mmutype() == KGSL_MMU_TYPE_NONE)
		adreno_preemption_disable(adreno_dev);

	
	if (gpudev->preemption_init && adreno_is_preemption_enabled(adreno_dev))
		gpudev->preemption_init(adreno_dev);

	return 0;
}

static void secvid_cpu_path(struct adreno_device *adreno_dev)
{
	if (adreno_is_a4xx(adreno_dev))
		adreno_writereg(adreno_dev,
			ADRENO_REG_RBBM_SECVID_TRUST_CONFIG, 0x2);
	adreno_writereg(adreno_dev, ADRENO_REG_RBBM_SECVID_TSB_CONTROL, 0x0);
	adreno_writereg(adreno_dev, ADRENO_REG_RBBM_SECVID_TSB_TRUSTED_BASE,
		KGSL_IOMMU_SECURE_MEM_BASE);
	adreno_writereg(adreno_dev, ADRENO_REG_RBBM_SECVID_TSB_TRUSTED_SIZE,
		KGSL_IOMMU_SECURE_MEM_SIZE);
}


static void secvid_gpu_path(struct adreno_device *adreno_dev)
{
	unsigned int *cmds;
	struct adreno_ringbuffer *rb = ADRENO_CURRENT_RINGBUFFER(adreno_dev);
	int ret = 0;

	cmds = adreno_ringbuffer_allocspace(rb, 13);

	*cmds++ = cp_packet(adreno_dev, CP_SET_PROTECTED_MODE, 1);
	*cmds++ = 0;

	*cmds++ = cp_packet(adreno_dev, CP_WIDE_REG_WRITE, 2);
	*cmds++ = adreno_getreg(adreno_dev, ADRENO_REG_RBBM_SECVID_TSB_CONTROL);
	*cmds++ = 0x0;

	*cmds++ = cp_packet(adreno_dev, CP_WIDE_REG_WRITE, 2);
	*cmds++ = adreno_getreg(adreno_dev,
			ADRENO_REG_RBBM_SECVID_TSB_TRUSTED_BASE);
	*cmds++ = KGSL_IOMMU_SECURE_MEM_BASE;

	*cmds++ = cp_packet(adreno_dev, CP_WIDE_REG_WRITE, 2);
	*cmds++ = adreno_getreg(adreno_dev,
			ADRENO_REG_RBBM_SECVID_TSB_TRUSTED_SIZE);
	*cmds++ = KGSL_IOMMU_SECURE_MEM_SIZE;

	*cmds++ = cp_packet(adreno_dev, CP_SET_PROTECTED_MODE, 1);
	*cmds++ = 1;

	adreno_ringbuffer_submit(rb, NULL);

	
	ret = adreno_spin_idle(&adreno_dev->dev);
	if (ret) {
		KGSL_DRV_ERR(rb->device, "secure init failed to idle %d\n",
			ret);
		secvid_cpu_path(adreno_dev);
	}
}

static int secvid_verify_gpu_path(struct adreno_device *adreno_dev)
{
	unsigned int *cmds, val = 0;
	struct adreno_ringbuffer *rb = ADRENO_CURRENT_RINGBUFFER(adreno_dev);
	int ret = 0;

	cmds = adreno_ringbuffer_allocspace(rb, 19);

	*cmds++ = cp_packet(adreno_dev, CP_SET_PROTECTED_MODE, 1);
	*cmds++ = 0;

	
	*cmds++ = cp_packet(adreno_dev, CP_WIDE_REG_WRITE, 2);
	*cmds++ = adreno_getreg(adreno_dev, ADRENO_REG_CP_SCRATCH_REG0);
	*cmds++ = 0x0;

	
	*cmds++ = cp_packet(adreno_dev, CP_WIDE_REG_WRITE, 2);
	*cmds++ = adreno_getreg(adreno_dev,
			ADRENO_REG_RBBM_SECVID_TSB_TRUSTED_BASE);
	*cmds++ = KGSL_IOMMU_SECURE_MEM_BASE;

	
	cmds += cp_wait_for_idle(adreno_dev, cmds);

	*cmds++ = cp_mem_packet(adreno_dev, CP_COND_WRITE, 6, 2);
	*cmds++ = 0x3;
	*cmds++ =  adreno_getreg(adreno_dev,
				ADRENO_REG_RBBM_SECVID_TSB_TRUSTED_BASE);
	*cmds++ = KGSL_IOMMU_SECURE_MEM_BASE;
	*cmds++ = 0xFFFFFFFF;
	*cmds++ = adreno_getreg(adreno_dev, ADRENO_REG_CP_SCRATCH_REG0);
	*cmds++ = 0x1;

	*cmds++ = cp_packet(adreno_dev, CP_SET_PROTECTED_MODE, 1);
	*cmds++ = 1;

	adreno_ringbuffer_submit(rb, NULL);

	ret = adreno_spin_idle(&adreno_dev->dev);
	if (ret) {
		KGSL_DRV_ERR(rb->device,
		"secure init verification failed to idle %d\n", ret);
		secvid_cpu_path(adreno_dev);
		return SECVID_PROGRAM_PATH_CPU;
	}

	adreno_readreg(adreno_dev, ADRENO_REG_CP_SCRATCH_REG0, &val);

	if (val == 0x1) {
		secvid_gpu_path(adreno_dev);
		return SECVID_PROGRAM_PATH_GPU;
	}

	secvid_cpu_path(adreno_dev);

	return SECVID_PROGRAM_PATH_CPU;
}

static void adreno_secvid_start(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	static int secvid_path = SECVID_PROGRAM_PATH_UNKNOWN;

	if (!adreno_is_a4xx(adreno_dev) ||
		secvid_path == SECVID_PROGRAM_PATH_CPU) {
		secvid_cpu_path(adreno_dev);
	} else if (secvid_path == SECVID_PROGRAM_PATH_GPU)
		secvid_gpu_path(adreno_dev);
	else
		secvid_path = secvid_verify_gpu_path(adreno_dev);
}

static int _adreno_start(struct adreno_device *adreno_dev)
{
	struct kgsl_device *device = &adreno_dev->dev;
	struct adreno_gpudev *gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	int i, status = -EINVAL;
	unsigned int state = device->state;
	unsigned int regulator_left_on = 0;
	unsigned int pmqos_wakeup_vote = device->pwrctrl.pm_qos_wakeup_latency;
	unsigned int pmqos_active_vote = device->pwrctrl.pm_qos_active_latency;

	
	BUG_ON(test_bit(ADRENO_DEVICE_STARTED, &adreno_dev->priv));

	pm_qos_update_request(&device->pwrctrl.pm_qos_req_dma,
			pmqos_wakeup_vote);

	kgsl_cffdump_open(device);

	for (i = 0; i < KGSL_MAX_REGULATORS; i++) {
		if (device->pwrctrl.gpu_reg[i] &&
			regulator_is_enabled(device->pwrctrl.gpu_reg[i])) {
			regulator_left_on = 1;
			break;
		}
	}

	
	adreno_clear_gpu_fault(adreno_dev);

	
	status = kgsl_pwrctrl_change_state(device, KGSL_STATE_AWARE);
	if (status)
		goto error_pwr_off;

	
	set_bit(ADRENO_DEVICE_PWRON, &adreno_dev->priv);

	
	if (regulator_left_on)
		_soft_reset(adreno_dev);

	status = kgsl_mmu_start(device);
	if (status)
		goto error_pwr_off;

	status = adreno_ocmem_malloc(adreno_dev);
	if (status) {
		KGSL_DRV_ERR(device, "OCMEM malloc failed\n");
		goto error_mmu_off;
	}

	if (adreno_dev->perfctr_pwr_lo == 0) {
		int ret = adreno_perfcounter_get(adreno_dev,
			KGSL_PERFCOUNTER_GROUP_PWR, 1,
			&adreno_dev->perfctr_pwr_lo, NULL,
			PERFCOUNTER_FLAG_KERNEL);

		if (ret) {
			KGSL_DRV_ERR(device,
				"Unable to get the perf counters for DCVS\n");
			adreno_dev->perfctr_pwr_lo = 0;
		}
	}

	if (device->pwrctrl.bus_control) {
		int ret;

		
		if (adreno_dev->starved_ram_lo == 0) {
			ret = adreno_perfcounter_get(adreno_dev,
				KGSL_PERFCOUNTER_GROUP_VBIF_PWR, 0,
				&adreno_dev->starved_ram_lo, NULL,
				PERFCOUNTER_FLAG_KERNEL);

			if (ret) {
				KGSL_DRV_ERR(device,
					"Unable to get perf counters for bus DCVS\n");
				adreno_dev->starved_ram_lo = 0;
			}
		}

		
		if (adreno_dev->ram_cycles_lo == 0) {
			ret = adreno_perfcounter_get(adreno_dev,
				KGSL_PERFCOUNTER_GROUP_VBIF,
				VBIF_AXI_TOTAL_BEATS,
				&adreno_dev->ram_cycles_lo, NULL,
				PERFCOUNTER_FLAG_KERNEL);

			if (ret) {
				KGSL_DRV_ERR(device,
					"Unable to get perf counters for bus DCVS\n");
				adreno_dev->ram_cycles_lo = 0;
			}
		}
	}

	
	adreno_dev->busy_data.gpu_busy = 0;
	adreno_dev->busy_data.vbif_ram_cycles = 0;
	adreno_dev->busy_data.vbif_starved_ram = 0;

	
	adreno_perfcounter_restore(adreno_dev);

	
	gpudev->start(adreno_dev);

	
	adreno_coresight_start(adreno_dev);

	adreno_irqctrl(adreno_dev, 1);

	adreno_perfcounter_start(adreno_dev);

	status = adreno_ringbuffer_cold_start(adreno_dev);

	if (status)
		goto error_mmu_off;

	
	if (gpudev->lm_init)
		gpudev->lm_init(adreno_dev);

	if (device->mmu.secured)
		adreno_secvid_start(device);

	
	if (gpudev->enable_pc)
		gpudev->enable_pc(adreno_dev);

	
	if (gpudev->gpmu_start)
		gpudev->gpmu_start(adreno_dev);

	
	if (gpudev->enable_ppd)
		gpudev->enable_ppd(adreno_dev);

	
	if (gpudev->lm_enable)
		gpudev->lm_enable(adreno_dev);

	
	adreno_dispatcher_start(device);

	device->reset_counter++;

	set_bit(ADRENO_DEVICE_STARTED, &adreno_dev->priv);

	if (pmqos_active_vote != pmqos_wakeup_vote)
		pm_qos_update_request(&device->pwrctrl.pm_qos_req_dma,
				pmqos_active_vote);

	return 0;

error_mmu_off:
	kgsl_mmu_stop(&device->mmu);

error_pwr_off:
	
	kgsl_pwrctrl_change_state(device, state);

	if (pmqos_active_vote != pmqos_wakeup_vote)
		pm_qos_update_request(&device->pwrctrl.pm_qos_req_dma,
				pmqos_active_vote);

	return status;
}

static int adreno_start(struct kgsl_device *device, int priority)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	int nice = task_nice(current);
	int ret;

	if (priority && (adreno_wake_nice < nice))
		set_user_nice(current, adreno_wake_nice);

	ret = _adreno_start(adreno_dev);

	if (priority)
		set_user_nice(current, nice);

	return ret;
}

static int adreno_vbif_clear_pending_transactions(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_gpudev *gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	unsigned int mask = gpudev->vbif_xin_halt_ctrl0_mask;
	unsigned int val;
	unsigned long wait_for_vbif;
	int ret = 0;

	adreno_writereg(adreno_dev, ADRENO_REG_VBIF_XIN_HALT_CTRL0, mask);
	
	wait_for_vbif = jiffies + msecs_to_jiffies(100);
	while (1) {
		adreno_readreg(adreno_dev,
			ADRENO_REG_VBIF_XIN_HALT_CTRL1, &val);
		if ((val & mask) == mask)
			break;
		if (time_after(jiffies, wait_for_vbif)) {
			KGSL_DRV_ERR(device,
				"Wait limit reached for VBIF XIN Halt\n");
			ret = -ETIMEDOUT;
			break;
		}
	}
	adreno_writereg(adreno_dev, ADRENO_REG_VBIF_XIN_HALT_CTRL0, 0);
	return ret;
}

static int adreno_stop(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	if (!test_bit(ADRENO_DEVICE_STARTED, &adreno_dev->priv))
		return 0;

	adreno_set_active_ctxs_null(adreno_dev);

	adreno_dispatcher_stop(adreno_dev);

	adreno_ringbuffer_stop(adreno_dev);

	adreno_irqctrl(adreno_dev, 0);

	adreno_ocmem_free(adreno_dev);

	
	adreno_coresight_stop(adreno_dev);

	
	adreno_perfcounter_save(adreno_dev);

	kgsl_mmu_stop(&device->mmu);
	kgsl_cffdump_close(device);

	clear_bit(ADRENO_DEVICE_STARTED, &adreno_dev->priv);

	return 0;
}

int adreno_reset(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	int ret = -EINVAL;
	struct kgsl_mmu *mmu = &device->mmu;
	int i = 0;

	if (!atomic_read(&mmu->fault) && !adreno_is_a304(adreno_dev)
		&& !adreno_vbif_clear_pending_transactions(device)) {
		ret = adreno_soft_reset(device);
		if (ret)
			KGSL_DEV_ERR_ONCE(device, "Device soft reset failed\n");
	}
	if (ret) {
		
		kgsl_pwrctrl_change_state(device, KGSL_STATE_INIT);

		
		for (i = 0; i < NUM_TIMES_RESET_RETRY; i++) {
			ret = adreno_start(device, 0);
			if (!ret)
				break;

			msleep(20);
		}
	}
	if (ret)
		return ret;

	if (0 != i)
		KGSL_DRV_WARN(device, "Device hard reset tried %d tries\n", i);


	if (atomic_read(&device->active_cnt))
		kgsl_pwrctrl_change_state(device, KGSL_STATE_ACTIVE);
	else
		kgsl_pwrctrl_change_state(device, KGSL_STATE_NAP);

	
	kgsl_mmu_set_pt(&device->mmu, device->mmu.defaultpagetable);
	kgsl_sharedmem_writel(device,
		&adreno_dev->ringbuffers[0].pagetable_desc,
		offsetof(struct adreno_ringbuffer_pagetable_info,
			current_global_ptname), 0);

	return ret;
}

static int adreno_getproperty(struct kgsl_device *device,
				unsigned int type,
				void __user *value,
				size_t sizebytes)
{
	int status = -EINVAL;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	switch (type) {
	case KGSL_PROP_DEVICE_INFO:
		{
			struct kgsl_devinfo devinfo;

			if (sizebytes != sizeof(devinfo)) {
				status = -EINVAL;
				break;
			}

			memset(&devinfo, 0, sizeof(devinfo));
			devinfo.device_id = device->id+1;
			devinfo.chip_id = adreno_dev->chipid;
			devinfo.mmu_enabled = kgsl_mmu_enabled();
			devinfo.gmem_gpubaseaddr = adreno_dev->gmem_base;
			devinfo.gmem_sizebytes = adreno_dev->gmem_size;

			if (copy_to_user(value, &devinfo, sizeof(devinfo)) !=
					0) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_DEVICE_SHADOW:
		{
			struct kgsl_shadowprop shadowprop;

			if (sizebytes != sizeof(shadowprop)) {
				status = -EINVAL;
				break;
			}
			memset(&shadowprop, 0, sizeof(shadowprop));
			if (device->memstore.hostptr) {
				shadowprop.gpuaddr =
					(unsigned int) device->memstore.gpuaddr;
				shadowprop.size = device->memstore.size;
				shadowprop.flags = KGSL_FLAGS_INITIALIZED |
					KGSL_FLAGS_PER_CONTEXT_TIMESTAMPS;
			}
			if (copy_to_user(value, &shadowprop,
				sizeof(shadowprop))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_MMU_ENABLE:
		{
			int mmu_prop = kgsl_mmu_enabled();

			if (sizebytes != sizeof(int)) {
				status = -EINVAL;
				break;
			}
			if (copy_to_user(value, &mmu_prop, sizeof(mmu_prop))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_INTERRUPT_WAITS:
		{
			int int_waits = 1;
			if (sizebytes != sizeof(int)) {
				status = -EINVAL;
				break;
			}
			if (copy_to_user(value, &int_waits, sizeof(int))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_UCHE_GMEM_VADDR:
		{
			uint64_t gmem_vaddr = 0;
			if (adreno_is_a5xx(adreno_dev))
				gmem_vaddr = ADRENO_UCHE_GMEM_BASE;
			if (sizebytes != sizeof(uint64_t)) {
				status = -EINVAL;
				break;
			}
			if (copy_to_user(value, &gmem_vaddr,
					sizeof(uint64_t))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_SP_GENERIC_MEM:
		{
			struct kgsl_sp_generic_mem sp_mem;
			if (sizebytes != sizeof(sp_mem)) {
				status = -EINVAL;
				break;
			}
			memset(&sp_mem, 0, sizeof(sp_mem));

			sp_mem.local = adreno_dev->sp_local_gpuaddr;
			sp_mem.pvt = adreno_dev->sp_pvt_gpuaddr;

			if (copy_to_user(value, &sp_mem, sizeof(sp_mem))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_UCODE_VERSION:
		{
			struct kgsl_ucode_version ucode;

			if (sizebytes != sizeof(ucode)) {
				status = -EINVAL;
				break;
			}
			memset(&ucode, 0, sizeof(ucode));

			ucode.pfp = adreno_dev->pfp_fw_version;
			ucode.pm4 = adreno_dev->pm4_fw_version;

			if (copy_to_user(value, &ucode, sizeof(ucode))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_GPMU_VERSION:
		{
			struct kgsl_gpmu_version gpmu;

			if (adreno_dev->gpucore == NULL) {
				status = -EINVAL;
				break;
			}

			if (!ADRENO_FEATURE(adreno_dev, ADRENO_GPMU)) {
				status = -EOPNOTSUPP;
				break;
			}

			if (sizebytes != sizeof(gpmu)) {
				status = -EINVAL;
				break;
			}
			memset(&gpmu, 0, sizeof(gpmu));

			gpmu.major = adreno_dev->gpucore->gpmu_major;
			gpmu.minor = adreno_dev->gpucore->gpmu_minor;
			gpmu.features = adreno_dev->gpucore->gpmu_features;

			if (copy_to_user(value, &gpmu, sizeof(gpmu))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_DEVICE_BITNESS:
	{
		unsigned int bitness = 32;

		if (sizebytes != sizeof(unsigned int)) {
			status = -EINVAL;
			break;
		}

		if (copy_to_user(value, &bitness,
				sizeof(unsigned int))) {
			status = -EFAULT;
			break;
		}
		status = 0;
	}
	break;
	default:
		status = -EINVAL;
	}

	return status;
}

int adreno_set_constraint(struct kgsl_device *device,
				struct kgsl_context *context,
				struct kgsl_device_constraint *constraint)
{
	int status = 0;

	switch (constraint->type) {
	case KGSL_CONSTRAINT_PWRLEVEL: {
		struct kgsl_device_constraint_pwrlevel pwr;

		if (constraint->size != sizeof(pwr)) {
			status = -EINVAL;
			break;
		}

		if (copy_from_user(&pwr,
				(void __user *)constraint->data,
				sizeof(pwr))) {
			status = -EFAULT;
			break;
		}
		if (pwr.level >= KGSL_CONSTRAINT_PWR_MAXLEVELS) {
			status = -EINVAL;
			break;
		}

		context->pwr_constraint.type =
				KGSL_CONSTRAINT_PWRLEVEL;
		context->pwr_constraint.sub_type = pwr.level;
		trace_kgsl_user_pwrlevel_constraint(device,
			context->id,
			context->pwr_constraint.type,
			context->pwr_constraint.sub_type);
		}
		break;
	case KGSL_CONSTRAINT_NONE:
		if (context->pwr_constraint.type == KGSL_CONSTRAINT_PWRLEVEL)
			trace_kgsl_user_pwrlevel_constraint(device,
				context->id,
				KGSL_CONSTRAINT_NONE,
				context->pwr_constraint.sub_type);
		context->pwr_constraint.type = KGSL_CONSTRAINT_NONE;
		break;

	default:
		status = -EINVAL;
		break;
	}

	
	if ((status == 0) &&
		(context->id == device->pwrctrl.constraint.owner_id)) {
		trace_kgsl_constraint(device, device->pwrctrl.constraint.type,
					device->pwrctrl.active_pwrlevel, 0);
		device->pwrctrl.constraint.type = KGSL_CONSTRAINT_NONE;
	}

	return status;
}

static int adreno_setproperty(struct kgsl_device_private *dev_priv,
				unsigned int type,
				void __user *value,
				unsigned int sizebytes)
{
	int status = -EINVAL;
	struct kgsl_device *device = dev_priv->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	switch (type) {
	case KGSL_PROP_PWRCTRL: {
			unsigned int enable;

			if (sizebytes != sizeof(enable))
				break;

			if (copy_from_user(&enable, value, sizeof(enable))) {
				status = -EFAULT;
				break;
			}

			mutex_lock(&device->mutex);

			if (enable) {
				device->pwrctrl.ctrl_flags = 0;

				if (!kgsl_active_count_get(&adreno_dev->dev)) {
					adreno_fault_detect_start(adreno_dev);
					kgsl_active_count_put(&adreno_dev->dev);
				}

				kgsl_pwrscale_enable(device);
			} else {
				kgsl_pwrctrl_change_state(device,
							KGSL_STATE_ACTIVE);
				device->pwrctrl.ctrl_flags = KGSL_PWR_ON;
				adreno_fault_detect_stop(adreno_dev);
				kgsl_pwrscale_disable(device);
			}

			mutex_unlock(&device->mutex);
			status = 0;
		}
		break;
	case KGSL_PROP_PWR_CONSTRAINT: {
			struct kgsl_device_constraint constraint;
			struct kgsl_context *context;

			if (sizebytes != sizeof(constraint))
				break;

			if (copy_from_user(&constraint, value,
				sizeof(constraint))) {
				status = -EFAULT;
				break;
			}

			context = kgsl_context_get_owner(dev_priv,
							constraint.context_id);

			if (context == NULL)
				break;

			status = adreno_set_constraint(device, context,
								&constraint);

			kgsl_context_put(context);
		}
		break;
	default:
		break;
	}

	return status;
}

inline unsigned int adreno_irq_pending(struct adreno_device *adreno_dev)
{
	struct adreno_gpudev *gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	unsigned int status;

	adreno_readreg(adreno_dev, ADRENO_REG_RBBM_INT_0_STATUS, &status);

	return (status & gpudev->irq->mask) ? 1 : 0;
}


bool adreno_hw_isidle(struct adreno_device *adreno_dev)
{
	const struct adreno_gpu_core *gpucore = adreno_dev->gpucore;
	unsigned int reg_rbbm_status;

	adreno_readreg(adreno_dev, ADRENO_REG_RBBM_STATUS,
		&reg_rbbm_status);

	if (reg_rbbm_status & gpucore->busy_mask)
		return false;

	
	if (adreno_irq_pending(adreno_dev))
		return false;

	return true;
}

static int adreno_soft_reset(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_gpudev *gpudev = ADRENO_GPU_DEVICE(adreno_dev);
	int ret;

	kgsl_pwrctrl_change_state(device, KGSL_STATE_AWARE);
	adreno_set_active_ctxs_null(adreno_dev);

	adreno_irqctrl(adreno_dev, 0);

	adreno_clear_gpu_fault(adreno_dev);
	
	clear_bit(ADRENO_DEVICE_STARTED, &adreno_dev->priv);

	
	adreno_perfcounter_save(adreno_dev);

	kgsl_cffdump_close(device);
	
	_soft_reset(adreno_dev);

	
	kgsl_cffdump_open(device);

	
	adreno_perfcounter_restore(adreno_dev);

	
	gpudev->start(adreno_dev);

	
	adreno_coresight_start(adreno_dev);

	
	adreno_irqctrl(adreno_dev, 1);

	
	adreno_ringbuffer_stop(adreno_dev);

	if (ADRENO_FEATURE(adreno_dev, ADRENO_WARM_START))
		ret = adreno_ringbuffer_warm_start(adreno_dev);
	else
		ret = adreno_ringbuffer_cold_start(adreno_dev);

	if (!ret) {
		device->reset_counter++;
		
		set_bit(ADRENO_DEVICE_STARTED, &adreno_dev->priv);
	}

	return ret;
}

bool adreno_isidle(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb;
	int i;

	if (!kgsl_state_is_awake(device))
		return true;

	adreno_get_rptr(ADRENO_CURRENT_RINGBUFFER(adreno_dev));

	smp_mb();

	FOR_EACH_RINGBUFFER(adreno_dev, rb, i) {
		if (rb->rptr != rb->wptr)
			break;
	}

	if (i == adreno_dev->num_ringbuffers)
		return adreno_hw_isidle(adreno_dev);

	return false;
}

int adreno_spin_idle(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	unsigned long wait = jiffies + msecs_to_jiffies(ADRENO_IDLE_TIMEOUT);

	kgsl_cffdump_regpoll(device,
		adreno_getreg(adreno_dev, ADRENO_REG_RBBM_STATUS) << 2,
		0x00000000, 0x80000000);

	while (time_before(jiffies, wait)) {

		if (adreno_gpu_fault(adreno_dev) != 0)
			return -EDEADLK;

		if (adreno_isidle(device))
			return 0;
	}

	return -ETIMEDOUT;
}


int adreno_idle(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	int ret;


	BUG_ON(!mutex_is_locked(&device->mutex));

	
	if (adreno_isidle(device))
		return 0;
	ret = adreno_dispatcher_idle(adreno_dev);
	if (ret)
		return ret;

	return adreno_spin_idle(device);
}

static int adreno_drain(struct kgsl_device *device)
{
	INIT_COMPLETION(device->cmdbatch_gate);

	return 0;
}

static int adreno_suspend_context(struct kgsl_device *device)
{
	int status = 0;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	
	adreno_profile_process_results(adreno_dev);

	status = adreno_idle(device);
	if (status)
		return status;
	
	kgsl_mmu_set_pt(&device->mmu, device->mmu.defaultpagetable);
	kgsl_sharedmem_writel(device,
		&adreno_dev->ringbuffers[0].pagetable_desc,
		offsetof(struct adreno_ringbuffer_pagetable_info,
			current_global_ptname), 0);
	
	adreno_set_active_ctxs_null(adreno_dev);

	return status;
}

static void adreno_read(struct kgsl_device *device, void __iomem *base,
		unsigned int offsetwords, unsigned int *value,
		unsigned int mem_len)
{

	unsigned int __iomem *reg;
	BUG_ON(offsetwords*sizeof(uint32_t) >= mem_len);
	reg = (unsigned int __iomem *)(base + (offsetwords << 2));

	if (!in_interrupt())
		kgsl_pre_hwaccess(device);

	*value = __raw_readl(reg);
	rmb();
}

static void adreno_regread(struct kgsl_device *device, unsigned int offsetwords,
	unsigned int *value)
{
	adreno_read(device, device->reg_virt, offsetwords, value,
						device->reg_len);
}

void adreno_shadermem_regread(struct kgsl_device *device,
	unsigned int offsetwords, unsigned int *value)
{
	adreno_read(device, device->shader_mem_virt, offsetwords, value,
					device->shader_mem_len);
}

static void adreno_regwrite(struct kgsl_device *device,
				unsigned int offsetwords,
				unsigned int value)
{
	unsigned int __iomem *reg;

	BUG_ON(offsetwords*sizeof(uint32_t) >= device->reg_len);

	if (!in_interrupt())
		kgsl_pre_hwaccess(device);

	trace_kgsl_regwrite(device, offsetwords, value);

	kgsl_cffdump_regwrite(device, offsetwords << 2, value);
	reg = (unsigned int __iomem *)(device->reg_virt + (offsetwords << 2));

	wmb();
	__raw_writel(value, reg);
}

static int adreno_waittimestamp(struct kgsl_device *device,
		struct kgsl_context *context,
		unsigned int timestamp,
		unsigned int msecs)
{
	int ret;

	if (context == NULL) {
		
		dev_WARN_ONCE(device->dev, 1,
			"IOCTL_KGSL_DEVICE_WAITTIMESTAMP is deprecated\n");
		return -ENOTTY;
	}

	
	if (kgsl_context_detached(context))
		return -ENOENT;

	ret = adreno_drawctxt_wait(ADRENO_DEVICE(device), context,
		timestamp, msecs);

	
	if (kgsl_context_invalid(context))
		ret = -EDEADLK;


	if (!ret && test_and_clear_bit(ADRENO_CONTEXT_FAULT, &context->priv))
		ret = -EPROTO;

	return ret;
}

int __adreno_readtimestamp(struct kgsl_device *device, int index, int type,
		unsigned int *timestamp)
{
	int status = 0;

	switch (type) {
	case KGSL_TIMESTAMP_CONSUMED:
		kgsl_sharedmem_readl(&device->memstore, timestamp,
			KGSL_MEMSTORE_OFFSET(index, soptimestamp));
		break;
	case KGSL_TIMESTAMP_RETIRED:
		kgsl_sharedmem_readl(&device->memstore, timestamp,
			KGSL_MEMSTORE_OFFSET(index, eoptimestamp));
		break;
	default:
		status = -EINVAL;
		*timestamp = 0;
		break;
	}
	return status;
}

/**
 * adreno_rb_readtimestamp(): Return the value of given type of timestamp
 * for a RB
 * @device: GPU device whose timestamp values are being queried
 * @priv: The object being queried for a timestamp (expected to be a rb pointer)
 * @type: The type of timestamp (one of 3) to be read
 * @timestamp: Pointer to where the read timestamp is to be written to
 *
 * CONSUMED and RETIRED type timestamps are sorted by id and are constantly
 * updated by the GPU through shared memstore memory. QUEUED type timestamps
 * are read directly from context struct.

 * The function returns 0 on success and timestamp value at the *timestamp
 * address and returns -EINVAL on any read error/invalid type and timestamp = 0.
 */
int adreno_rb_readtimestamp(struct kgsl_device *device,
		void *priv, enum kgsl_timestamp_type type,
		unsigned int *timestamp)
{
	int status = 0;
	struct adreno_ringbuffer *rb = priv;

	if (!timestamp)
		return status;

	if (KGSL_TIMESTAMP_QUEUED == type)
		*timestamp = rb->timestamp;
	else
		status = __adreno_readtimestamp(device,
				rb->id + KGSL_MEMSTORE_MAX,
				type, timestamp);

	return status;
}

/**
 * adreno_readtimestamp(): Return the value of given type of timestamp
 * @device: GPU device whose timestamp values are being queried
 * @priv: The object being queried for a timestamp (expected to be a context)
 * @type: The type of timestamp (one of 3) to be read
 * @timestamp: Pointer to where the read timestamp is to be written to
 *
 * CONSUMED and RETIRED type timestamps are sorted by id and are constantly
 * updated by the GPU through shared memstore memory. QUEUED type timestamps
 * are read directly from context struct.

 * The function returns 0 on success and timestamp value at the *timestamp
 * address and returns -EINVAL on any read error/invalid type and timestamp = 0.
 */
static int adreno_readtimestamp(struct kgsl_device *device,
		void *priv, enum kgsl_timestamp_type type,
		unsigned int *timestamp)
{
	int status = 0;
	struct kgsl_context *context = priv;
	unsigned int id = KGSL_CONTEXT_ID(context);

	BUG_ON(NULL == context || id >= KGSL_MEMSTORE_MAX);
	if (!timestamp)
		return status;

	if (KGSL_TIMESTAMP_QUEUED == type)
		*timestamp = adreno_context_timestamp(context);
	else
		status = __adreno_readtimestamp(device,
				context->id, type, timestamp);

	return status;
}

static inline s64 adreno_ticks_to_us(u32 ticks, u32 freq)
{
	freq /= 1000000;
	return ticks / freq;
}

static unsigned int counter_delta(struct adreno_device *adreno_dev,
			unsigned int reg, unsigned int *counter)
{
	struct kgsl_device *device = &adreno_dev->dev;
	unsigned int val;
	unsigned int ret = 0;

	
	kgsl_regread(device, reg, &val);

	
	if (*counter != 0) {
		if (val < *counter)
			ret = (0xFFFFFFFF - *counter) + val;
		else
			ret = val - *counter;
	}

	*counter = val;
	return ret;
}

static void adreno_power_stats(struct kgsl_device *device,
				struct kgsl_power_stats *stats)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	struct adreno_busy_data *busy = &adreno_dev->busy_data;

	memset(stats, 0, sizeof(*stats));

	
	if (adreno_dev->perfctr_pwr_lo != 0) {
		uint64_t gpu_busy;

		gpu_busy = counter_delta(adreno_dev, adreno_dev->perfctr_pwr_lo,
			&busy->gpu_busy);

		stats->busy_time = adreno_ticks_to_us(gpu_busy,
			kgsl_pwrctrl_active_freq(pwr));
	}

	if (device->pwrctrl.bus_control) {
		uint64_t ram_cycles = 0, starved_ram = 0;

		if (adreno_dev->ram_cycles_lo != 0)
			ram_cycles = counter_delta(adreno_dev,
				adreno_dev->ram_cycles_lo,
				&busy->vbif_ram_cycles);

		if (adreno_dev->starved_ram_lo != 0)
			starved_ram = counter_delta(adreno_dev,
				adreno_dev->starved_ram_lo,
				&busy->vbif_starved_ram);

		stats->ram_time = ram_cycles;
		stats->ram_wait = starved_ram;
	}
}

static unsigned int adreno_gpuid(struct kgsl_device *device,
	unsigned int *chipid)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);


	if (chipid != NULL)
		*chipid = adreno_dev->chipid;


	return (0x0003 << 16) | ADRENO_GPUREV(adreno_dev);
}

static int adreno_regulator_enable(struct kgsl_device *device)
{
	int ret = 0;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_gpudev *gpudev  = ADRENO_GPU_DEVICE(adreno_dev);
	if (gpudev->regulator_enable &&
		!test_bit(ADRENO_DEVICE_GPU_REGULATOR_ENABLED,
			&adreno_dev->priv)) {
		ret = gpudev->regulator_enable(adreno_dev);
		if (!ret)
			set_bit(ADRENO_DEVICE_GPU_REGULATOR_ENABLED,
				&adreno_dev->priv);
	}
	return ret;
}

static bool adreno_is_hw_collapsible(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_gpudev *gpudev  = ADRENO_GPU_DEVICE(adreno_dev);

	if (adreno_is_a304(adreno_dev) &&
			device->pwrctrl.ctrl_flags)
		return false;

	return adreno_isidle(device) && (gpudev->is_sptp_idle ?
				gpudev->is_sptp_idle(adreno_dev) : true);
}

static void adreno_regulator_disable(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_gpudev *gpudev  = ADRENO_GPU_DEVICE(adreno_dev);
	if (gpudev->regulator_disable &&
		test_bit(ADRENO_DEVICE_GPU_REGULATOR_ENABLED,
			&adreno_dev->priv)) {
		gpudev->regulator_disable(adreno_dev);
		clear_bit(ADRENO_DEVICE_GPU_REGULATOR_ENABLED,
			&adreno_dev->priv);
	}
}

static void adreno_pwrlevel_change_settings(struct kgsl_device *device,
		unsigned int prelevel, unsigned int postlevel, bool post)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_gpudev *gpudev  = ADRENO_GPU_DEVICE(adreno_dev);

	if (gpudev->pwrlevel_change_settings)
		gpudev->pwrlevel_change_settings(adreno_dev, prelevel,
					postlevel, post);
}

static const struct kgsl_functable adreno_functable = {
	
	.regread = adreno_regread,
	.regwrite = adreno_regwrite,
	.idle = adreno_idle,
	.isidle = adreno_isidle,
	.suspend_context = adreno_suspend_context,
	.init = adreno_init,
	.start = adreno_start,
	.stop = adreno_stop,
	.getproperty = adreno_getproperty,
	.getproperty_compat = adreno_getproperty_compat,
	.waittimestamp = adreno_waittimestamp,
	.readtimestamp = adreno_readtimestamp,
	.issueibcmds = adreno_ringbuffer_issueibcmds,
	.ioctl = adreno_ioctl,
	.compat_ioctl = adreno_compat_ioctl,
	.power_stats = adreno_power_stats,
	.gpuid = adreno_gpuid,
	.snapshot = adreno_snapshot,
	.irq_handler = adreno_irq_handler,
	.drain = adreno_drain,
	
	.drawctxt_create = adreno_drawctxt_create,
	.drawctxt_detach = adreno_drawctxt_detach,
	.drawctxt_destroy = adreno_drawctxt_destroy,
	.drawctxt_dump = adreno_drawctxt_dump,
	.setproperty = adreno_setproperty,
	.setproperty_compat = adreno_setproperty_compat,
	.drawctxt_sched = adreno_drawctxt_sched,
	.resume = adreno_dispatcher_start,
	.regulator_enable = adreno_regulator_enable,
	.is_hw_collapsible = adreno_is_hw_collapsible,
	.regulator_disable = adreno_regulator_disable,
	.pwrlevel_change_settings = adreno_pwrlevel_change_settings,
};

static struct platform_driver adreno_platform_driver = {
	.probe = adreno_probe,
	.remove = adreno_remove,
	.suspend = kgsl_suspend_driver,
	.resume = kgsl_resume_driver,
	.id_table = adreno_id_table,
	.driver = {
		.owner = THIS_MODULE,
		.name = DEVICE_3D_NAME,
		.pm = &kgsl_pm_ops,
		.of_match_table = adreno_match_table,
	}
};

static int __init kgsl_3d_init(void)
{
	return platform_driver_register(&adreno_platform_driver);
}

static void __exit kgsl_3d_exit(void)
{
	platform_driver_unregister(&adreno_platform_driver);
}

module_init(kgsl_3d_init);
module_exit(kgsl_3d_exit);


static struct of_device_id busmon_match_table[] = {
	{ .compatible = "qcom,kgsl-busmon", .data = &device_3d0 },
	{}
};

static int kgsl_busmon_probe(struct platform_device *pdev)
{
	struct kgsl_device *device;
	const struct of_device_id *pdid =
			of_match_device(busmon_match_table, &pdev->dev);

	if (pdid == NULL)
		return -ENXIO;

	device = (struct kgsl_device *)pdid->data;
	device->busmondev = &pdev->dev;
	dev_set_drvdata(device->busmondev, device);

	return 0;
}

static struct platform_driver kgsl_bus_platform_driver = {
	.probe = kgsl_busmon_probe,
	.driver = {
		.owner = THIS_MODULE,
		.name = "kgsl-busmon",
	.of_match_table = busmon_match_table,
	}
};

static int __init kgsl_busmon_init(void)
{
	return platform_driver_register(&kgsl_bus_platform_driver);
}

static void __exit kgsl_busmon_exit(void)
{
	platform_driver_unregister(&kgsl_bus_platform_driver);
}

module_init(kgsl_busmon_init);
module_exit(kgsl_busmon_exit);

MODULE_DESCRIPTION("3D Graphics driver");
MODULE_VERSION("1.2");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:kgsl_3d");
