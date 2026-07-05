/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026, Jorijn van der Graaf
 *
 * Register map for the Qualcomm WCD9378 audio codec.
 *
 * The codec exposes its control registers in the SoundWire SDCA control
 * address space (bit 30 set, SDCA function number in bits 25:22), accessed
 * through the TX SoundWire slave. The analog core registers (function 0,
 * implementation-defined region at +0x180000) are layout-compatible with
 * the WCD937x family.
 */

#ifndef __WCD9378_H__
#define __WCD9378_H__

#include <linux/regmap.h>
#include <sound/soc.h>

/* SDCA function 0 (extension unit): device identity */
#define WCD9378_FUNC_EXT_ID_0			0x40000048
#define WCD9378_FUNC_EXT_ID_1			0x40000049
#define WCD9378_FUNC_EXT_VER			0x40000050
#define WCD9378_FUNC_STAT			0x40080000
#define WCD9378_DEV_MANU_ID_0			0x40100060
#define WCD9378_DEV_MANU_ID_1			0x40100061
#define WCD9378_DEV_PART_ID_0			0x40100068
#define WCD9378_DEV_PART_ID_1			0x40100069
#define WCD9378_DEV_VER				0x40100070

/* Analog core (WCD937x-compatible layout), function 0 + 0x180000 */
#define WCD9378_ANA_PAGE			0x40180000
#define WCD9378_ANA_BIAS			0x40180001
#define WCD9378_ANA_RX_SUPPLIES			0x40180008
#define WCD9378_ANA_TX_CH1			0x4018000e
#define WCD9378_ANA_TX_CH2			0x4018000f
#define WCD9378_ANA_TX_CH3			0x40180010
#define WCD9378_ANA_MICB1			0x40180022
#define WCD9378_ANA_MICB2			0x40180023
#define WCD9378_ANA_MICB2_RAMP			0x40180024
#define WCD9378_ANA_MICB3			0x40180025

/* Sequencer block (SEQR) */
#define WCD9378_SYS_USAGE_CTRL			0x40180501
#define WCD9378_SM0_MB_SEL			0x401805b0
#define WCD9378_SM1_MB_SEL			0x401805b1
#define WCD9378_SM2_MB_SEL			0x401805b2

/* SDCA function activation (one per function) */
#define WCD9378_SMP_AMP_FUNC_STAT		0x40880000
#define WCD9378_SMP_AMP_FUNC_ACT		0x40880008
#define WCD9378_SMP_JACK_FUNC_STAT		0x40c80000
#define WCD9378_SMP_JACK_FUNC_ACT		0x40c80008
#define WCD9378_SMP_MIC_CTRL0_FUNC_STAT		0x41080000
#define WCD9378_SMP_MIC_CTRL0_FUNC_ACT		0x41080008
#define WCD9378_SMP_MIC_CTRL1_FUNC_STAT		0x41480000
#define WCD9378_SMP_MIC_CTRL1_FUNC_ACT		0x41480008
#define WCD9378_SMP_MIC_CTRL2_FUNC_STAT		0x41880000
#define WCD9378_SMP_MIC_CTRL2_FUNC_ACT		0x41880008

#define WCD9378_MAX_REGISTER			0x41900070

#endif /* __WCD9378_H__ */
