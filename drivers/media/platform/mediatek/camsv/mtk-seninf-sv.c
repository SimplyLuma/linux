// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6878 raw capture: CSI-2 receiver (seninf) feeding a CAMSV
 * DMA engine, without the ISP.
 *
 * Media graph: sensor -> seninf subdev -> camsv video node. The platform
 * driver is named "mtk-seninf" so the libcamera simple pipeline handler
 * picks it up.
 *
 * Buffers are scatter-gather pages mapped into the MM M4U (larb13 runs
 * translated), one fixed IOVA slot per vb2 buffer index.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-dma-sg.h>

#include "mt6878-cam-m4u.h"
#include "mtk-seninf-sv.h"

#define SENINF_PAD_SINK		0
#define SENINF_PAD_SOURCE	1

#define MTK_SV_IOVA_BASE	0x40000000UL
#define MTK_SV_IOVA_SLOT	SZ_64M
#define MTK_SV_MAX_BUFFERS	32

static const struct mtk_sv_format mtk_sv_formats[] = {
	/* RAW10 on the bus, 8 MSBs in memory */
	{ V4L2_PIX_FMT_SRGGB8, MEDIA_BUS_FMT_SRGGB10_1X10, 8 },
	{ V4L2_PIX_FMT_SGRBG8, MEDIA_BUS_FMT_SGRBG10_1X10, 8 },
	{ V4L2_PIX_FMT_SGBRG8, MEDIA_BUS_FMT_SGBRG10_1X10, 8 },
	{ V4L2_PIX_FMT_SBGGR8, MEDIA_BUS_FMT_SBGGR10_1X10, 8 },
};

static const struct mtk_sv_format *mtk_sv_find_format(u32 fourcc, u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mtk_sv_formats); i++)
		if (mtk_sv_formats[i].fourcc == fourcc || mtk_sv_formats[i].code == code)
			return &mtk_sv_formats[i];
	return NULL;
}

static void mtk_sv_fill_pix(struct v4l2_pix_format *pix, const struct mtk_sv_format *f,
			    u32 width, u32 height)
{
	pix->pixelformat = f->fourcc;
	pix->width = clamp_t(u32, width, 16, MTK_SV_MAX_WIDTH) & ~1;
	pix->height = clamp_t(u32, height, 16, MTK_SV_MAX_HEIGHT) & ~1;
	pix->field = V4L2_FIELD_NONE;
	/* stride aligned for the DMA */
	pix->bytesperline = ALIGN(DIV_ROUND_UP(pix->width * f->bpp, 8), MTK_SV_STRIDE_ALIGN);
	pix->sizeimage = pix->bytesperline * pix->height;
	pix->colorspace = V4L2_COLORSPACE_RAW;
}

/* seninf subdev */

static int mtk_seninf_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt;
	unsigned int pad;

	for (pad = 0; pad < 2; pad++) {
		fmt = v4l2_subdev_state_get_format(state, pad);
		fmt->width = 4096;
		fmt->height = 3072;
		fmt->code = mtk_sv_formats[0].code;
		fmt->field = V4L2_FIELD_NONE;
		fmt->colorspace = V4L2_COLORSPACE_RAW;
	}
	return 0;
}

static int mtk_seninf_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	if (!mtk_sv_find_format(0, fmt->format.code))
		fmt->format.code = mtk_sv_formats[0].code;
	fmt->format.field = V4L2_FIELD_NONE;

	/* The source pad always mirrors the sink pad. */
	if (fmt->pad == SENINF_PAD_SOURCE) {
		fmt->format = *v4l2_subdev_state_get_format(state, SENINF_PAD_SINK);
		return 0;
	}
	*v4l2_subdev_state_get_format(state, SENINF_PAD_SINK) = fmt->format;
	*v4l2_subdev_state_get_format(state, SENINF_PAD_SOURCE) = fmt->format;
	return 0;
}

static int mtk_seninf_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(mtk_sv_formats))
		return -EINVAL;
	code->code = mtk_sv_formats[code->index].code;
	return 0;
}

