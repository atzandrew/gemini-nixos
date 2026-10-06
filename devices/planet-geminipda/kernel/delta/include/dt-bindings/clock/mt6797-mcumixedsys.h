/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * MT6797 MCUMIXEDSYS (0x1001a000) CPU clocks — Gemini PDA
 * (drivers/clk/mediatek/clk-mt6797-mcu.c, docs/cpu-dvfs.md).
 */
#ifndef _DT_BINDINGS_CLK_MT6797_MCUMIXEDSYS_H
#define _DT_BINDINGS_CLK_MT6797_MCUMIXEDSYS_H

#define CLK_MCU_ARMPLL_LL	0	/* ARMCAXPLL0: A53 cluster LL (cpu0-3) */
#define CLK_MCU_ARMPLL_L	1	/* ARMCAXPLL1: A53 cluster L (cpu4-7) */
#define CLK_MCU_ARMPLL_CCI	2	/* ARMCAXPLL2: CCI / MCSI bus */
#define CLK_MCU_LL_SEL		3	/* ARMPLLDIV_MUXSEL[3:2] */
#define CLK_MCU_L_SEL		4	/* ARMPLLDIV_MUXSEL[5:4] */
#define CLK_MCU_CCI_SEL		5	/* ARMPLLDIV_MUXSEL[7:6] */
#define CLK_MCU_LL		6	/* ARMPLLDIV_CKDIV[9:5]   -> cpu0-3 clock */
#define CLK_MCU_L		7	/* ARMPLLDIV_CKDIV[14:10] -> cpu4-7 clock */
#define CLK_MCU_CCI		8	/* ARMPLLDIV_CKDIV[19:15] -> CCI clock */
#define CLK_MCU_NR_CLK		9

#endif
