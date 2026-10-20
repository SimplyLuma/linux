// SPDX-License-Identifier: GPL-2.0
/*
 * MT6878 seninf (CSI PHY 3.1) and CAMSV register programming.
 *
 * Route: CSI port N (PHY A+B as one 4D1C port: 4-lane D-PHY or 3-trio
 * C-PHY) -> SENINF_(2N+1) -> seninf mux 0 (MUX1) -> cammux 10 -> CAMSV2
 * (camsv_b) tag 2 IMGO, as stock uses it for the rear sensor. Register
 * sequences from the isp7sp seninf driver (csi phy 3.1) and libccd
 * camsys_sv_compose; FIFO thresholds and the PHY efuse words read back on
 * stock.
 *
 * IMGO runs in UNPACK_MODE, which stores the 8 MSBs of each RAW10 pixel.
 * Addresses are M4U IOVAs (larb13 ports run with MMU_EN=1).
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include "mtk-seninf-sv.h"

/* csi-port-N efuse words (rg_csi), stock dmesg */
static const u32 csi_efuse[MTK_SV_NUM_PORTS] = { 0x7b9ee70c, 0x7b9ce68b, 0x7bdce70e };

#define CPHY_SETTLE		23		/* ceil(312 MHz * 73 ns) */
#define DPHY_SETTLE		26		/* ceil(312 MHz * 100 ns) - 6 */
#define PIX_MODE		3		/* 8 pixels per cycle, as on stock */
#define CAMMUX			10		/* camsv2: cammux base 8 + tag 2 */
#define TAG			2
#define MUX_VR			0

/* SPM power control, checked only when no power domain is attached */
#define SPM_BASE		0x1c001000
#define PWR_CON_CAM_MAIN	0xe48
#define PWR_CON_CSI_RX		0xe9c
#define PWR_ACKS		(BIT(30) | BIT(31))

/* camsys_main clock gates (no clock driver yet): CG0 0x00/0x04/0x08, CG1 0x4c/0x50/0x54 */
#define CAM_CG0_CLR		0x08
#define CAM_CG1_CLR		0x54
/* larb13, larb14, larb29, cam, cam_mraw, camtg, seninf, camsv_top, cam2mm0/1 gals */
#define CAM_CG0_BITS		(BIT(0) | BIT(1) | BIT(3) | BIT(4) | BIT(8) | BIT(9) | \
				 BIT(10) | BIT(11) | BIT(16) | BIT(17))
#define CAM_CG1_BITS		(BIT(0) | BIT(1))	/* camsv_a, camsv_b */
#define CAM_MAIN_SW_RST_1	0x58

/* camsv central (outer page; inner page at +0x8000) */
#define SV_INNER		0x8000
#define SV_TAG(o)		((o) + 0x40 * TAG)
#define SV_DONE_STATUS		0x348
#define SV_DONE_FRAME		(BIT(0) | BIT(16))
#define SV_FBC0_EN		BIT(15)
#define SV_FBC0_RCNT_INC	BIT(7)

enum {
	R_SENINF,	/* 0x1a00e000: top, SENINF_n (+0x1000 * (n - 1)), MUX1, cammux */
	R_CSI,		/* 0x11c80000: per port (+0x8000 * N) PHY A/B, D-PHY, C-PHY, MAC top/A/B */
	R_CAMSYS,	/* 0x1a000000 */
	R_SV,		/* 0x1a101000, outer + inner */
	R_SVDMA,	/* 0x1a111000, outer + inner */
	R_SVCQ,		/* 0x1a121000 */
	R_MRAW,		/* 0x1a170000 camsys_mraw gates */
	R_VCORE,	/* 0x1b204000 cam_vcore gates */
	R_NR
};

static const char * const reg_names[R_NR] = {
	"seninf", "csi", "camsys", "camsv", "camsv-dma", "camsv-cq", "camsys-mraw", "cam-vcore",
};

