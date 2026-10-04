// SPDX-License-Identifier: GPL-2.0
/*
 * mt6351-probe.c — READ-ONLY look at the MT6351 PMIC fuel gauge (FGADC),
 * AUXADC and charger-detect registers on the Gemini PDA.
 *
 * Why: Android on this device ran MediaTek "HAFG 2.0" — a hardware
 * coulomb counter inside the MT6351 (10 mOhm sense resistor), not a
 * voltage guess. Linux has never touched it. This module answers step 0
 * of the bring-up plan (claude/power.md): did LK leave the FGADC running
 * and counting, and what do the AUXADC battery channels hold?
 *
 * How: no DT node and no driver binding. It looks up the already-probed
 * pwrap device (mediatek,mt6797-pwrap) and borrows the 16-bit PMIC
 * regmap that mtk-pmic-wrap registers on it (the same regmap the
 * mt6351-regulator / mt6351-sound / mt6351-keys children use).
 *
 * Default = strictly read-only: regmap_read() of a fixed list of
 * FGADC / AUXADC result / status registers. Nothing is written, no ADC
 * conversion is requested.
 *
 * Optional, off by default: adc=1 requests fresh AUXADC conversions for
 * BATSNS (ch0, VBAT) and VCDT (ch2, VBUS) on each status read — a write
 * of the channel bit to AUXADC_RQST0_SET (0x0E98), the vendor's request
 * path; result read from the AP-side copy (AUXADC_ADC23) for ch0.
 *
 * Optional, off by default: latch=1 (module param, writable at runtime
 * via /sys/module/mt6351_probe/parameters/latch) performs the vendor
 * FG "read latch" handshake on the next status read. It writes ONLY
 * the upper byte of FGADC_CON0 (0x0200 latch / 0x0800 clear / 0x0000),
 * exactly as the vendor battery_meter_hal.c does, leaving FG_ON / CAL /
 * AUTOCALRATE (low byte) untouched. It never resets the counters.
 *
 * Files (debugfs):
 *   /sys/kernel/debug/mt6351_probe/status   decoded summary (re-read on every cat)
 *   /sys/kernel/debug/mt6351_probe/regs     raw dump of every register read
 *
 * Register map + scaling: vendor gemian/gemini-linux-kernel-3.18
 * (include/mt-plat/mt6797/include/mach/upmu_hw.h, mt_battery_meter.h,
 * power/mt6797/battery_meter_hal.c, pmic_auxadc.c).
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>

/* ---- MT6351 registers (PMIC address space, 16-bit) ------------------ */
#define MT6351_HWCID		0x0200
#define MT6351_SWCID		0x0202
#define MT6351_TOP_CKPDN_CON2	0x0246	/* bit2 FGADC_ANA_CK_PDN, bit3 FGADC_DIG_CK_PDN (1 = gated) */
#define MT6351_FGADC_CON(n)	(0x0CA4 + 2 * (n))	/* n = 0..33 */
#define MT6351_AUXADC_ADC(n)	(0x0E00 + 2 * (n))	/* n = 0..39 */
#define MT6351_AUXADC_RQST0	0x0E96
#define MT6351_AUXADC_RQST0_SET	0x0E98	/* write-1-to-set: bit n requests a conversion on channel n */
#define MT6351_AUXADC_CON0	0x0EA2
#define MT6351_CHR_CON0		0x0F78	/* bit5 RGS_CHRDET (VBUS present) */

/* FGADC_CON0 fields */
#define FG_ON			BIT(0)
#define FG_CAL_SHIFT		2	/* 2 bits */
#define FG_AUTOCALRATE_SHIFT	4	/* 3 bits */
#define FG_SW_CR		BIT(8)
#define FG_SW_READ_PRE		BIT(9)
#define FG_LATCHDATA_ST		BIT(10)
#define FG_SW_CLEAR		BIT(11)

/* FGADC_CON index map */
#define CON_CAR_34_19		1
#define CON_CAR_18_03		2
#define CON_CAR_02_00		3
#define CON_NTER_32_17		4
#define CON_NTER_16_01		5
#define CON_NTER_00		6
#define CON_CURRENT_OUT		11
#define CON_OFFSET		13
#define CON_OSR			15
#define CON_R_CURR		25	/* real-time current, no latch needed */