static int mtk_seninf_enable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				     u32 pad, u64 streams_mask)
{
	struct mtk_sv *sv = container_of(sd, struct mtk_sv, seninf_sd);
	const struct v4l2_mbus_framefmt *fmt = v4l2_subdev_state_get_format(state, SENINF_PAD_SINK);
	struct media_pad *remote;
	int ret;

	remote = media_pad_remote_pad_unique(&sv->seninf_pads[SENINF_PAD_SINK]);
	if (IS_ERR(remote))
		return PTR_ERR(remote);

	ret = mtk_sv_hw_seninf_start(sv, fmt);
	if (ret)
		return ret;

	ret = v4l2_subdev_enable_streams(sv->sensor, remote->index, BIT(0));
	if (ret)
		mtk_sv_hw_seninf_stop(sv);
	return ret;
}

static int mtk_seninf_disable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				      u32 pad, u64 streams_mask)
{
	struct mtk_sv *sv = container_of(sd, struct mtk_sv, seninf_sd);
	struct media_pad *remote;

	remote = media_pad_remote_pad_unique(&sv->seninf_pads[SENINF_PAD_SINK]);
	if (!IS_ERR(remote))
		v4l2_subdev_disable_streams(sv->sensor, remote->index, BIT(0));
	mtk_sv_hw_seninf_stop(sv);
	return 0;
}

static const struct v4l2_subdev_pad_ops mtk_seninf_pad_ops = {
	.enum_mbus_code = mtk_seninf_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = mtk_seninf_set_fmt,
	.link_validate = v4l2_subdev_link_validate_default,
	.enable_streams = mtk_seninf_enable_streams,
	.disable_streams = mtk_seninf_disable_streams,
};

static const struct v4l2_subdev_video_ops mtk_seninf_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_ops mtk_seninf_ops = {
	.video = &mtk_seninf_video_ops,
	.pad = &mtk_seninf_pad_ops,
};

static const struct v4l2_subdev_internal_ops mtk_seninf_internal_ops = {
	.init_state = mtk_seninf_init_state,
};

static const struct media_entity_operations mtk_seninf_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* camsv video node */

static void mtk_sv_next_buffer(struct mtk_sv *sv);

static int mtk_sv_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			      unsigned int *nplanes, unsigned int sizes[],
			      struct device *alloc_devs[])
{
	struct mtk_sv *sv = vb2_get_drv_priv(vq);

	if (*nplanes)
		return sizes[0] < sv->pix.sizeimage ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = sv->pix.sizeimage;
	return 0;
}

static struct mtk_sv_buffer *to_mtk_sv_buffer(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct mtk_sv_buffer, vb);
}

static int mtk_sv_buf_init(struct vb2_buffer *vb)
{
	struct mtk_sv_buffer *buf = to_mtk_sv_buffer(vb);
	int ret;

	if (vb->index >= MTK_SV_MAX_BUFFERS || vb2_plane_size(vb, 0) > MTK_SV_IOVA_SLOT)
		return -EINVAL;
	buf->iova = MTK_SV_IOVA_BASE + vb->index * MTK_SV_IOVA_SLOT;
	ret = mt6878_cam_m4u_map_sg(buf->iova, vb2_dma_sg_plane_desc(vb, 0), &buf->mapped);
	if (ret) {
		mt6878_cam_m4u_unmap(buf->iova, buf->mapped);
		buf->mapped = 0;
	}
	return ret;
}

static void mtk_sv_buf_cleanup(struct vb2_buffer *vb)
{
	struct mtk_sv_buffer *buf = to_mtk_sv_buffer(vb);

	mt6878_cam_m4u_unmap(buf->iova, buf->mapped);
	buf->mapped = 0;
}

static int mtk_sv_buf_prepare(struct vb2_buffer *vb)
{
	struct mtk_sv *sv = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < sv->pix.sizeimage)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, sv->pix.sizeimage);
	return 0;
}

static void mtk_sv_buf_queue(struct vb2_buffer *vb)
{
	struct mtk_sv *sv = vb2_get_drv_priv(vb->vb2_queue);
	struct mtk_sv_buffer *buf = container_of(to_vb2_v4l2_buffer(vb),
						 struct mtk_sv_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&sv->buf_lock, flags);
	list_add_tail(&buf->list, &sv->buf_list);
	if (sv->streaming && !sv->active)
		mtk_sv_next_buffer(sv);
	spin_unlock_irqrestore(&sv->buf_lock, flags);
}