/* offsets of the sub-blocks inside the "seninf" and "csi" regions */
#define SENTOP			0x0000
#define MUX1			0x0000	/* +0xd00 inside top */
#define CAMMUXB			0x17000
/* CSI port N feeds SENINF_(2N+1); MUX1_SRC_SEL takes the 0-based index */
#define SEN			(0x2000 * hw->port)
#define SENIDX			(2 * hw->port)
#define PHYA			(0x8000 * hw->port)
#define PHYB			(PHYA + 0x1000)
#define DPHY			(PHYA + 0x2000)
#define CPHY			(PHYA + 0x3000)
#define MACTOP			(PHYA + 0x4000)
#define MACA			(PHYA + 0x5000)
#define MACB			(PHYA + 0x6000)

struct mtk_sv_hw {
	void __iomem *r[R_NR];
	void __iomem *spm;
	unsigned int port;	/* CSI port of the running stream */
};

static void wr(struct mtk_sv_hw *hw, int r, u32 o, u32 v)
{
	writel(v, hw->r[r] + o);
}

static u32 rd(struct mtk_sv_hw *hw, int r, u32 o)
{
	return readl(hw->r[r] + o);
}

/* read-modify-write: mask is the field in place, v the field value */
static void fld(struct mtk_sv_hw *hw, int r, u32 o, u32 mask, u32 v)
{
	u32 x = readl(hw->r[r] + o);

	x &= ~mask;
	x |= (v << __ffs(mask)) & mask;
	writel(x, hw->r[r] + o);
}

