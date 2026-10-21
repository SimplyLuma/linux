// SPDX-License-Identifier: GPL-2.0
/*
 * Unihertz Titan 2 Elite bring-up: command-mode panel support on top of the
 * bootloader-configured display path.
 *
 * The bootloader leaves OVL0 -> DSC -> DSI0 configured and scanning out the
 * simple-framebuffer. A command-mode panel only refreshes when the display
 * mutex and DSI engine are started, so do that at a fixed rate. The DSI
 * command queue holds the bootloader's frame command (write_memory_start);
 * DCS commands for power and brightness are sent by swapping it out briefly.
 *
 * simpledrm reports output on/off so the panel sleeps when the screen is
 * blanked, and reports each frame copy: the copy waits for the panel transfer
 * in flight, and the new frame is sent on the panel's next tearing-effect
 * (TE) pulse. The panel keeps its DSC-compressed frame in memory and decodes
 * it while refreshing, so a transfer that crosses the refresh line corrupts
 * one slice row; starting in the TE interrupt keeps the write ahead of the
 * refresh. A slow timer refresh remains as a fallback, and frames are sent
 * directly if TE stops. A backlight device drives the panel brightness.
 *
 * A panel that misses a wake from sleep stays black and sends no TE; it is
 * then reset and initialised again with the stock VTDR6130A sequence.
 *
 * The driver is also simpledrm's vblank source: on every TE slot that may
 * start a frame (every te_skip-th TE, 60 Hz) it reports a vblank, so page
 * flips complete at the panel's refresh, in step with the transfers, instead
 * of as soon as the copy is done.
 */

#include <linux/backlight.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_data/simplefb.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <drm/drm_vblank.h>
#include <video/mipi_display.h>

#define DSI_START		0x00
#define DSI_INTEN		0x08
#define DSI_INTSTA		0x0c
#define DSI_CMD_DONE		BIT(1)
#define DSI_TE_RDY		BIT(2)
#define DSI_BUSY		BIT(31)
#define DSI_CMDQ_SIZE		0x60
#define DSI_CMDQ_SIZE_SEL	BIT(15)
#define DSI_CMDQ0		0xd00
#define DSI_CMDQ_WORDS		32	/* 124-byte payloads */

#define DSI_CFG_LONG		0x02
#define DSI_CFG_HSTX		0x08

#define DISP_MUTEX0_EN		0x20

#define OVL_SRC_CON		0x2c
#define OVL_SRC_CON_L0		BIT(0)

#define KICK_FALLBACK_MS	1000
#define TE_TIMEOUT_MS		100

#define PANEL_MAX_BRIGHTNESS	4095
#define PANEL_DEF_BRIGHTNESS	2047

/*
 * The panel reports TE at 120 Hz, but frames sent on every TE (7.0 ms of each
 * 8.3 ms period) leave corrupted slice rows during motion; every second TE
 * (60 Hz) is clean. te_delay_us delays the start after TE, for timing tests.
 */
static unsigned int te_delay_us;
module_param(te_delay_us, uint, 0644);
static unsigned int te_skip = 2;
module_param(te_skip, uint, 0644);
/* Test the recovery: the next wake resets and re-initialises the panel. */
static bool reset_on_wake;
module_param(reset_on_wake, bool, 0644);

struct titan2e_kick {
	struct device *dev;
	void __iomem *dsi;
	void __iomem *mutex;
	struct task_struct *thread;
	struct hrtimer delay_timer;	/* te_delay_us after TE */
	struct mutex lock;		/* DCS commands and panel power */
	struct backlight_device *bl;
	struct gpio_desc *reset;
	struct notifier_block crtc_nb;
	struct notifier_block vblank_nb;
	spinlock_t slock;		/* the fields below, vs. the TE interrupt */
	unsigned int hold;		/* framebuffer copies and DCS commands in progress */
	bool pending;			/* a frame waits for the next TE */
	bool delay_armed;
	unsigned int te_phase;		/* TEs since the last frame slot */
	unsigned long pending_since;
	unsigned long last_te;
	bool panel_on;
	struct drm_crtc *vblank_crtc;	/* set while simpledrm wants vblank interrupts */
};

static int titan2e_wait_idle(struct titan2e_kick *k)
{
	u32 val;

	return readl_poll_timeout(k->dsi + DSI_INTSTA, val, !(val & DSI_BUSY), 50, 50000);
}

