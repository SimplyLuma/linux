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
 */

#include <linux/backlight.h>
#include <linux/bits.h>
#include <linux/delay.h>
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
#define DSI_CMDQ_WORDS		4

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

struct titan2e_kick {
	struct device *dev;
	void __iomem *dsi;
	void __iomem *mutex;
	struct task_struct *thread;
	struct hrtimer delay_timer;	/* te_delay_us after TE */
	struct mutex lock;		/* DCS commands and panel power */
	struct backlight_device *bl;
	struct notifier_block crtc_nb;
	spinlock_t slock;		/* the fields below, vs. the TE interrupt */
	unsigned int hold;		/* framebuffer copies and DCS commands in progress */
	bool pending;			/* a frame waits for the next TE */
	bool delay_armed;
	unsigned int te_count;
	unsigned long pending_since;
	bool panel_on;
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

	if (!(sta & DSI_TE_RDY))
		return IRQ_NONE;
	writel((u32)~DSI_TE_RDY, k->dsi + DSI_INTSTA);	/* write 0 to clear */

	spin_lock(&k->slock);
	k->te_count++;
	if (k->pending && !k->hold && k->panel_on && !(sta & DSI_BUSY) && !k->delay_armed &&
	    k->te_count >= max(READ_ONCE(te_skip), 1U)) {
		unsigned int delay = READ_ONCE(te_delay_us);

		k->te_count = 0;
		if (delay) {
			k->delay_armed = true;
			hrtimer_start(&k->delay_timer, ns_to_ktime((u64)delay * NSEC_PER_USEC),
				      HRTIMER_MODE_REL_HARD);
		} else {
			titan2e_start_frame(k);
		}
	}
	if (!k->pending)
		titan2e_te_enable(k, false);
	spin_unlock(&k->slock);
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
		spin_unlock_irqrestore(&k->slock, flags);
	}
	return 0;
}

/* Send one DCS write; caller holds k->lock. */
static int titan2e_dcs_write(struct titan2e_kick *k, const u8 *data, size_t len)
{
	u32 saved[DSI_CMDQ_WORDS], saved_size, word, val;
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
		u8 type = len == 1 ? MIPI_DSI_DCS_SHORT_WRITE : MIPI_DSI_DCS_SHORT_WRITE_PARAM;

		word = DSI_CFG_HSTX | (type << 8) | (data[0] << 16);
		if (len == 2)
			word |= data[1] << 24;
		writel(word, k->dsi + DSI_CMDQ0);
		words = 1;
	} else {
		writel(DSI_CFG_LONG | DSI_CFG_HSTX | (MIPI_DSI_DCS_LONG_WRITE << 8) | (len << 16),
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
		dev_warn_ratelimited(k->dev, "DCS 0x%02x not acknowledged\n", data[0]);
	return ret;
}

static void titan2e_panel_power(struct titan2e_kick *k, bool on)
{
	u8 cmd;

	mutex_lock(&k->lock);
	if (on == k->panel_on)
		goto out;
	titan2e_hold(k);
	if (on) {
		cmd = MIPI_DCS_EXIT_SLEEP_MODE;
		titan2e_dcs_write(k, &cmd, 1);
		msleep(120);
		cmd = MIPI_DCS_SET_DISPLAY_ON;
		titan2e_dcs_write(k, &cmd, 1);
	} else {
		cmd = MIPI_DCS_SET_DISPLAY_OFF;
		titan2e_dcs_write(k, &cmd, 1);
		cmd = MIPI_DCS_ENTER_SLEEP_MODE;
		titan2e_dcs_write(k, &cmd, 1);
		msleep(20);
	}
	spin_lock_irq(&k->slock);
	k->panel_on = on;
	spin_unlock_irq(&k->slock);
	titan2e_release(k);
	if (on)
		titan2e_queue_frame(k);
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
	platform_set_drvdata(pdev, k);
	dev_info(&pdev->dev, "command-mode panel: TE-synced refresh\n");
	return 0;
}

static void titan2e_kick_remove(struct platform_device *pdev)
{
	struct titan2e_kick *k = platform_get_drvdata(pdev);

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