int mtk_sv_hw_init(struct mtk_sv *sv, struct platform_device *pdev)
{
	struct mtk_sv_hw *hw;
	int i;

	hw = devm_kzalloc(&pdev->dev, sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return -ENOMEM;
	for (i = 0; i < R_NR; i++) {
		hw->r[i] = devm_platform_ioremap_resource_byname(pdev, reg_names[i]);
		if (IS_ERR(hw->r[i]))
			return PTR_ERR(hw->r[i]);
	}
	if (!sv->pd_list && !pdev->dev.pm_domain) {
		hw->spm = devm_ioremap(&pdev->dev, SPM_BASE, SZ_4K);
		if (!hw->spm)
			return -ENOMEM;
	}
	sv->hw = hw;
	return 0;
}

void mtk_sv_hw_exit(struct mtk_sv *sv)
{
}

/* Without a power domain the camera domains must have been switched on elsewhere. */
static bool powered(struct mtk_sv_hw *hw)
{
	if (!hw->spm)
		return true;
	return (readl(hw->spm + PWR_CON_CAM_MAIN) & PWR_ACKS) == PWR_ACKS &&
	       (readl(hw->spm + PWR_CON_CSI_RX) & PWR_ACKS) == PWR_ACKS;
}

static void gates_on(struct mtk_sv_hw *hw)
{
	wr(hw, R_CAMSYS, CAM_CG0_CLR, CAM_CG0_BITS);
	wr(hw, R_CAMSYS, CAM_CG1_CLR, CAM_CG1_BITS);
	wr(hw, R_MRAW, 0x08, 0x3);
	wr(hw, R_VCORE, 0x2c, rd(hw, R_VCORE, 0x2c) | 0x3);
}

/* analog PHY init with efuse trim, port A or B */
static void phya_init(struct mtk_sv_hw *hw, u32 p, bool is_a)
{
	u32 e = csi_efuse[hw->port], t2, t3, t4;
	const int C = R_CSI;

	fld(hw, C, p + 0x04, GENMASK(2, 0), 4);
	fld(hw, C, p + 0x04, GENMASK(6, 4), 4);
	fld(hw, C, p + 0x04, GENMASK(11, 8), 8);
	fld(hw, C, p + 0x04, GENMASK(18, 16), 2);
	fld(hw, C, p + 0x08, GENMASK(26, 24), 4);
	fld(hw, C, p + 0x08, GENMASK(30, 28), 4);
	fld(hw, C, p + 0x08, GENMASK(4, 0), 0x10);
	fld(hw, C, p + 0x08, GENMASK(12, 8), 0x10);
	fld(hw, C, p + 0x08, BIT(16) | BIT(17), 0);
	fld(hw, C, p + 0x0c, GENMASK(4, 0), 0x10);
	fld(hw, C, p + 0x0c, GENMASK(12, 8), 0x10);
	fld(hw, C, p + 0x10, GENMASK(4, 0), 0x10);
	fld(hw, C, p + 0x10, GENMASK(12, 8), 0x10);
	fld(hw, C, p + 0x14, GENMASK(1, 0), 1);
	fld(hw, C, p + 0x14, GENMASK(3, 2), 1);
	fld(hw, C, p + 0x14, BIT(4), 0);
	fld(hw, C, p + 0x14, GENMASK(11, 8), 0);
	fld(hw, C, p + 0x14, BIT(30), 1);
	fld(hw, C, p + 0x00, BIT(21) | BIT(22), 0);
	fld(hw, C, p + 0x18, GENMASK(28, 24), 4);
	fld(hw, C, p + 0x18, GENMASK(4, 0), 0xa);
	fld(hw, C, p + 0x18, GENMASK(12, 8), 0xa);
	fld(hw, C, p + 0x18, GENMASK(20, 16), 0xa);
	fld(hw, C, p + 0x1c, GENMASK(28, 24), 4);
	fld(hw, C, p + 0x1c, GENMASK(4, 0), 0xa);
	fld(hw, C, p + 0x1c, GENMASK(12, 8), 0xa);
	fld(hw, C, p + 0x1c, GENMASK(20, 16), 0xa);
	fld(hw, C, p + 0x38, BIT(24), 1);
	fld(hw, C, p + 0x20, GENMASK(15, 0), 0x0100);

	if (is_a) {
		t2 = (e >> 27) & 0x1f;
		t3 = (e >> 22) & 0x1f;
		t4 = (e >> 17) & 0x1f;
	} else {
		t2 = (e >> 12) & 0x1f;
		t3 = (e >> 7) & 0x1f;
		t4 = (e >> 2) & 0x1f;
	}
	fld(hw, C, p + 0x08, GENMASK(4, 0), t2);
	fld(hw, C, p + 0x08, GENMASK(12, 8), t2);
	fld(hw, C, p + 0x0c, GENMASK(4, 0), t3);
	fld(hw, C, p + 0x0c, GENMASK(12, 8), t3);
	fld(hw, C, p + 0x10, GENMASK(4, 0), t4);
	fld(hw, C, p + 0x10, GENMASK(12, 8), t4);
}

/* analog PHY power on */
static void phya_power_on(struct mtk_sv_hw *hw, u32 p)
{
	const int C = R_CSI;
	int i;

	fld(hw, C, p + 0x20, GENMASK(21, 16), 0);
	fld(hw, C, p + 0x00, BIT(1) | BIT(0), 0);
	udelay(200);
	fld(hw, C, p + 0x00, BIT(0), 1);
	udelay(30);
	fld(hw, C, p + 0x00, BIT(1), 1);
	udelay(1);
	for (i = 16; i <= 21; i++)
		fld(hw, C, p + 0x20, BIT(i), 1);
	udelay(1);
}

/* C-PHY analog setting for rates below 1.5 Gsps, then power on */
static void phya_setting(struct mtk_sv_hw *hw, u32 p)
{
	const int C = R_CSI;

	fld(hw, C, p + 0x00, BIT(20), 1);
	fld(hw, C, p + 0x00, BIT(6) | BIT(7), 3);
	wr(hw, C, p + 0x14, 0x55);
	wr(hw, C, p + 0xf0, 0x322);
	fld(hw, C, p + 0x0c, GENMASK(25, 20), 0x20);
	fld(hw, C, p + 0x14, GENMASK(1, 0), 1);
	fld(hw, C, p + 0x14, BIT(4), 0);
	fld(hw, C, p + 0x14, GENMASK(11, 8), 0);
	fld(hw, C, p + 0x14, GENMASK(3, 2), 2);
	fld(hw, C, p + 0x18, GENMASK(28, 24), 0x14);
	fld(hw, C, p + 0x1c, GENMASK(28, 24), 0x14);
	fld(hw, C, p + 0x38, BIT(8), 1);
	phya_power_on(hw, p);
}

/* MAC CSI2: lanes, C/D-PHY select, ECC/B2P/image packets, size checkers */
static void mac_fixed(struct mtk_sv_hw *hw, u32 m, bool cphy, unsigned int lanes)
{
	const int C = R_CSI;

	wr(hw, C, m + 0x00, GENMASK(lanes - 1, 0));
	wr(hw, C, m + 0x04, 0xb6 | cphy);
	/* C-PHY header mode 2 with the trio header length, D-PHY none */
	wr(hw, C, m + 0x08, cphy ? 0x402 : 0);
	wr(hw, C, m + 0x10, 0x11f);
	fld(hw, C, m + 0x14, BIT(24) | BIT(25), 0);
	wr(hw, C, m + 0xb8, 0xf);
	wr(hw, C, m + 0xc0, 0x80000000);
	fld(hw, C, m + 0xe0, BIT(17), 1);
	fld(hw, C, m + 0xf8, GENMASK(7, 0), 0);
	wr(hw, C, m + 0x300, 0x002b0011);
	wr(hw, C, m + 0x310, 0x002b0111);
	wr(hw, C, m + 0x320, 0x002b0211);
	wr(hw, C, m + 0x330, 0x002c0011);
	wr(hw, C, m + 0x340, 0x002d0011);
}

/* lane timing in the D-PHY top (its TRIO fields are the C-PHY settle) */
static void dphy_timing(struct mtk_sv_hw *hw, bool cphy)
{
	const int C = R_CSI;
	u32 o;

	if (cphy) {
		for (o = 0x20; o <= 0x2c; o += 4)
			wr(hw, C, DPHY + o, CPHY_SETTLE << 18);
		for (o = 0x10; o <= 0x14; o += 4) {
			fld(hw, C, DPHY + o, GENMASK(23, 16), CPHY_SETTLE);
			fld(hw, C, DPHY + o, BIT(28), 0);
		}
		return;
	}

	/* data lanes: settle, HS prepare on, no trail; clock lanes: prepare on */
	for (o = 0x20; o <= 0x2c; o += 4)
		wr(hw, C, DPHY + o, DPHY_SETTLE << 18 | BIT(28));
	for (o = 0x10; o <= 0x14; o += 4)
		wr(hw, C, DPHY + o, BIT(28));
	fld(hw, C, DPHY + 0x50, BIT(8), 1);		/* deskew on */
	for (o = 0x60; o <= 0x6c; o += 4)
		fld(hw, C, DPHY + o, BIT(0), 0);	/* no deskew delay */
}

/* PHY A/B as one 4D1C C-PHY port, 3 trios */
static void cphy_setting(struct mtk_sv_hw *hw)
{
	const int C = R_CSI;

	phya_setting(hw, PHYA);
	phya_setting(hw, PHYB);
	fld(hw, C, PHYA + 0xf4, GENMASK(7, 4), 0xc);
	fld(hw, C, PHYA + 0xf4, BIT(2), 1);
	fld(hw, C, PHYA + 0xf4, GENMASK(23, 20), 4);
	fld(hw, C, PHYA + 0xf4, GENMASK(29, 24), 1);
	fld(hw, C, PHYA + 0xf4, GENMASK(19, 16), 0);
	fld(hw, C, PHYA + 0xf4, BIT(30), 1);
	fld(hw, C, PHYA + 0xf4, BIT(3), 0);
	fld(hw, C, PHYB + 0xf4, GENMASK(7, 4), 0xc);
	fld(hw, C, PHYB + 0xf4, BIT(2), 1);
	fld(hw, C, PHYB + 0xf4, GENMASK(23, 20), 4);
	fld(hw, C, PHYB + 0xf4, GENMASK(29, 24), 1);
	fld(hw, C, PHYB + 0xf4, GENMASK(19, 16), 0);
	fld(hw, C, PHYB + 0xf4, BIT(30), 0);
	fld(hw, C, PHYB + 0xf4, BIT(3), 0);
	fld(hw, C, PHYA + 0x100, BIT(0), 1);
	fld(hw, C, PHYA + 0x100, BIT(16), 1);
	fld(hw, C, PHYB + 0x100, BIT(0), 1);
	wr(hw, C, CPHY + 0x168, 0x7);
	fld(hw, C, CPHY + 0x00, GENMASK(3, 0), 0x7);
	wr(hw, C, DPHY + 0xf0, 0xf1);
	wr(hw, C, CPHY + 0xf0, 0xf);
	wr(hw, C, CPHY + 0xf4, 0x00ff0000);
}

/*
 * PHY A/B as one 4D1C D-PHY port, 4 data lanes up to 2.5 Gbps: data lanes
 * on A0/A1/B0/B1, clock on A2. The equaliser setting follows the lane rate.
 */
static void dphy_setting(struct mtk_sv_hw *hw, s64 link_freq)
{
	const int C = R_CSI;
	bool fast = link_freq * 2 >= 1500000000LL;
	u32 p;
	int i;

	for (i = 0; i < 2; i++) {
		p = i ? PHYB : PHYA;
		fld(hw, C, p + 0x00, BIT(20), 0);			/* CPHY_EN */
		fld(hw, C, p + 0x00, GENMASK(14, 12), i ? 0 : 4);	/* CKMODE: A L2 */
		fld(hw, C, p + 0x00, GENMASK(10, 8), 7);		/* CKSEL */
		fld(hw, C, p + 0x00, BIT(6) | BIT(7), 3);		/* HSMODE */
		fld(hw, C, p + 0x14, GENMASK(1, 0), 1);			/* EQ_BW */
		fld(hw, C, p + 0x14, BIT(4), fast);			/* EQ_DG0_EN */
		fld(hw, C, p + 0x14, GENMASK(11, 8), 0);		/* EQ_SR0 */
		fld(hw, C, p + 0x14, GENMASK(3, 2), 2);			/* EQ_IS */
		fld(hw, C, p + 0x38, BIT(8), 1);			/* EQ_OS_IS */
		fld(hw, C, p + 0xf4, GENMASK(23, 20), 3);		/* AFIFO dummy valid num */
	}
	fld(hw, C, PHYA + 0xf4, GENMASK(7, 4), 4);		/* ASYNC_OPTION, no PN swap */
	fld(hw, C, PHYA + 0xf4, BIT(2), 1);			/* AFIFO dummy valid on */
	fld(hw, C, PHYA + 0xf4, GENMASK(19, 16), 4);
	fld(hw, C, PHYA + 0xf4, GENMASK(29, 24), 4);
	fld(hw, C, PHYA + 0xf0, GENMASK(7, 4), 2);		/* async FIFO gating */
	phya_power_on(hw, PHYA);
	phya_power_on(hw, PHYB);

	fld(hw, C, DPHY + 0x204, GENMASK(3, 0), 0xf);		/* lane flush */
	fld(hw, C, PHYA + 0x20, BIT(0), 1);
	for (i = 0; i < 2; i++) {
		p = i ? PHYB : PHYA;
		fld(hw, C, p + 0xf4, GENMASK(29, 24), 3);	/* gap */
		fld(hw, C, p + 0xf4, GENMASK(19, 16), 0);	/* prepare */
		fld(hw, C, p + 0xf4, BIT(30), 1);		/* deskew */
		fld(hw, C, p + 0x100, BIT(0) | BIT(8), 3);	/* L0/L1 AFIFO flush */
	}
	fld(hw, C, PHYA + 0xf4, BIT(3), 0);			/* no split */
	/* LC0, LD0-3 on; LC0 = A2, LD0 = A1, LD1 = B0, LD2 = A0, LD3 = B1 */
	wr(hw, C, DPHY + 0x00, BIT(0) | GENMASK(11, 8));
	wr(hw, C, DPHY + 0x04, 2 | 1 << 8 | 3 << 12 | 0 << 16 | 4 << 20 | BIT(31));
	fld(hw, C, DPHY + 0x08, GENMASK(17, 12), 0);		/* no PN swap */
	fld(hw, C, DPHY + 0x180, BIT(3), 1);			/* sync method */
	wr(hw, C, DPHY + 0xf0, 0xf1);
	wr(hw, C, DPHY + 0x94, 0xf);
}

int mtk_sv_hw_seninf_start(struct mtk_sv *sv, const struct v4l2_mbus_framefmt *fmt)
{
	struct mtk_sv_hw *hw = sv->hw;
	const int S = R_SENINF, C = R_CSI;
	u32 o;
	int i;

	if (sv->port >= MTK_SV_NUM_PORTS ||
	    (sv->cphy && sv->num_lanes != 3) || (!sv->cphy && sv->num_lanes != 4) ||
	    (!sv->cphy && sv->link_freq * 2 > 2500000000LL))
		return -EINVAL;	/* 4D1C ports only: 3 trios, or 4 lanes up to 2.5 Gbps */
	if (!powered(hw))
		return -EIO;
	hw->port = sv->port;

	/* reset */
	fld(hw, S, SEN + 0x210, BIT(4), 1);
	udelay(1);
	fld(hw, S, SEN + 0x210, BIT(4), 0);
	fld(hw, C, MACTOP + 0x00, BIT(0), 1);
	udelay(1);
	fld(hw, C, MACTOP + 0x00, BIT(0), 0);

	/* VC/DT filter on the SENINF csi2: VC 0, DT 0x2b (RAW10) */
	for (o = 0x20; o <= 0x3c; o += 4)
		wr(hw, S, SEN + 0xa00 + o, 0);
	for (o = 0x60; o <= 0x6c; o += 4)
		wr(hw, S, SEN + 0xa00 + o, 0);
	wr(hw, S, SEN + 0xa20, 0x002b0011);
	wr(hw, S, SEN + 0xa60, 0x100);

	/* PHY init */
	phya_init(hw, PHYA, true);
	phya_init(hw, PHYB, false);
	dphy_timing(hw, sv->cphy);
	fld(hw, C, CPHY + 0x18, BIT(4), 1);

	/* MAC top (D-PHY or C-PHY, non-split), MAC CSI2 */
	fld(hw, C, MACTOP + 0x00, BIT(16), 0);
	wr(hw, C, MACTOP + 0x40, sv->cphy ? 0x2 : 0x1);
	mac_fixed(hw, MACA, sv->cphy, sv->num_lanes);
	mac_fixed(hw, MACB, sv->cphy, sv->num_lanes);
	fld(hw, C, CPHY + 0x130, BIT(1), 0);	/* no LRTE */

	/* SENINF: afifo pop 1T to 2T for this port, CSI2 + SENINF on */
	fld(hw, S, SENTOP + 0x004, BIT(hw->port), 1);
	fld(hw, S, SEN + 0x210, BIT(0), 1);
	fld(hw, S, SEN + 0x200, BIT(0), 1);

	if (sv->cphy)
		cphy_setting(hw);
	else
		dphy_setting(hw, sv->link_freq);

	/* seninf mux 0 (MUX1), source this SENINF */
	fld(hw, S, MUX1 + 0xd00, BIT(0), 1);
	fld(hw, S, MUX1 + 0xd04, GENMASK(3, 0), 8);
	fld(hw, S, MUX1 + 0xd04, GENMASK(10, 8), PIX_MODE);
	fld(hw, S, MUX1 + 0xd08, BIT(3), 1);
	fld(hw, S, MUX1 + 0xd08, BIT(16) | BIT(17), 0);
	wr(hw, S, MUX1 + 0xd00, rd(hw, S, MUX1 + 0xd00) | 0x6);
	wr(hw, S, MUX1 + 0xd00, rd(hw, S, MUX1 + 0xd00) & ~0x6);
	fld(hw, S, SENTOP + 0x010, GENMASK(4, 0), SENIDX);
	wr(hw, S, MUX1 + 0xe00, 0x80);

	/* cammux 10: outer page, then inner page */
	for (i = 1; i >= 0; i--) {
		u32 k = CAMMUXB + CAMMUX * 0x40;

		fld(hw, S, CAMMUXB + 0xf04, BIT(7), i);
		wr(hw, S, k + 0x04, 0x0000ab80 | ((TAG / 4) << 24));
		wr(hw, S, k + 0x20, 0x80u << (8 * (TAG % 4)));
		wr(hw, S, k + 0x24, 0xabu << (8 * (TAG % 4)));
		fld(hw, S, k + 0x00, GENMASK(6, 0), MUX_VR);
		wr(hw, S, k + 0x14, (fmt->height << 16) | fmt->width);
		fld(hw, S, k + 0x00, GENMASK(10, 8), PIX_MODE);
		fld(hw, S, k + 0x00, BIT(7), 1);
		wr(hw, S, k + 0x0c, 0x103);
		fld(hw, S, k + 0x00, GENMASK(22, 16), MUX_VR);
	}
	return 0;
}

void mtk_sv_hw_seninf_stop(struct mtk_sv *sv)
{
	struct mtk_sv_hw *hw = sv->hw;

	if (!powered(hw))
		return;
	fld(hw, R_SENINF, CAMMUXB + CAMMUX * 0x40, BIT(7), 0);
	fld(hw, R_SENINF, MUX1 + 0xd00, BIT(0), 0);
	fld(hw, R_SENINF, SEN + 0x200, BIT(0), 0);
	fld(hw, R_SENINF, SEN + 0x210, BIT(0), 0);
}

int mtk_sv_hw_camsv_start(struct mtk_sv *sv, const struct v4l2_pix_format *pix)
{
	struct mtk_sv_hw *hw = sv->hw;
	u32 tmp;
	int ret;

	if (!powered(hw))
		return -EIO;
	gates_on(hw);

	/* SCQ + camsv sw reset (camsv_id 1: CAM_MAIN_SW_RST_1 bits 3:2) */
	wr(hw, R_SVCQ, 0x10, 0);
	wr(hw, R_SVCQ, 0x10, 1);
	readl_poll_timeout_atomic(hw->r[R_SVCQ] + 0x10, tmp, tmp & BIT(1), 1, 1000);
	wr(hw, R_SVCQ, 0x10, 0);
	wr(hw, R_CAMSYS, CAM_MAIN_SW_RST_1, 0);
	wr(hw, R_CAMSYS, CAM_MAIN_SW_RST_1, 0xc);
	wr(hw, R_CAMSYS, CAM_MAIN_SW_RST_1, 0);

	/* DMA top reset */
	wr(hw, R_SVDMA, 0x20, 0);
	wr(hw, R_SVDMA, 0x20, 1);
	ret = readl_poll_timeout_atomic(hw->r[R_SVDMA] + 0x20, tmp, tmp & BIT(1), 1, 1000);
	wr(hw, R_SV, 0x7c, 0);
	wr(hw, R_SV, 0x1d0, 1);
	wr(hw, R_SVDMA, 0x20, 0);
	wr(hw, R_SV, 0x1d0, 0);
	if (ret)
		dev_warn(sv->dev, "camsv DMA reset timeout\n");

	/* IMG and LEN FIFO thresholds */
	wr(hw, R_SVDMA, 0x00, 0x10510028);
	wr(hw, R_SVDMA, 0x04, 0x10510028);
	wr(hw, R_SVDMA, 0x08, 0x80b8008a);
	wr(hw, R_SVDMA, 0x0c, 0x80280000);
	wr(hw, R_SVDMA, 0x10, 0x10010000);
	wr(hw, R_SVDMA, 0x14, 0x10010000);
	wr(hw, R_SVDMA, 0x18, 0x800a0007);
	wr(hw, R_SVDMA, 0x1c, 0x80000000);

	/* TG */
	wr(hw, R_SV, 0x18c, 3);
	fld(hw, R_SV, 0x148, BIT(1), 0);
	fld(hw, R_SV, 0x14c, BIT(8), 0);
	fld(hw, R_SV, 0x1b4, BIT(30), 0);
	fld(hw, R_SV, 0x140, GENMASK(19, 16), 0);
	wr(hw, R_SV, 0x144, 0);

	/* tag grouping, done/error enables */
	wr(hw, R_SV, 0x1b8, BIT(TAG));
	wr(hw, R_SV, 0x344, 0xf0001);
	fld(hw, R_SV, 0x170, BIT(5), 1);
	wr(hw, R_SV, 0x1cc, BIT(TAG));
	wr(hw, R_SV, 0x1c8, BIT(TAG));

	/* grab window, format */
	wr(hw, R_SV, SV_TAG(0x548), pix->width << 16);
	wr(hw, R_SV, SV_TAG(0x54c), pix->height << 16);
	wr(hw, R_SV, SV_TAG(0x554), 1 | BIT(4));	/* BAYER10, UNPACK_MODE: 8 MSBs */
	wr(hw, R_SV, SV_TAG(0x55c), PIX_MODE);

	wr(hw, R_SV, 0x34c, rd(hw, R_SV, 0x34c) | BIT(8 + 3 * TAG) | BIT(9 + 3 * TAG));
	wr(hw, R_SV, 0x35c, rd(hw, R_SV, 0x35c) | BIT(2 + 4 * TAG));
	wr(hw, R_SV, 0x36c, 0x11111111);

	/* IMGO stride, MAX_BURST_LEN 16; FBC on, no buffer yet */
	wr(hw, R_SVDMA, SV_TAG(0x210), (pix->bytesperline << 16) | 0x10);
	wr(hw, R_SV, SV_TAG(0x540), SV_FBC0_EN);

	wr(hw, R_SV, 0x040, BIT(TAG));
	wr(hw, R_SV, 0x044, BIT(TAG));
	wr(hw, R_SVDMA, 0xf30, BIT(TAG) | BIT(TAG) << 8);

	/* double-buffer load, TG on */
	fld(hw, R_SV, 0x1b4, BIT(30), 0);
	fld(hw, R_SV, 0x1b4, BIT(30), 1);
	fld(hw, R_SV, 0x14c, BIT(8), 1);
	fld(hw, R_SV, 0x14c, BIT(8), 0);
	rd(hw, R_SV, SV_DONE_STATUS);
	fld(hw, R_SV, 0x140, BIT(0), 1);
	fld(hw, R_SV, 0x148, BIT(0), 1);
	return 0;
}

void mtk_sv_hw_camsv_stop(struct mtk_sv *sv)
{
	struct mtk_sv_hw *hw = sv->hw;

	if (!powered(hw))
		return;
	fld(hw, R_SV, 0x148, BIT(0), 0);
	fld(hw, R_SV, 0x140, BIT(0), 0);
	wr(hw, R_SV, 0x040, 0);
	wr(hw, R_SV, 0x044, 0);
	wr(hw, R_SV, SV_TAG(0x540), 0);
	wr(hw, R_SV, 0x344, 0);
}

/* Program the IMGO address and hand the buffer to the FBC. */
void mtk_sv_hw_set_addr(struct mtk_sv *sv, dma_addr_t addr)
{
	struct mtk_sv_hw *hw = sv->hw;

	wr(hw, R_SVDMA, SV_TAG(0x200), lower_32_bits(addr));
	wr(hw, R_SVDMA, SV_TAG(0x204), upper_32_bits(addr) & 0xf);
	wr(hw, R_SV, SV_TAG(0x540), SV_FBC0_EN | SV_FBC0_RCNT_INC);
}

u32 mtk_sv_hw_irq_status(struct mtk_sv *sv)
{
	u32 st = rd(sv->hw, R_SV, SV_DONE_STATUS);	/* read-clear */

	return (st & SV_DONE_FRAME) ? MTK_SV_IRQ_FRAME_DONE | st : st;
}
