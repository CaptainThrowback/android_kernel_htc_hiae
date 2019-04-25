/* Copyright (c) 2012-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */



#ifndef MSM_JPEG_SYNC_H
#define MSM_JPEG_SYNC_H

#include <linux/fs.h>
#include <linux/list.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include "msm_camera_io_util.h"
#include "msm_jpeg_hw.h"
#include "cam_smmu_api.h"

#define JPEG_8974_V1 0x10000000
#define JPEG_8974_V2 0x10010000
#define JPEG_8994 0x10020000
#define JPEG_CLK_MAX 16

enum msm_jpeg_state {
	MSM_JPEG_INIT,
	MSM_JPEG_RESET,
	MSM_JPEG_EXECUTING,
	MSM_JPEG_STOPPED,
	MSM_JPEG_IDLE
};

enum msm_jpeg_core_type {
	MSM_JPEG_CORE_CODEC,
	MSM_JPEG_CORE_DMA
};

struct msm_jpeg_q {
	char const	*name;
	struct list_head  q;
	spinlock_t	lck;
	wait_queue_head_t wait;
	int	       unblck;
};

struct msm_jpeg_q_entry {
	struct list_head list;
	void   *data;
};

struct msm_jpeg_device {
	struct platform_device *pdev;
	struct resource        *mem;
	int                     irq;
	void                   *base;
	struct clk *jpeg_clk[JPEG_CLK_MAX];
	struct msm_cam_clk_info jpeg_clk_info[JPEG_CLK_MAX];

	struct regulator *jpeg_fs;
	uint32_t hw_version;

	struct device *device;
	struct cdev   cdev;
	struct mutex  lock;
	char	  open_count;
	uint8_t       op_mode;

	int jpeg_bus_vote;

	struct msm_jpeg_q evt_q;

	struct msm_jpeg_q output_rtn_q;

	struct msm_jpeg_q output_buf_q;

	struct msm_jpeg_q input_rtn_q;

	struct msm_jpeg_q input_buf_q;

	struct v4l2_subdev subdev;

	struct class *msm_jpeg_class;

	dev_t msm_jpeg_devno;

	
	int idx;
	int iommu_hdl;
	int decode_flag;
	struct ion_client *jpeg_client;
	void *jpeg_vbif;
	int release_buf;
	struct msm_jpeg_hw_pingpong fe_pingpong_buf;
	struct msm_jpeg_hw_pingpong we_pingpong_buf;
	int we_pingpong_index;
	int reset_done_ack;
	spinlock_t reset_lock;
	wait_queue_head_t reset_wait;
	uint32_t res_size;
	uint32_t jpeg_bus_client;
	uint32_t num_clk;
	enum msm_jpeg_state state;
	enum msm_jpeg_core_type core_type;
};

int __msm_jpeg_open(struct msm_jpeg_device *pgmn_dev);
int __msm_jpeg_release(struct msm_jpeg_device *pgmn_dev);

long __msm_jpeg_ioctl(struct msm_jpeg_device *pgmn_dev,
	unsigned int cmd, unsigned long arg);

#ifdef CONFIG_COMPAT
long __msm_jpeg_compat_ioctl(struct msm_jpeg_device *pgmn_dev,
	unsigned int cmd, unsigned long arg);
#endif

int __msm_jpeg_init(struct msm_jpeg_device *pgmn_dev);
int __msm_jpeg_exit(struct msm_jpeg_device *pgmn_dev);

#endif 
