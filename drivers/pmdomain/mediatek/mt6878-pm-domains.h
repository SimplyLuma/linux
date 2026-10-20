/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_MEDIATEK_MT6878_PM_DOMAINS_H
#define __SOC_MEDIATEK_MT6878_PM_DOMAINS_H

#include "mtk-pm-domains.h"
#include <dt-bindings/power/mediatek,mt6878-power.h>

/*
 * MT6878 power domain support. Only the camera domains are described so
 * far: CAM_VCORE, CAM_MAIN (a sub-domain of CAM_VCORE) and CSI_RX. MM_INFRA,
 * which CAM_VCORE depends on, is hardware-voted and left as the bootloader
 * sets it up (it also carries the display).
 */

#define MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_SET		0xc14
#define MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_CLR		0xc18
#define MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_STA		0xc1c
#define MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_CAM_MAIN	BIT(29)
#define MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_CAM_MAIN_2ND	BIT(30)
#define MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_CAM_VCORE	BIT(31)
#define MT6878_TOP_AXI_PROT_EN_MMSYS_STA_1_SET		0xc24
#define MT6878_TOP_AXI_PROT_EN_MMSYS_STA_1_CLR		0xc28
#define MT6878_TOP_AXI_PROT_EN_MMSYS_STA_1_STA		0xc2c
#define MT6878_TOP_AXI_PROT_EN_MMSYS_STA_1_CAM_VCORE	(BIT(9) | BIT(10))

#define MT6878_PWR_ACK					BIT(30)
#define MT6878_PWR_ACK_2ND				BIT(31)

static enum scpsys_bus_prot_block scpsys_bus_prot_blocks_mt6878[] = {
	BUS_PROT_BLOCK_INFRA,
};

static const struct scpsys_domain_data scpsys_domain_data_mt6878[] = {
	[MT6878_POWER_DOMAIN_CSI_RX] = {
		.name = "csi-rx",
		.sta_mask = MT6878_PWR_ACK,
		.sta2nd_mask = MT6878_PWR_ACK_2ND,
		.ctl_offs = 0xe9c,
		.pwr_sta_offs = 0xe9c,
		.pwr_sta2nd_offs = 0xe9c,
		.caps = MTK_SCPD_KEEP_DEFAULT_OFF,
	},
	[MT6878_POWER_DOMAIN_CAM_VCORE] = {
		.name = "cam-vcore",
		.sta_mask = MT6878_PWR_ACK,
		.sta2nd_mask = MT6878_PWR_ACK_2ND,
		.ctl_offs = 0xe5c,
		.pwr_sta_offs = 0xe5c,
		.pwr_sta2nd_offs = 0xe5c,
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_CAM_VCORE,
					MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_SET,
					MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_CLR,
					MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_STA),
			BUS_PROT_WR_IGN(INFRA, MT6878_TOP_AXI_PROT_EN_MMSYS_STA_1_CAM_VCORE,
					MT6878_TOP_AXI_PROT_EN_MMSYS_STA_1_SET,
					MT6878_TOP_AXI_PROT_EN_MMSYS_STA_1_CLR,
					MT6878_TOP_AXI_PROT_EN_MMSYS_STA_1_STA),
		},
		.caps = MTK_SCPD_KEEP_DEFAULT_OFF,
	},
	[MT6878_POWER_DOMAIN_CAM_MAIN] = {
		.name = "cam-main",
		.sta_mask = MT6878_PWR_ACK,
		.sta2nd_mask = MT6878_PWR_ACK_2ND,
		.ctl_offs = 0xe48,
		.pwr_sta_offs = 0xe48,
		.pwr_sta2nd_offs = 0xe48,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(12),
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_CAM_MAIN,
					MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_SET,
					MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_CLR,
					MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_STA),
			BUS_PROT_WR_IGN(INFRA, MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_CAM_MAIN_2ND,
					MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_SET,
					MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_CLR,
					MT6878_TOP_AXI_PROT_EN_MMSYS_STA_0_STA),
		},
		.caps = MTK_SCPD_KEEP_DEFAULT_OFF,
	},
};

static const struct scpsys_soc_data mt6878_scpsys_data = {
	.domains_data = scpsys_domain_data_mt6878,
	.num_domains = ARRAY_SIZE(scpsys_domain_data_mt6878),
	.bus_prot_blocks = scpsys_bus_prot_blocks_mt6878,
	.num_bus_prot_blocks = ARRAY_SIZE(scpsys_bus_prot_blocks_mt6878),
	.type = SCPSYS_MTCMOS_TYPE_DIRECT_CTL,
};

#endif /* __SOC_MEDIATEK_MT6878_PM_DOMAINS_H */