/* Start a frame transfer; caller holds k->slock. */
static void titan2e_start_frame(struct titan2e_kick *k)
{
	writel(1, k->mutex + DISP_MUTEX0_EN);
	writel(0, k->dsi + DSI_START);
	writel(1, k->dsi + DSI_START);
	k->pending = false;
}

static void titan2e_te_enable(struct titan2e_kick *k, bool on)
{
	u32 val = readl(k->dsi + DSI_INTEN);

	writel(on ? val | DSI_TE_RDY : val & ~DSI_TE_RDY, k->dsi + DSI_INTEN);
}

/* Queue a frame for the next TE pulse. */
static void titan2e_queue_frame(struct titan2e_kick *k)
{
	unsigned long flags;

	spin_lock_irqsave(&k->slock, flags);
	if (!k->pending) {
		k->pending = true;
		k->pending_since = jiffies;
	}
	titan2e_te_enable(k, true);
	spin_unlock_irqrestore(&k->slock, flags);
}

/* Keep new transfers from starting; returns once the one in flight is done. */
static void titan2e_hold(struct titan2e_kick *k)
{
	unsigned long flags;

	spin_lock_irqsave(&k->slock, flags);
	k->hold++;
	spin_unlock_irqrestore(&k->slock, flags);
	titan2e_wait_idle(k);
}

static void titan2e_release(struct titan2e_kick *k)
{
	unsigned long flags;

	spin_lock_irqsave(&k->slock, flags);
	k->hold--;
	spin_unlock_irqrestore(&k->slock, flags);
}

static irqreturn_t titan2e_te_irq(int irq, void *data)
{
	struct titan2e_kick *k = data;
	u32 sta = readl(k->dsi + DSI_INTSTA);
	struct drm_crtc *vblank = NULL;

	if (!(sta & DSI_TE_RDY))
		return IRQ_NONE;
	writel((u32)~DSI_TE_RDY, k->dsi + DSI_INTSTA);	/* write 0 to clear */

	spin_lock(&k->slock);
	k->last_te = jiffies;
	/* Frames start, and vblanks are reported, only on every te_skip-th TE. */
	if (++k->te_phase >= max(READ_ONCE(te_skip), 1U)) {
		k->te_phase = 0;
		if (k->pending && !k->hold && k->panel_on && !(sta & DSI_BUSY) && !k->delay_armed) {
			unsigned int delay = READ_ONCE(te_delay_us);

			if (delay) {
				k->delay_armed = true;
				hrtimer_start(&k->delay_timer, ns_to_ktime((u64)delay * NSEC_PER_USEC),
					      HRTIMER_MODE_REL_HARD);
			} else {
				titan2e_start_frame(k);
			}
		}
		vblank = k->vblank_crtc;
	}
	if (!k->pending && !k->vblank_crtc)
		titan2e_te_enable(k, false);
	spin_unlock(&k->slock);
	/* After the start: a flip committed before this TE completes with its transfer. */
	if (vblank)
		drm_crtc_handle_vblank(vblank);
	return IRQ_HANDLED;
}

static enum hrtimer_restart titan2e_delay_timer(struct hrtimer *t)
{
	struct titan2e_kick *k = container_of(t, struct titan2e_kick, delay_timer);
	unsigned long flags;

	spin_lock_irqsave(&k->slock, flags);
	k->delay_armed = false;
	if (k->pending && !k->hold && k->panel_on && !(readl(k->dsi + DSI_INTSTA) & DSI_BUSY))
		titan2e_start_frame(k);
	if (!k->pending)
		titan2e_te_enable(k, false);
	spin_unlock_irqrestore(&k->slock, flags);
	return HRTIMER_NORESTART;
}

