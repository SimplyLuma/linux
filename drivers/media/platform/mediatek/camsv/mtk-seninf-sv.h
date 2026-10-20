/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MTK_SENINF_SV_H__
#define __MTK_SENINF_SV_H__

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pm_domain.h>
#include <linux/spinlock.h>
#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-v4l2.h>

#define MTK_SV_MAX_WIDTH	8192
#define MTK_SV_MAX_HEIGHT	6144
#define MTK_SV_STRIDE_ALIGN	16
#define MTK_SV_IRQ_FRAME_DONE	BIT(0)

struct mtk_sv_format {
	u32 fourcc;
	u32 code;
	u8 bpp;
};

struct mtk_sv_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	unsigned long iova;
	size_t mapped;
};

struct mtk_sv_hw;

struct mtk_sv {
	struct device *dev;
	struct media_device mdev;
	struct v4l2_device v4l2_dev;
	struct v4l2_async_notifier notifier;
	struct v4l2_subdev *sensor;
	bool cphy;
	unsigned int num_lanes;

	struct v4l2_subdev seninf_sd;
	struct media_pad seninf_pads[2];

	struct video_device vdev;
	struct media_pad vdev_pad;
	struct vb2_queue queue;
	struct mutex lock;
	struct v4l2_pix_format pix;
	const struct mtk_sv_format *fmt;

	spinlock_t buf_lock;
	struct list_head buf_list;
	struct mtk_sv_buffer *active;
	bool streaming;
	u32 sequence;

	struct dev_pm_domain_list *pd_list;
	struct mtk_sv_hw *hw;
};

/* mtk-seninf-sv-hw.c */
int mtk_sv_hw_init(struct mtk_sv *sv, struct platform_device *pdev);
void mtk_sv_hw_exit(struct mtk_sv *sv);
int mtk_sv_hw_seninf_start(struct mtk_sv *sv, const struct v4l2_mbus_framefmt *fmt);
void mtk_sv_hw_seninf_stop(struct mtk_sv *sv);
int mtk_sv_hw_camsv_start(struct mtk_sv *sv, const struct v4l2_pix_format *pix);
void mtk_sv_hw_camsv_stop(struct mtk_sv *sv);
void mtk_sv_hw_set_addr(struct mtk_sv *sv, dma_addr_t addr);
u32 mtk_sv_hw_irq_status(struct mtk_sv *sv);

#endif