static void mtk_sv_return_buffers(struct mtk_sv *sv, enum vb2_buffer_state state)
{
	struct mtk_sv_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&sv->buf_lock, flags);
	if (sv->active) {
		vb2_buffer_done(&sv->active->vb.vb2_buf, state);
		sv->active = NULL;
	}
	list_for_each_entry_safe(buf, tmp, &sv->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&sv->buf_lock, flags);
}

/*
 * Called with buf_lock held: hand the next queued buffer to the hardware.
 * With none queued the FBC has no buffer and drops frames until one arrives.
 */
static void mtk_sv_next_buffer(struct mtk_sv *sv)
{
	struct mtk_sv_buffer *buf;

	buf = list_first_entry_or_null(&sv->buf_list, struct mtk_sv_buffer, list);
	sv->active = buf;
	if (buf) {
		list_del(&buf->list);
		mtk_sv_hw_set_addr(sv, buf->iova);
	}
}

static int mtk_sv_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct mtk_sv *sv = vb2_get_drv_priv(vq);
	unsigned long flags;
	int ret;

	sv->sequence = 0;
	ret = pm_runtime_resume_and_get(sv->dev);
	if (ret)
		goto err_return;
	ret = video_device_pipeline_alloc_start(&sv->vdev);
	if (ret)
		goto err_pm;

	ret = mtk_sv_hw_camsv_start(sv, &sv->pix);
	if (ret)
		goto err_pipeline;

	spin_lock_irqsave(&sv->buf_lock, flags);
	sv->streaming = true;
	mtk_sv_next_buffer(sv);
	spin_unlock_irqrestore(&sv->buf_lock, flags);

	ret = v4l2_subdev_enable_streams(&sv->seninf_sd, SENINF_PAD_SOURCE, BIT(0));
	if (ret)
		goto err_camsv;
	return 0;

err_camsv:
	spin_lock_irqsave(&sv->buf_lock, flags);
	sv->streaming = false;
	spin_unlock_irqrestore(&sv->buf_lock, flags);
	mtk_sv_hw_camsv_stop(sv);
err_pipeline:
	video_device_pipeline_stop(&sv->vdev);
err_pm:
	pm_runtime_put(sv->dev);
err_return:
	mtk_sv_return_buffers(sv, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void mtk_sv_stop_streaming(struct vb2_queue *vq)
{
	struct mtk_sv *sv = vb2_get_drv_priv(vq);
	unsigned long flags;

	v4l2_subdev_disable_streams(&sv->seninf_sd, SENINF_PAD_SOURCE, BIT(0));
	spin_lock_irqsave(&sv->buf_lock, flags);
	sv->streaming = false;
	spin_unlock_irqrestore(&sv->buf_lock, flags);
	mtk_sv_hw_camsv_stop(sv);
	video_device_pipeline_stop(&sv->vdev);
	mtk_sv_return_buffers(sv, VB2_BUF_STATE_ERROR);
	pm_runtime_put(sv->dev);
}

static const struct vb2_ops mtk_sv_vb2_ops = {
	.queue_setup = mtk_sv_queue_setup,
	.buf_init = mtk_sv_buf_init,
	.buf_cleanup = mtk_sv_buf_cleanup,
	.buf_prepare = mtk_sv_buf_prepare,
	.buf_queue = mtk_sv_buf_queue,
	.start_streaming = mtk_sv_start_streaming,
	.stop_streaming = mtk_sv_stop_streaming,
};

/* Frame-done interrupt: complete the active buffer and arm the next one. */
static irqreturn_t mtk_sv_irq(int irq, void *data)
{
	struct mtk_sv *sv = data;
	struct mtk_sv_buffer *done;
	u32 status;

	status = mtk_sv_hw_irq_status(sv);
	if (!(status & MTK_SV_IRQ_FRAME_DONE))
		return status ? IRQ_HANDLED : IRQ_NONE;

	spin_lock(&sv->buf_lock);
	done = sv->active;
	mtk_sv_next_buffer(sv);
	spin_unlock(&sv->buf_lock);

	if (done) {
		done->vb.vb2_buf.timestamp = ktime_get_ns();
		done->vb.sequence = sv->sequence;
		done->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&done->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}
	sv->sequence++;
	return IRQ_HANDLED;
}

static int mtk_sv_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "mtk-seninf", sizeof(cap->driver));
	strscpy(cap->card, "MT6878 CAMSV raw capture", sizeof(cap->card));
	return 0;
}