static int titan2e_kick_thread(void *data)
{
	struct titan2e_kick *k = data;
	unsigned long flags, last = jiffies;
	struct drm_crtc *vblank;

	while (!kthread_should_stop()) {
		msleep_interruptible(TE_TIMEOUT_MS);
		if (time_after(jiffies, last + msecs_to_jiffies(KICK_FALLBACK_MS))) {
			titan2e_queue_frame(k);
			last = jiffies;
		}
		/* No TE (panel not reporting it): send the frame anyway. */
		spin_lock_irqsave(&k->slock, flags);
		if (k->pending && !k->hold && k->panel_on &&
		    time_after(jiffies, k->pending_since + msecs_to_jiffies(TE_TIMEOUT_MS)) &&
		    !(readl(k->dsi + DSI_INTSTA) & DSI_BUSY))
			titan2e_start_frame(k);
		vblank = k->vblank_crtc;
		if (vblank && !time_after(jiffies, k->last_te + msecs_to_jiffies(TE_TIMEOUT_MS)))
			vblank = NULL;
		spin_unlock_irqrestore(&k->slock, flags);
		/* ...and keep page flips completing, slowly, so user space does not stall. */
		if (vblank)
			drm_crtc_handle_vblank(vblank);
	}
	return 0;
}

/*
 * Send one DCS or generic write, in high speed or (lp) low power; caller
 * holds k->lock. The stock panel driver sends everything in low power, and a
 * panel initialised that way takes no high-speed commands.
 */
static int titan2e_dsi_write(struct titan2e_kick *k, const u8 *data, size_t len, bool generic,
			     bool lp)
{
	u32 hs = lp ? 0 : DSI_CFG_HSTX, saved[DSI_CMDQ_WORDS], saved_size, word, val;
	unsigned int i, words;
	int ret;

	if (!len || len > 4 * (DSI_CMDQ_WORDS - 1))
		return -EINVAL;

	ret = titan2e_wait_idle(k);
	if (ret)
		return ret;

	for (i = 0; i < DSI_CMDQ_WORDS; i++)
		saved[i] = readl(k->dsi + DSI_CMDQ0 + 4 * i);
	saved_size = readl(k->dsi + DSI_CMDQ_SIZE);

	if (len <= 2) {
		u8 type;

		if (generic)
			type = len == 1 ? MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM :
					  MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM;
		else
			type = len == 1 ? MIPI_DSI_DCS_SHORT_WRITE : MIPI_DSI_DCS_SHORT_WRITE_PARAM;

		word = hs | (type << 8) | (data[0] << 16);
		if (len == 2)
			word |= data[1] << 24;
		writel(word, k->dsi + DSI_CMDQ0);
		words = 1;
	} else {
		u8 type = generic ? MIPI_DSI_GENERIC_LONG_WRITE : MIPI_DSI_DCS_LONG_WRITE;

		writel(DSI_CFG_LONG | hs | (type << 8) | (len << 16),
		       k->dsi + DSI_CMDQ0);
		words = 1 + DIV_ROUND_UP(len, 4);
		for (i = 1; i < words; i++) {
			word = 0;
			for (unsigned int b = 0; b < 4 && 4 * (i - 1) + b < len; b++)
				word |= data[4 * (i - 1) + b] << (8 * b);
			writel(word, k->dsi + DSI_CMDQ0 + 4 * i);
		}
	}
	writel(DSI_CMDQ_SIZE_SEL | words, k->dsi + DSI_CMDQ_SIZE);

	writel(readl(k->dsi + DSI_INTSTA) & ~DSI_CMD_DONE, k->dsi + DSI_INTSTA);
	writel(0, k->dsi + DSI_START);
	writel(1, k->dsi + DSI_START);
	ret = readl_poll_timeout(k->dsi + DSI_INTSTA, val, val & DSI_CMD_DONE, 50, 20000);
	writel(readl(k->dsi + DSI_INTSTA) & ~DSI_CMD_DONE, k->dsi + DSI_INTSTA);

	for (i = 0; i < DSI_CMDQ_WORDS; i++)
		writel(saved[i], k->dsi + DSI_CMDQ0 + 4 * i);
	writel(saved_size, k->dsi + DSI_CMDQ_SIZE);

	if (ret)
		dev_warn_ratelimited(k->dev, "DSI write 0x%02x not acknowledged\n", data[0]);
	return ret;
}

static int titan2e_dcs_write(struct titan2e_kick *k, const u8 *data, size_t len)
{
	return titan2e_dsi_write(k, data, len, false, true);
}

struct titan2e_cmd {
	u8 type;
	u8 len;
	u16 delay_ms;
	const u8 *data;
};

enum { CMD_DCS, CMD_GENERIC, CMD_DELAY };