/* AUXADC result index map (value bits 14:0 or 11:0, bit15 = ready) */
#define ADC_CH0_BATSNS		0
#define ADC_CH1_ISENSE		1
#define ADC_CH2_VCDT		2
#define ADC_CH3_BATON		3
#define ADC_WAKEUP_PCHR		20	/* VBAT latched at power-on (HW OCV) */
#define ADC_WAKEUP_SWCHR	21
#define ADC_CH0_BY_AP		23
#define ADC_CH1_BY_AP		25

/* ---- Board / vendor calibration (mt_battery_meter.h, header wins) --- */
#define R_FG_MOHM		10	/* sense resistor; vendor math is based on 20 */
#define CAR_TUNE_PCT		115
#define UNIT_FGCURRENT_NA	158122	/* 158.122 uA per LSB at 20 mOhm (in nA) */
#define UNIT_FGCAR_NAH_X8	359860	/* 359.86 uAh per LSB (in nAh), /8 for FG_OSR=8 */
#define VFULL_MV		1800
#define RBAT_PULL_UP_OHM	24000
#define RBAT_PULL_UP_MV		2800	/* header value; dtsi says 1800 — verify */

static bool latch;
module_param(latch, bool, 0644);
MODULE_PARM_DESC(latch, "1 = do the vendor FG read-latch handshake (writes FGADC_CON0[15:8] only)");

static bool adc_req;
module_param_named(adc, adc_req, bool, 0644);
MODULE_PARM_DESC(adc, "1 = request fresh AUXADC conversions (BATSNS ch0, VCDT ch2) on each status read (writes AUXADC_RQST0_SET only)");

static struct platform_device *pwrap_pdev;
static struct regmap *pmic;
static struct dentry *dbg_dir;
static DEFINE_MUTEX(probe_lock);

static int rd(unsigned int reg, unsigned int *val)
{
	int ret = regmap_read(pmic, reg, val);

	if (ret)
		*val = 0;
	return ret;
}

/* 10k NTC table from the vendor bat_meter node: { degC, ohm } */
static const int ntc_table[][2] = {
	{ -20, 68237 }, { -15, 53650 }, { -10, 42506 }, { -5, 33892 },
	{ 0, 27219 },   { 5, 22021 },   { 10, 17926 },  { 15, 14674 },
	{ 20, 12081 },  { 25, 10000 },  { 30, 8315 },   { 35, 6948 },
	{ 40, 5834 },   { 45, 4917 },   { 50, 4161 },   { 55, 3535 },
	{ 60, 3014 },
};

/* returns tenths of degC, or INT_MIN if out of range */
static int ntc_ohm_to_dc(int ohm)
{
	int i;

	if (ohm <= 0 || ohm > ntc_table[0][1] || ohm < ntc_table[ARRAY_SIZE(ntc_table) - 1][1])
		return INT_MIN;
	for (i = 0; i < ARRAY_SIZE(ntc_table) - 1; i++) {
		int r_hi = ntc_table[i][1], r_lo = ntc_table[i + 1][1];
		int t_hi = ntc_table[i][0], t_lo = ntc_table[i + 1][0];

		if (ohm <= r_hi && ohm >= r_lo)
			return t_hi * 10 + (r_hi - ohm) * (t_lo - t_hi) * 10 / (r_hi - r_lo);
	}
	return INT_MIN;
}

/* FG current LSB -> uA (signed; + = charging), vendor scaling */
static long fg_raw_to_ua(u16 raw)
{
	long long v = (s16)raw;

	v = v * UNIT_FGCURRENT_NA;		/* nA at 20 mOhm basis */
	v = v * 20 / R_FG_MOHM;			/* actual sense resistor */
	v = v * CAR_TUNE_PCT / 100;		/* board tune */
	return (long)div_s64(v, 1000);		/* -> uA */
}

/*
 * Vendor CAR decode (fgauge_read_columb_internal): 17-bit magnitude from
 * CAR[30:14] plus sign bit CAR[34]; LSB 359.86 uAh / 8 (OSR) at 20 mOhm.
 * Returns uAh (signed; + = net charge in).
 */