static int mtk_sv_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	unsigned int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(mtk_sv_formats); i++) {
		if (f->mbus_code && mtk_sv_formats[i].code != f->mbus_code)
			continue;
		if (n++ == f->index) {
			f->pixelformat = mtk_sv_formats[i].fourcc;
			return 0;
		}
	}
	return -EINVAL;
}

static int mtk_sv_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct mtk_sv *sv = video_drvdata(file);

	f->fmt.pix = sv->pix;
	return 0;
}

static int mtk_sv_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	const struct mtk_sv_format *fmt = mtk_sv_find_format(f->fmt.pix.pixelformat, 0);

	if (!fmt)
		fmt = &mtk_sv_formats[0];
	mtk_sv_fill_pix(&f->fmt.pix, fmt, f->fmt.pix.width, f->fmt.pix.height);
	return 0;
}

static int mtk_sv_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct mtk_sv *sv = video_drvdata(file);

	if (vb2_is_busy(&sv->queue))
		return -EBUSY;
	mtk_sv_try_fmt(file, fh, f);
	sv->pix = f->fmt.pix;
	sv->fmt = mtk_sv_find_format(sv->pix.pixelformat, 0);
	return 0;
}

static int mtk_sv_enum_framesizes(struct file *file, void *fh, struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index || !mtk_sv_find_format(fsize->pixel_format, 0))
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = (struct v4l2_frmsize_stepwise) {
		16, MTK_SV_MAX_WIDTH, 2, 16, MTK_SV_MAX_HEIGHT, 2
	};
	return 0;
}

static const struct v4l2_ioctl_ops mtk_sv_ioctl_ops = {
	.vidioc_querycap = mtk_sv_querycap,
	.vidioc_enum_fmt_vid_cap = mtk_sv_enum_fmt,
	.vidioc_g_fmt_vid_cap = mtk_sv_g_fmt,
	.vidioc_try_fmt_vid_cap = mtk_sv_try_fmt,
	.vidioc_s_fmt_vid_cap = mtk_sv_s_fmt,
	.vidioc_enum_framesizes = mtk_sv_enum_framesizes,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations mtk_sv_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
};

static int mtk_sv_link_validate(struct media_link *link)
{
	struct video_device *vdev = media_entity_to_video_device(link->sink->entity);
	struct mtk_sv *sv = video_get_drvdata(vdev);
	struct v4l2_subdev_format sd_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = SENINF_PAD_SOURCE,
	};
	int ret;

	ret = v4l2_subdev_call_state_active(&sv->seninf_sd, pad, get_fmt, &sd_fmt);
	if (ret)
		return ret;
	if (sd_fmt.format.width != sv->pix.width || sd_fmt.format.height != sv->pix.height ||
	    sd_fmt.format.code != sv->fmt->code)
		return -EPIPE;
	return 0;
}

static const struct media_entity_operations mtk_sv_entity_ops = {
	.link_validate = mtk_sv_link_validate,
};

/* async notifier */

static int mtk_sv_notify_bound(struct v4l2_async_notifier *notifier, struct v4l2_subdev *sd,
			       struct v4l2_async_connection *asc)
{
	struct mtk_sv *sv = container_of(notifier, struct mtk_sv, notifier);
	int pad;

	pad = media_entity_get_fwnode_pad(&sd->entity, asc->match.fwnode, MEDIA_PAD_FL_SOURCE);
	if (pad < 0)
		return pad;

	sv->sensor = sd;
	return media_create_pad_link(&sd->entity, pad, &sv->seninf_sd.entity, SENINF_PAD_SINK,
				     MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
}

static int mtk_sv_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct mtk_sv *sv = container_of(notifier, struct mtk_sv, notifier);