#define DCS(...)	{ CMD_DCS, sizeof((const u8[]){ __VA_ARGS__ }), 0, (const u8[]){ __VA_ARGS__ } }
#define GEN(...)	{ CMD_GENERIC, sizeof((const u8[]){ __VA_ARGS__ }), 0, (const u8[]){ __VA_ARGS__ } }
#define DELAY(ms)	{ CMD_DELAY, 0, ms, NULL }

/* VTDR6130A initialisation after reset, as the stock panel driver sends it. */
static const struct titan2e_cmd titan2e_panel_init[] = {
	DCS(0x03, 0x01), DCS(0x35, 0x00), DCS(0x53, 0x20), DCS(0x55, 0x10),
	DCS(0x5e, 0x00), DCS(0x59, 0x09), DCS(0x6c, 0x00), DCS(0x6d, 0x00),
	DCS(0x6f, 0x02),
	DCS(0x70, 0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x04, 0xb0, 0x04, 0x38, 0x00, 0x14,
	    0x02, 0x1c, 0x02, 0x1c, 0x02, 0x00, 0x02, 0x25, 0x00, 0x20, 0x01, 0xd5, 0x00,
	    0x07, 0x00, 0x0d, 0x05, 0x7a, 0x05, 0x16, 0x18, 0x00, 0x10, 0xf0, 0x03, 0x0c,
	    0x20, 0x00, 0x06, 0x0b, 0x0b, 0x33, 0x0e, 0x1c, 0x2a, 0x38, 0x46, 0x54, 0x62,
	    0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x01, 0x02, 0x01, 0x00, 0x09, 0x40,
	    0x09, 0xbe, 0x19, 0xfc, 0x19, 0xfa, 0x19, 0xf8, 0x1a, 0x38, 0x1a, 0x78, 0x1a,
	    0xb6, 0x2a, 0xb6, 0x2a, 0xf4, 0x2a, 0xf4, 0x4b, 0x34, 0x63, 0x74, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00),
	DCS(0x72, 0x00),
	GEN(0xff, 0x5a, 0x80), DCS(0x65, 0x0a), GEN(0xf9, 0xa8),
	GEN(0xff, 0x5a, 0x80), DCS(0x65, 0x0e), GEN(0xf9, 0xa1),
	GEN(0xff, 0x5a, 0x81), DCS(0x65, 0x06), GEN(0xf3, 0x87),
	DCS(0x65, 0x09), GEN(0xf3, 0x00), DCS(0x65, 0x0a), GEN(0xf3, 0x10),
	DCS(0x65, 0x10), GEN(0xf3, 0x10), GEN(0xf8, 0x01),
	DCS(0x65, 0x03), GEN(0xfb, 0x93, 0x93),
	GEN(0xff, 0x5a, 0x81), DCS(0x65, 0x03), GEN(0xf3, 0x14),
	DCS(0x65, 0x05), GEN(0xf3, 0xa0),
	GEN(0xff, 0x5a, 0x00), GEN(0xf0, 0xaa, 0x00),
	DCS(MIPI_DCS_SET_DISPLAY_OFF), DELAY(50),
	DCS(MIPI_DCS_ENTER_SLEEP_MODE), DCS(MIPI_DCS_EXIT_SLEEP_MODE), DELAY(120),
	DCS(MIPI_DCS_SET_DISPLAY_ON), DELAY(50),
};

/* Reset the panel and send the stock initialisation; caller holds k->lock. */
static void titan2e_panel_reinit(struct titan2e_kick *k)
{
	unsigned int i, level = backlight_get_brightness(k->bl);
	u8 bl[3] = { MIPI_DCS_SET_DISPLAY_BRIGHTNESS, level >> 8, level & 0xff };
	const struct titan2e_cmd *c;

	titan2e_hold(k);
	gpiod_set_value_cansleep(k->reset, 0);
	msleep(30);
	gpiod_set_value_cansleep(k->reset, 1);
	msleep(30);
	for (i = 0; i < ARRAY_SIZE(titan2e_panel_init); i++) {
		c = &titan2e_panel_init[i];
		if (c->type == CMD_DELAY)
			msleep(c->delay_ms);
		else
			titan2e_dsi_write(k, c->data, c->len, c->type == CMD_GENERIC, true);
	}
	titan2e_dsi_write(k, bl, sizeof(bl), false, true);
	titan2e_release(k);
}