static long car_to_uah(unsigned int c1, unsigned int c2, int *raw17)
{
	unsigned int v = (c2 >> 11) | ((c1 & 0x0FFF) << 5);
	long long q;

	if (c1 & 0x8000)
		q = (long long)v - 0x1ffff;	/* discharging: negative */
	else
		q = v;
	*raw17 = (int)q;
	q = q * UNIT_FGCAR_NAH_X8;		/* nAh, before /8 */
	q = div_s64(q, 8);			/* FG_OSR = 8 */
	q = q * 20 / R_FG_MOHM;
	q = q * CAR_TUNE_PCT / 100;
	return (long)div_s64(q, 1000);		/* -> uAh */
}

/* Vendor latch handshake: only touches FGADC_CON0[15:8]. */
static int fg_latch(void)
{
	unsigned int v;
	int i, ret;

	ret = regmap_update_bits(pmic, MT6351_FGADC_CON(0), 0xFF00, FG_SW_READ_PRE);
	if (ret)
		return ret;
	for (i = 0; i < 1000; i++) {
		rd(MT6351_FGADC_CON(0), &v);
		if (v & FG_LATCHDATA_ST)
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static void fg_unlatch(void)
{
	unsigned int v;
	int i;

	regmap_update_bits(pmic, MT6351_FGADC_CON(0), 0xFF00, FG_SW_CLEAR);
	for (i = 0; i < 1000; i++) {
		rd(MT6351_FGADC_CON(0), &v);
		if (!(v & FG_LATCHDATA_ST))
			break;
		udelay(10);
	}
	regmap_update_bits(pmic, MT6351_FGADC_CON(0), 0xFF00, 0);
}

/*
 * Vendor IMM_GetOneChannelValue (pmic_auxadc.c): write the channel bit to
 * AUXADC_RQST0_SET, then poll the channel's ready bit (bit 15 of its
 * result register) every ~1.4 ms. We additionally wait one poll period
 * before the first check so a stale ready bit from an older result is not
 * mistaken for the new one. The vendor also forces AUXADC_CK_AON first;
 * we deliberately do not (try the plain request first).
 * Returns the raw result (bits 14:0) or a negative errno.
 */
static int adc_request(int ch, unsigned int out_reg, unsigned int *waited_us)
{
	unsigned int v = 0;
	int i, ret;

	ret = regmap_write(pmic, MT6351_AUXADC_RQST0_SET, BIT(ch));
	if (ret)
		return ret;
	for (i = 0; i < 40; i++) {
		usleep_range(1300, 1500);
		ret = rd(out_reg, &v);
		if (ret)
			return ret;
		if (v & 0x8000) {
			*waited_us = (i + 1) * 1400;
			return v & 0x7FFF;
		}
	}
	*waited_us = 40 * 1400;
	return -ETIMEDOUT;
}

static int status_show(struct seq_file *s, void *unused)
{
	unsigned int hwcid, swcid, ck2, con[34], adc[40], chr;
	u64 nter;
	bool do_latch = READ_ONCE(latch);
	bool do_adc = READ_ONCE(adc_req);
	int f_bat = -ENODATA, f_vcdt = -ENODATA;
	unsigned int w_bat = 0, w_vcdt = 0, pre_bat = 0, pre_vcdt = 0;
	long r_curr_ua, cur_out_ua, car_uah;
	int raw17, i, latched = 0, lret = 0;
	int batsns_mv, batsns_ap_mv, ocv_mv, ocv_sw_mv, isense_mv, vcdt_mv, baton_mv;

	mutex_lock(&probe_lock);

	if (do_latch) {
		lret = fg_latch();
		latched = (lret == 0);
	}

	rd(MT6351_HWCID, &hwcid);
	rd(MT6351_SWCID, &swcid);
	rd(MT6351_TOP_CKPDN_CON2, &ck2);
	for (i = 0; i < 34; i++)
		rd(MT6351_FGADC_CON(i), &con[i]);
	for (i = 0; i < 40; i++)
		rd(MT6351_AUXADC_ADC(i), &adc[i]);
	rd(MT6351_CHR_CON0, &chr);

	if (do_adc) {
		pre_bat = adc[ADC_CH0_BY_AP];
		pre_vcdt = adc[ADC_CH2_VCDT];
		f_bat = adc_request(0, MT6351_AUXADC_ADC(ADC_CH0_BY_AP), &w_bat);
		f_vcdt = adc_request(2, MT6351_AUXADC_ADC(ADC_CH2_VCDT), &w_vcdt);
	}

	if (do_latch)
		fg_unlatch();

	mutex_unlock(&probe_lock);

	seq_printf(s, "mt6351_probe  mode=%s%s%s\n",
		   do_latch ? "latch" : "read-only", do_adc ? "+adc" : "",
		   do_latch ? (latched ? "" : lret == -ETIMEDOUT ? " (LATCH TIMEOUT)" : " (latch error)") : "");
	seq_printf(s, "chip          HWCID=0x%04x SWCID=0x%04x\n", hwcid, swcid);

	seq_puts(s, "\n[fuel gauge]\n");
	seq_printf(s, "clocks        TOP_CKPDN_CON2=0x%04x  ana_ck=%s dig_ck=%s\n", ck2,
		   (ck2 & BIT(2)) ? "GATED" : "on", (ck2 & BIT(3)) ? "GATED" : "on");
	seq_printf(s, "FGADC_CON0    0x%04x  FG_ON=%u CAL=%u AUTOCALRATE=%u SW_CR=%u READ_PRE=%u LATCH_ST=%u SW_CLEAR=%u\n",
		   con[0], !!(con[0] & FG_ON), (con[0] >> FG_CAL_SHIFT) & 3,
		   (con[0] >> FG_AUTOCALRATE_SHIFT) & 7, !!(con[0] & FG_SW_CR),
		   !!(con[0] & FG_SW_READ_PRE), !!(con[0] & FG_LATCHDATA_ST),
		   !!(con[0] & FG_SW_CLEAR));
	seq_printf(s, "FG_OSR        %u (vendor sets 8)   FG_OFFSET raw=%d\n",
		   con[CON_OSR] & 0xF, (s16)con[CON_OFFSET]);

	r_curr_ua = fg_raw_to_ua(con[CON_R_CURR]);
	seq_printf(s, "R_CURR        raw=0x%04x (%d)  -> %ld mA  (%s; real-time, no latch needed)\n",
		   con[CON_R_CURR], (s16)con[CON_R_CURR], r_curr_ua / 1000,
		   (s16)con[CON_R_CURR] > 0 ? "charging" : (s16)con[CON_R_CURR] < 0 ? "discharging" : "zero");

	cur_out_ua = fg_raw_to_ua(con[CON_CURRENT_OUT]);
	seq_printf(s, "CURRENT_OUT   raw=0x%04x (%d)  -> %ld mA  (%s)\n",
		   con[CON_CURRENT_OUT], (s16)con[CON_CURRENT_OUT], cur_out_ua / 1000,
		   latched ? "latched now" : "value from the last latch, possibly stale");

	car_uah = car_to_uah(con[CON_CAR_34_19], con[CON_CAR_18_03], &raw17);
	seq_printf(s, "CAR           CON1=0x%04x CON2=0x%04x CON3=0x%04x  raw17=%d  -> %ld.%03ld mAh net since counter reset (%s)\n",
		   con[CON_CAR_34_19], con[CON_CAR_18_03], con[CON_CAR_02_00], raw17,
		   car_uah / 1000, abs(car_uah % 1000),
		   latched ? "latched now" : "unlatched read");

	nter = ((u64)con[CON_NTER_32_17] << 17) | ((u64)con[CON_NTER_16_01] << 1) | (con[CON_NTER_00] & 1);
	seq_printf(s, "NTER          %llu samples (CON4=0x%04x CON5=0x%04x CON6=0x%04x) — moving = FG is sampling\n",
		   nter, con[CON_NTER_32_17], con[CON_NTER_16_01], con[CON_NTER_00]);

	seq_puts(s, "\n[auxadc — last results, no new conversion requested]\n");
	batsns_mv = (adc[ADC_CH0_BATSNS] & 0x7FFF) * 3 * VFULL_MV / 32768;
	batsns_ap_mv = (adc[ADC_CH0_BY_AP] & 0x7FFF) * 3 * VFULL_MV / 32768;
	ocv_mv = (adc[ADC_WAKEUP_PCHR] & 0x7FFF) * 3 * VFULL_MV / 32768;
	ocv_sw_mv = (adc[ADC_WAKEUP_SWCHR] & 0x7FFF) * 3 * VFULL_MV / 32768;
	isense_mv = (adc[ADC_CH1_ISENSE] & 0x7FFF) * 3 * VFULL_MV / 32768;
	vcdt_mv = (adc[ADC_CH2_VCDT] & 0x0FFF) * 1 * VFULL_MV / 4096;
	baton_mv = (adc[ADC_CH3_BATON] & 0x0FFF) * 2 * VFULL_MV / 4096;

	seq_printf(s, "BATSNS ch0    0x%04x rdy=%u  %d mV (VBAT)\n", adc[ADC_CH0_BATSNS],
		   !!(adc[ADC_CH0_BATSNS] & 0x8000), batsns_mv);
	seq_printf(s, "BATSNS by AP  0x%04x rdy=%u  %d mV\n", adc[ADC_CH0_BY_AP],
		   !!(adc[ADC_CH0_BY_AP] & 0x8000), batsns_ap_mv);
	seq_printf(s, "ISENSE ch1    0x%04x rdy=%u  %d mV\n", adc[ADC_CH1_ISENSE],
		   !!(adc[ADC_CH1_ISENSE] & 0x8000), isense_mv);
	seq_printf(s, "boot OCV      PCHR 0x%04x -> %d mV   SWCHR 0x%04x -> %d mV  (latched at power-on)\n",
		   adc[ADC_WAKEUP_PCHR], ocv_mv, adc[ADC_WAKEUP_SWCHR], ocv_sw_mv);
	seq_printf(s, "VCDT ch2      0x%04x rdy=%u  %d mV at pin (VBUS via divider, R_CHARGER 330k/39k)\n",
		   adc[ADC_CH2_VCDT], !!(adc[ADC_CH2_VCDT] & 0x8000), vcdt_mv);
	if (baton_mv > 0 && baton_mv < RBAT_PULL_UP_MV) {
		int ohm = RBAT_PULL_UP_OHM * baton_mv / (RBAT_PULL_UP_MV - baton_mv);
		int dc = ntc_ohm_to_dc(ohm);

		if (dc == INT_MIN)
			seq_printf(s, "BATON ch3     0x%04x  %d mV -> NTC ~%d ohm (outside table)\n",
				   adc[ADC_CH3_BATON], baton_mv, ohm);
		else
			seq_printf(s, "BATON ch3     0x%04x  %d mV -> NTC ~%d ohm -> ~%d.%d C (pull-up assumed %d mV)\n",
				   adc[ADC_CH3_BATON], baton_mv, ohm, dc / 10, abs(dc % 10),
				   RBAT_PULL_UP_MV);
	} else {
		seq_printf(s, "BATON ch3     0x%04x  %d mV\n", adc[ADC_CH3_BATON], baton_mv);
	}

	if (do_adc) {
		seq_puts(s, "\n[auxadc — fresh conversions requested now]\n");
		if (f_bat >= 0)
			seq_printf(s, "VBAT fresh    %d mV  (BATSNS by AP raw=0x%04x, ready after ~%u us; before: 0x%04x)\n",
				   f_bat * 3 * VFULL_MV / 32768, f_bat, w_bat, pre_bat);
		else
			seq_printf(s, "VBAT fresh    ERROR %d%s (before: 0x%04x)\n", f_bat,
				   f_bat == -ETIMEDOUT ? " (no ready bit within ~56 ms)" : "", pre_bat);
		if (f_vcdt >= 0) {
			int pin = (f_vcdt & 0x0FFF) * VFULL_MV / 4096;

			seq_printf(s, "VCDT fresh    %d mV at pin -> VBUS ~%d mV  (raw=0x%04x, ~%u us; before: 0x%04x)\n",
				   pin, pin * (330 + 39) / 39, f_vcdt, w_vcdt, pre_vcdt);
		} else {
			seq_printf(s, "VCDT fresh    ERROR %d%s (before: 0x%04x)\n", f_vcdt,
				   f_vcdt == -ETIMEDOUT ? " (no ready bit within ~56 ms)" : "", pre_vcdt);
		}
	}

	seq_puts(s, "\n[charger detect]\n");
	seq_printf(s, "CHR_CON0      0x%04x  CHRDET=%u (VBUS present)\n", chr, !!(chr & BIT(5)));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static int regs_show(struct seq_file *s, void *unused)
{
	static const struct { unsigned int reg; const char *name; } singles[] = {
		{ MT6351_HWCID, "HWCID" },
		{ MT6351_SWCID, "SWCID" },
		{ MT6351_TOP_CKPDN_CON2, "TOP_CKPDN_CON2" },
		{ MT6351_AUXADC_RQST0, "AUXADC_RQST0" },
		{ MT6351_AUXADC_CON0, "AUXADC_CON0" },
		{ MT6351_CHR_CON0, "CHR_CON0" },
	};
	unsigned int v;
	int i;

	mutex_lock(&probe_lock);
	for (i = 0; i < ARRAY_SIZE(singles); i++) {
		rd(singles[i].reg, &v);
		seq_printf(s, "0x%04x %-16s 0x%04x\n", singles[i].reg, singles[i].name, v);
	}
	for (i = 0; i < 34; i++) {
		rd(MT6351_FGADC_CON(i), &v);
		seq_printf(s, "0x%04x FGADC_CON%-7d 0x%04x\n", MT6351_FGADC_CON(i), i, v);
	}
	for (i = 0; i < 40; i++) {
		rd(MT6351_AUXADC_ADC(i), &v);
		seq_printf(s, "0x%04x AUXADC_ADC%-6d 0x%04x\n", MT6351_AUXADC_ADC(i), i, v);
	}
	mutex_unlock(&probe_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regs);

static int __init mt6351_probe_init(void)
{
	struct device_node *np;
	unsigned int hwcid = 0, con0 = 0, ck2 = 0;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-pwrap");
	if (!np) {
		pr_err("mt6351_probe: no mediatek,mt6797-pwrap node\n");
		return -ENODEV;
	}
	pwrap_pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pwrap_pdev) {
		pr_err("mt6351_probe: pwrap platform device not found\n");
		return -ENODEV;
	}
	pmic = dev_get_regmap(&pwrap_pdev->dev, NULL);
	if (!pmic) {
		pr_err("mt6351_probe: pwrap has no regmap (driver not bound?)\n");
		put_device(&pwrap_pdev->dev);
		return -ENODEV;
	}

	dbg_dir = debugfs_create_dir("mt6351_probe", NULL);
	debugfs_create_file("status", 0444, dbg_dir, NULL, &status_fops);
	debugfs_create_file("regs", 0444, dbg_dir, NULL, &regs_fops);

	rd(MT6351_HWCID, &hwcid);
	rd(MT6351_FGADC_CON(0), &con0);
	rd(MT6351_TOP_CKPDN_CON2, &ck2);
	pr_info("mt6351_probe: HWCID=0x%04x FGADC_CON0=0x%04x (FG_ON=%u) TOP_CKPDN_CON2=0x%04x (fg clocks %s) — see /sys/kernel/debug/mt6351_probe/\n",
		hwcid, con0, !!(con0 & FG_ON), ck2,
		(ck2 & (BIT(2) | BIT(3))) ? "GATED" : "on");
	return 0;
}

static void __exit mt6351_probe_exit(void)
{
	debugfs_remove_recursive(dbg_dir);
	put_device(&pwrap_pdev->dev);
}

module_init(mt6351_probe_init);
module_exit(mt6351_probe_exit);
MODULE_DESCRIPTION("Gemini PDA: read-only MT6351 fuel-gauge/AUXADC register probe");
MODULE_LICENSE("GPL");