	return v4l2_device_register_subdev_nodes(&sv->v4l2_dev);
}

static const struct v4l2_async_notifier_operations mtk_sv_notify_ops = {
	.bound = mtk_sv_notify_bound,
	.complete = mtk_sv_notify_complete,
};

static int mtk_sv_parse_endpoint(struct mtk_sv *sv)
{
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_UNKNOWN };
	struct v4l2_async_connection *asc;
	struct fwnode_handle *fwnode;
	int ret;

	fwnode = fwnode_graph_get_endpoint_by_id(dev_fwnode(sv->dev), 0, 0, 0);
	if (!fwnode)
		return dev_err_probe(sv->dev, -EINVAL, "no sensor endpoint\n");

	ret = v4l2_fwnode_endpoint_parse(fwnode, &ep);
	if (ret) {
		fwnode_handle_put(fwnode);
		return dev_err_probe(sv->dev, ret, "bad endpoint\n");
	}
	sv->cphy = ep.bus_type == V4L2_MBUS_CSI2_CPHY;
	sv->num_lanes = ep.bus.mipi_csi2.num_data_lanes;

	asc = v4l2_async_nf_add_fwnode_remote(&sv->notifier, fwnode, struct v4l2_async_connection);
	fwnode_handle_put(fwnode);
	return PTR_ERR_OR_ZERO(asc);
}