/* Wait for a TE pulse after @since; the frame queued on wake enables TE. */
static bool titan2e_wait_te(struct titan2e_kick *k, unsigned long since)
{
	unsigned long end = jiffies + msecs_to_jiffies(TE_TIMEOUT_MS);

	do {
		if (time_after(READ_ONCE(k->last_te), since))
			return true;
		usleep_range(5000, 10000);
	} while (time_before(jiffies, end));
	return false;
}

static void titan2e_panel_power(struct titan2e_kick *k, bool on)
{
	unsigned int tries = 0;
	unsigned long since;
	u8 cmd;

	mutex_lock(&k->lock);
	if (on == k->panel_on)
		goto out;
	if (!on) {
		titan2e_hold(k);
		cmd = MIPI_DCS_SET_DISPLAY_OFF;
		titan2e_dcs_write(k, &cmd, 1);
		cmd = MIPI_DCS_ENTER_SLEEP_MODE;
		titan2e_dcs_write(k, &cmd, 1);
		msleep(20);
		spin_lock_irq(&k->slock);
		k->panel_on = false;
		spin_unlock_irq(&k->slock);
		titan2e_release(k);
		goto out;
	}

	since = jiffies;
	titan2e_hold(k);
	cmd = MIPI_DCS_EXIT_SLEEP_MODE;
	titan2e_dcs_write(k, &cmd, 1);
	msleep(120);
	cmd = MIPI_DCS_SET_DISPLAY_ON;
	titan2e_dcs_write(k, &cmd, 1);
	spin_lock_irq(&k->slock);
	k->panel_on = true;
	spin_unlock_irq(&k->slock);
	titan2e_release(k);
	titan2e_queue_frame(k);

	/* A panel that misses the wake stays black and sends no TE: reset it. */
	if (k->reset && READ_ONCE(reset_on_wake)) {
		WRITE_ONCE(reset_on_wake, false);
		dev_info(k->dev, "resetting the panel on request\n");
		since = jiffies;
		titan2e_panel_reinit(k);
		titan2e_queue_frame(k);
	}
	while (!titan2e_wait_te(k, since)) {
		if (!k->reset || tries++ == 2) {
			dev_err(k->dev, "no TE after panel wake\n");
			break;
		}
		dev_warn(k->dev, "no TE after panel wake, resetting the panel (%u)\n", tries);
		since = jiffies;
		titan2e_panel_reinit(k);
		titan2e_queue_frame(k);
	}
out:
	mutex_unlock(&k->lock);
}

static int titan2e_crtc_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct titan2e_kick *k = container_of(nb, struct titan2e_kick, crtc_nb);

	switch (event) {
	case SIMPLEDRM_OUTPUT_OFF:
	case SIMPLEDRM_OUTPUT_ON:
		titan2e_panel_power(k, event == SIMPLEDRM_OUTPUT_ON);
		break;
	case SIMPLEDRM_FRAME_BEGIN:
		/* Don't overwrite the framebuffer while the panel is reading it. */
		titan2e_hold(k);
		break;
	case SIMPLEDRM_FRAME_DONE:
		titan2e_release(k);
		titan2e_queue_frame(k);
		break;
	}
	return NOTIFY_OK;
}

/* simpledrm turns vblank interrupts on and off; atomic context. */
static int titan2e_vblank_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct titan2e_kick *k = container_of(nb, struct titan2e_kick, vblank_nb);
	unsigned long flags;

	spin_lock_irqsave(&k->slock, flags);
	if (event == SIMPLEDRM_VBLANK_ON) {
		k->vblank_crtc = data;
		k->last_te = jiffies;
		titan2e_te_enable(k, true);
	} else if (event == SIMPLEDRM_VBLANK_OFF) {
		k->vblank_crtc = NULL;
		if (!k->pending)
			titan2e_te_enable(k, false);
	}
	spin_unlock_irqrestore(&k->slock, flags);
	return NOTIFY_OK;
}

static int titan2e_bl_update(struct backlight_device *bl)
{
	struct titan2e_kick *k = bl_get_data(bl);
	unsigned int level = backlight_get_brightness(bl);
	u8 cmd[3] = { MIPI_DCS_SET_DISPLAY_BRIGHTNESS, level >> 8, level & 0xff };
	int ret;

	mutex_lock(&k->lock);
	titan2e_hold(k);
	ret = titan2e_dcs_write(k, cmd, sizeof(cmd));
	titan2e_release(k);
	mutex_unlock(&k->lock);
	return ret;
}

