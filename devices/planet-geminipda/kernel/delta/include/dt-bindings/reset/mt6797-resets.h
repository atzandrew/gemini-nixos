/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * MT6797 INFRACFG_AO software resets (reset id = 32 * bank + bit).
 * Banks: RST0 set/clr 0x120/0x124, RST1 0x130/0x134, RST2 0x140/0x144,
 * RST3 0x150/0x154. Bits from the vendor 3.18 kernel:
 *   thermal controller  INFRA_GLOBALCON_RST_0 bit 0 (thermal/mt6797 mtk_tc.c)
 *   PMIC wrapper        INFRA_GLOBALCON_RST_2 bit 0 (0x140/0x144)
 */

#ifndef _DT_BINDINGS_RESET_CONTROLLER_MT6797
#define _DT_BINDINGS_RESET_CONTROLLER_MT6797

#define MT6797_INFRA_THERM_CTRL_SW_RST		0
#define MT6797_INFRA_PMIC_WRAP_SW_RST		64

#endif  /* _DT_BINDINGS_RESET_CONTROLLER_MT6797 */