static int mtk_sv_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_sv *sv;
	struct vb2_queue *q;
	int ret, irq;

	sv = devm_kzalloc(dev, sizeof(*sv), GFP_KERNEL);
	if (!sv)
		return -ENOMEM;
	sv->dev = dev;
	platform_set_drvdata(pdev, sv);
	spin_lock_init(&sv->buf_lock);
	INIT_LIST_HEAD(&sv->buf_list);
	mutex_init(&sv->lock);

	/* CAM_MAIN (under CAM_VCORE) and CSI_RX, switched with runtime PM */
	ret = devm_pm_domain_attach_list(dev, NULL, &sv->pd_list);
	if (ret < 0 && ret != -EEXIST)
		return ret;

	ret = mtk_sv_hw_init(sv, pdev);
	if (ret)
		return ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, mtk_sv_irq, 0, dev_name(dev), sv);
	if (ret)
		return ret;

	/*
	 * The DMA API only does cache maintenance for this device; the camsv
	 * sees M4U IOVAs. A full mask keeps swiotlb from bouncing the pages.
	 */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	sv->fmt = &mtk_sv_formats[0];
	mtk_sv_fill_pix(&sv->pix, sv->fmt, 4096, 3072);

	sv->mdev.dev = dev;
	strscpy(sv->mdev.model, "MT6878 seninf + camsv", sizeof(sv->mdev.model));
	media_device_init(&sv->mdev);
	sv->v4l2_dev.mdev = &sv->mdev;

	ret = v4l2_device_register(dev, &sv->v4l2_dev);
	if (ret)
		goto err_mdev;

	v4l2_subdev_init(&sv->seninf_sd, &mtk_seninf_ops);
	sv->seninf_sd.internal_ops = &mtk_seninf_internal_ops;
	sv->seninf_sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sv->seninf_sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sv->seninf_sd.entity.ops = &mtk_seninf_entity_ops;
	sv->seninf_sd.dev = dev;
	sv->seninf_sd.owner = THIS_MODULE;
	sv->seninf_sd.fwnode = dev_fwnode(dev);
	strscpy(sv->seninf_sd.name, "mtk-seninf", sizeof(sv->seninf_sd.name));
	sv->seninf_pads[SENINF_PAD_SINK].flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	sv->seninf_pads[SENINF_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sv->seninf_sd.entity, 2, sv->seninf_pads);
	if (ret)
		goto err_v4l2;
	ret = v4l2_subdev_init_finalize(&sv->seninf_sd);
	if (ret)
		goto err_entity;
	ret = v4l2_device_register_subdev(&sv->v4l2_dev, &sv->seninf_sd);
	if (ret)
		goto err_subdev;

	q = &sv->queue;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = sv;
	q->buf_struct_size = sizeof(struct mtk_sv_buffer);
	q->ops = &mtk_sv_vb2_ops;
	q->mem_ops = &vb2_dma_sg_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 1;
	q->max_num_buffers = MTK_SV_MAX_BUFFERS;
	q->lock = &sv->lock;
	q->dev = dev;
	ret = vb2_queue_init(q);
	if (ret)
		goto err_unreg_subdev;

	strscpy(sv->vdev.name, "mtk-camsv", sizeof(sv->vdev.name));
	sv->vdev.fops = &mtk_sv_fops;
	sv->vdev.ioctl_ops = &mtk_sv_ioctl_ops;
	sv->vdev.v4l2_dev = &sv->v4l2_dev;
	sv->vdev.queue = q;
	sv->vdev.lock = &sv->lock;
	sv->vdev.release = video_device_release_empty;
	sv->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	sv->vdev.entity.ops = &mtk_sv_entity_ops;
	sv->vdev_pad.flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	ret = media_entity_pads_init(&sv->vdev.entity, 1, &sv->vdev_pad);
	if (ret)
		goto err_unreg_subdev;
	video_set_drvdata(&sv->vdev, sv);
	ret = video_register_device(&sv->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_vdev_entity;

	ret = media_create_pad_link(&sv->seninf_sd.entity, SENINF_PAD_SOURCE,
				    &sv->vdev.entity, 0,
				    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		goto err_vdev;

	v4l2_async_nf_init(&sv->notifier, &sv->v4l2_dev);
	sv->notifier.ops = &mtk_sv_notify_ops;
	ret = mtk_sv_parse_endpoint(sv);
	if (ret)
		goto err_nf;
	ret = v4l2_async_nf_register(&sv->notifier);
	if (ret)
		goto err_nf;

	ret = media_device_register(&sv->mdev);
	if (ret)
		goto err_nf_unreg;

	dev_info(dev, "seninf + camsv ready (%s, %u lanes)\n",
		 sv->cphy ? "C-PHY" : "D-PHY", sv->num_lanes);
	return 0;

err_nf_unreg:
	v4l2_async_nf_unregister(&sv->notifier);
err_nf:
	v4l2_async_nf_cleanup(&sv->notifier);
err_vdev:
	video_unregister_device(&sv->vdev);
err_vdev_entity:
	media_entity_cleanup(&sv->vdev.entity);
err_unreg_subdev:
	v4l2_device_unregister_subdev(&sv->seninf_sd);
err_subdev:
	v4l2_subdev_cleanup(&sv->seninf_sd);
err_entity:
	media_entity_cleanup(&sv->seninf_sd.entity);
err_v4l2:
	v4l2_device_unregister(&sv->v4l2_dev);
err_mdev:
	media_device_cleanup(&sv->mdev);
	mtk_sv_hw_exit(sv);
	return ret;
}

static void mtk_sv_remove(struct platform_device *pdev)
{
	struct mtk_sv *sv = platform_get_drvdata(pdev);

	media_device_unregister(&sv->mdev);
	v4l2_async_nf_unregister(&sv->notifier);
	v4l2_async_nf_cleanup(&sv->notifier);
	video_unregister_device(&sv->vdev);
	media_entity_cleanup(&sv->vdev.entity);
	v4l2_device_unregister_subdev(&sv->seninf_sd);
	v4l2_subdev_cleanup(&sv->seninf_sd);
	media_entity_cleanup(&sv->seninf_sd.entity);
	v4l2_device_unregister(&sv->v4l2_dev);
	media_device_cleanup(&sv->mdev);
	mtk_sv_hw_exit(sv);
}

static const struct of_device_id mtk_sv_of_match[] = {
	{ .compatible = "mediatek,mt6878-seninf-camsv" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_sv_of_match);

static struct platform_driver mtk_sv_driver = {
	.probe = mtk_sv_probe,
	.remove = mtk_sv_remove,
	.driver = {
		.name = "mtk-seninf",
		.of_match_table = mtk_sv_of_match,
	},
};
module_platform_driver(mtk_sv_driver);

MODULE_DESCRIPTION("MediaTek MT6878 seninf + CAMSV raw capture");
MODULE_LICENSE("GPL");