static const struct backlight_ops titan2e_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = titan2e_bl_update,
};

static int titan2e_kick_probe(struct platform_device *pdev)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_PLATFORM,
		.max_brightness = PANEL_MAX_BRIGHTNESS,
		.brightness = PANEL_DEF_BRIGHTNESS,
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};
	struct titan2e_kick *k;
	int ret;

	k = devm_kzalloc(&pdev->dev, sizeof(*k), GFP_KERNEL);
	if (!k)
		return -ENOMEM;
	k->dev = &pdev->dev;
	k->panel_on = true;	/* the bootloader leaves the panel running */
	mutex_init(&k->lock);
	spin_lock_init(&k->slock);

	k->dsi = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(k->dsi))
		return PTR_ERR(k->dsi);
	k->mutex = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(k->mutex))
		return PTR_ERR(k->mutex);

	/* Only OVL0 layer 0 (the simple-framebuffer) belongs to Linux; the
	 * bootloader leaves its verified-boot warning overlay on layer 1. */
	if (platform_get_resource(pdev, IORESOURCE_MEM, 2)) {
		void __iomem *ovl = devm_platform_ioremap_resource(pdev, 2);

		if (IS_ERR(ovl))
			return PTR_ERR(ovl);
		writel(readl(ovl + OVL_SRC_CON) & OVL_SRC_CON_L0, ovl + OVL_SRC_CON);
	}
	/* As is: the bootloader leaves the panel running. */
	k->reset = devm_gpiod_get_optional(&pdev->dev, "reset", GPIOD_ASIS);
	if (IS_ERR(k->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(k->reset), "cannot get the panel reset\n");

	hrtimer_setup(&k->delay_timer, titan2e_delay_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);

	writel(0, k->dsi + DSI_INTEN);
	writel((u32)~DSI_TE_RDY, k->dsi + DSI_INTSTA);
	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	ret = devm_request_irq(&pdev->dev, ret, titan2e_te_irq, 0, "titan2e-dsi-te", k);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "cannot request TE interrupt\n");

	k->bl = devm_backlight_device_register(&pdev->dev, "panel", &pdev->dev, k,
					       &titan2e_bl_ops, &props);
	if (IS_ERR(k->bl))
		return dev_err_probe(&pdev->dev, PTR_ERR(k->bl), "cannot register backlight\n");

	k->crtc_nb.notifier_call = titan2e_crtc_event;
	ret = simpledrm_register_crtc_notifier(&k->crtc_nb);
	if (ret)
		return ret;

	k->thread = kthread_run(titan2e_kick_thread, k, "titan2e-dsi-kick");
	if (IS_ERR(k->thread)) {
		simpledrm_unregister_crtc_notifier(&k->crtc_nb);
		return PTR_ERR(k->thread);
	}
	k->vblank_nb.notifier_call = titan2e_vblank_event;
	ret = simpledrm_register_vblank_source(&k->vblank_nb);
	if (ret) {
		kthread_stop(k->thread);
		simpledrm_unregister_crtc_notifier(&k->crtc_nb);
		return ret;
	}
	platform_set_drvdata(pdev, k);
	dev_info(&pdev->dev, "command-mode panel: TE-synced refresh\n");
	return 0;
}

static void titan2e_kick_remove(struct platform_device *pdev)
{
	struct titan2e_kick *k = platform_get_drvdata(pdev);

	simpledrm_unregister_vblank_source(&k->vblank_nb);
	simpledrm_unregister_crtc_notifier(&k->crtc_nb);
	kthread_stop(k->thread);
	writel(0, k->dsi + DSI_INTEN);
	hrtimer_cancel(&k->delay_timer);
}

static const struct of_device_id titan2e_kick_of_match[] = {
	{ .compatible = "unihertz,titan2e-dsi-kick" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, titan2e_kick_of_match);

static struct platform_driver titan2e_kick_driver = {
	.driver = {
		.name = "titan2e-dsi-kick",
		.of_match_table = titan2e_kick_of_match,
	},
	.probe = titan2e_kick_probe,
	.remove = titan2e_kick_remove,
};
module_platform_driver(titan2e_kick_driver);

MODULE_DESCRIPTION("Titan 2 Elite command-mode panel support (bring-up)");
MODULE_LICENSE("GPL");
