// SPDX-License-Identifier: GPL-2.0
/*
 * mt6351-gauge.c — MT6351 PMIC fuel gauge as a Linux power_supply
 * (Gemini PDA). STEP 3: measurements + state of charge
 * (claude/power.md "Bringing the MT6351 gauge online").
 *
 * /sys/class/power_supply/mt6351-battery (type Battery):
 *   status            Full / Charging / Discharging / Not charging
 *   capacity, capacity_level, charge_now/full/full_design, energy_*
 *   voltage_now       fresh MT6351 AUXADC BATSNS conversion
 *   voltage_ocv       estimated resting voltage (V - I*R)
 *   current_now       latched FG CURRENT_OUT (uA, + = charging)
 *   current_avg       coulomb-counter slope over the last poll window
 *   power_now         |voltage_now * current_avg| (uW)
 *   charge_counter    coulomb counter since PMIC power-on (uAh, signed)
 *   charge_full is writable (userspace can restore a learned value).
 *
 * State of charge ("charge_now", 0 .. charge_full):
 *  - Start: estimated OCV (V - I*R) through the vendor 25 C OCV curve
 *    (gemini-linux-kernel-3.18 battery_profile_t2), rescaled so that
 *    "full at our charge voltage" (full_ocv_mv) = 100 %.
 *  - Then pure coulomb counting (CAR deltas).
 *  - Full: VBUS present, cell current tapered below full_ma, voltage at
 *    least full_mv, for two polls -> status Full, charge_now = charge_full.
 *  - charge_now is writable: battery-guard restores the charge saved
 *    before a power-off (the start estimate under boot load is poor).
 *  - Near empty (estimated OCV < 3.70 V, where the curve is steep, and the
 *    current below steep_max_ma so the estimate is trustworthy): pull
 *    charge_now toward the curve by at most 1 % of charge_full per poll.
 *  - charge_full learning: after a Full anchor, the first steep-region
 *    reading with >= 40 % of charge_full counted out gives an estimate
 *    (blended 1/4 into charge_full, bounded 2000..4600 mAh).
 *  - Displayed %: 99 % max on the charger until the real Full event.
 *    Safety: on battery, an IR-compensated voltage (V + |I| x r_bat_mohm,
 *    the same estimate as ocv_mv) below vlow_mv (3450) for two polls caps
 *    the % at 5; below v0_mv (3400) it reads 0 (Critical). A raw loaded
 *    voltage below vfloor_mv (3100) counts too, whatever the current.
 *    (Raw loaded voltage alone tripped the 5 % cap at 68 % under a ~1.2 A
 *    load on the worn cell, 2026-10-08.)
 *  - OCV estimates use r_bat_mohm on battery and the larger r_chg_mohm
 *    while charging (polarization), so a start on the charger is not
 *    over-optimistic.
 *
 * Development build: loads like mt6351-probe — no DT node; finds the
 * pwrap device and uses its MT6351 regmap. Default scope "Device";
 * system=1 makes it the System battery for UPower/Plasma.
 *
 * Register writes (same as the vendor driver, nothing else):
 *   FGADC_CON0[15:8]  FG read-latch handshake (never resets counters)
 *   AUXADC_RQST0_SET  bit 0: request a BATSNS conversion
 * Do not load together with mt6351-probe latch/adc modes.
 *
 * Calibration (vendor mt_battery_meter.h): R_FG 10 mOhm, CAR_TUNE 115.
 * CAR: 35-bit two's complement CON1:CON2:CON3[2:0], full LSB
 * 359.86 uAh / 2^14 / OSR(8) at a 20 mOhm basis. NTER runs at 16 Hz.
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define MT6351_FGADC_CON(n)	(0x0CA4 + 2 * (n))
#define MT6351_AUXADC_ADC(n)	(0x0E00 + 2 * (n))
#define MT6351_AUXADC_RQST0_SET	0x0E98
#define MT6351_CHR_CON0		0x0F78
#define CHR_CON0_RGS_CHRDET	BIT(5)

#define FG_SW_READ_PRE		BIT(9)
#define FG_LATCHDATA_ST		BIT(10)
#define FG_SW_CLEAR		BIT(11)

#define ADC_CH0_BY_AP		23	/* BATSNS result, AP-requested copy */

#define UNIT_FGCURRENT_NA	158122LL
#define UNIT_FGCAR_NAH_17	359860LL
#define FG_OSR			8
#define NTER_HZ			16
#define VFULL_MV		1800
#define NOMINAL_MV		3850	/* energy = charge * nominal voltage */
#define FCC_MIN_UAH		2000000
#define FCC_MAX_UAH		4600000
#define DESIGN_UAH		4220000

/* ---- parameters ------------------------------------------------------ */
static unsigned int poll_ms = 10000;
module_param(poll_ms, uint, 0444);
MODULE_PARM_DESC(poll_ms, "sampling period (ms, default 10000)");

static unsigned int r_fg_mohm = 10;
module_param(r_fg_mohm, uint, 0444);
MODULE_PARM_DESC(r_fg_mohm, "fuel-gauge sense resistor (mOhm, vendor 10)");

static unsigned int car_tune = 115;
module_param(car_tune, uint, 0444);
MODULE_PARM_DESC(car_tune, "board current/charge tuning in percent (vendor 115)");

static bool system_scope;
module_param_named(system, system_scope, bool, 0444);
MODULE_PARM_DESC(system, "1 = register as the System battery (default: Device scope)");

static unsigned int fcc_mah = 2600;
module_param(fcc_mah, uint, 0444);
MODULE_PARM_DESC(fcc_mah, "initial full-charge capacity at our charge voltage (mAh, default 2600 = ~3.0 Ah cell estimate x ~85 percent reachable at VREG 4.208 V; learned afterwards)");

static unsigned int r_bat_mohm = 280;
module_param(r_bat_mohm, uint, 0644);
MODULE_PARM_DESC(r_bat_mohm, "battery path resistance for OCV = V - I*R (mOhm, measured 280)");

static unsigned int r_chg_mohm = 500;
module_param(r_chg_mohm, uint, 0644);
MODULE_PARM_DESC(r_chg_mohm, "effective resistance while CHARGING (ohmic + polarization), for OCV estimates on the charger (mOhm, default 500; on glass 280 under-corrected by ~15 points)");

static unsigned int full_ocv_mv = 4180;
module_param(full_ocv_mv, uint, 0444);
MODULE_PARM_DESC(full_ocv_mv, "resting voltage of a cell charged full at our VREG (mV) = 100 %");

static unsigned int full_ma = 200;
module_param(full_ma, uint, 0644);
MODULE_PARM_DESC(full_ma, "Full when VBUS present and cell charge current below this (mA)");

static unsigned int full_mv = 4150;
module_param(full_mv, uint, 0644);
MODULE_PARM_DESC(full_mv, "...and battery voltage at least this (mV)");

static unsigned int chg_thresh_ma = 30;
module_param(chg_thresh_ma, uint, 0644);
MODULE_PARM_DESC(chg_thresh_ma, "|current| above this = Charging/Discharging while on VBUS (mA)");

static unsigned int idle_drain_ma = 300;
module_param(idle_drain_ma, uint, 0644);
MODULE_PARM_DESC(idle_drain_ma, "on VBUS, a cell drain smaller than this is the charger idling after termination (loads on the BQ BAT node), not real discharging (mA)");

static unsigned int steep_max_ma = 700;
module_param(steep_max_ma, uint, 0644);
MODULE_PARM_DESC(steep_max_ma, "near-empty correction only while the battery current is below this (mA, default 700; blanked idle is ~550): under heavier load the OCV estimate of the worn cell reads too low");

static unsigned int vlow_mv = 3450;
module_param(vlow_mv, uint, 0644);
MODULE_PARM_DESC(vlow_mv, "on battery: IR-compensated voltage below this (2 polls) caps the percentage at 5 (mV, default 3450)");

static unsigned int vfloor_mv = 3100;
module_param(vfloor_mv, uint, 0644);
MODULE_PARM_DESC(vfloor_mv, "on battery: a RAW loaded voltage below this counts as empty for the caps even under load (mV, default 3100)");

static unsigned int v0_mv = 3400;
module_param(v0_mv, uint, 0644);
MODULE_PARM_DESC(v0_mv, "on battery: IR-compensated voltage below this (2 polls) reads 0 percent, Critical (mV, default 3400)");

/* ---- vendor 25 C OCV profile (battery_profile_t2): {DOD %, mV} --------- */
static const u16 ocv_dod[][2] = {
	{0, 4357}, {1, 4344}, {2, 4330}, {4, 4316}, {5, 4301}, {6, 4287}, {7, 4274},
	{8, 4261}, {9, 4248}, {11, 4235}, {12, 4222}, {13, 4209}, {14, 4196},
	{15, 4183}, {16, 4170}, {18, 4157}, {19, 4144}, {20, 4132}, {21, 4119},
	{22, 4107}, {23, 4094}, {25, 4082}, {26, 4071}, {27, 4064}, {28, 4057},
	{29, 4043}, {30, 4023}, {32, 4004}, {33, 3989}, {34, 3978}, {35, 3970},
	{36, 3964}, {37, 3960}, {39, 3952}, {40, 3941}, {41, 3927}, {42, 3913},
	{43, 3900}, {44, 3888}, {46, 3879}, {47, 3870}, {48, 3863}, {49, 3856},
	{50, 3849}, {51, 3843}, {53, 3837}, {54, 3832}, {55, 3826}, {56, 3821},
	{57, 3816}, {58, 3812}, {60, 3807}, {61, 3803}, {62, 3800}, {63, 3795},
	{64, 3792}, {65, 3789}, {67, 3785}, {68, 3782}, {69, 3779}, {70, 3776},
	{71, 3772}, {72, 3767}, {74, 3764}, {75, 3760}, {76, 3756}, {77, 3751},
	{78, 3747}, {79, 3743}, {81, 3739}, {82, 3734}, {83, 3728}, {84, 3721},
	{85, 3715}, {86, 3709}, {88, 3700}, {89, 3691}, {90, 3688}, {91, 3687},
	{92, 3686}, {93, 3684}, {95, 3681}, {96, 3669}, {97, 3628}, {98, 3568},
	{99, 3483}, {100, 3400},
};

/* vendor state of charge in 0.01 % for a resting voltage */
static int vendor_soc_x100(int mv)
{
	int i;

	if (mv >= ocv_dod[0][1])
		return 10000;
	for (i = 0; i < ARRAY_SIZE(ocv_dod) - 1; i++) {
		int v_hi = ocv_dod[i][1], v_lo = ocv_dod[i + 1][1];

		if (mv <= v_hi && mv >= v_lo) {
			int d_hi = ocv_dod[i][0] * 100, d_lo = ocv_dod[i + 1][0] * 100;
			int dod = d_hi + (v_hi - mv) * (d_lo - d_hi) / (v_hi - v_lo);

			return 10000 - dod;
		}
	}
	return 0;
}

/* our scale: fraction (0..10000) of "full at our VREG" */
static int our_soc_x100(int ocv_mv)
{
	int full = vendor_soc_x100(full_ocv_mv);
	int s = vendor_soc_x100(ocv_mv);

	if (full <= 0)
		return 0;
	return clamp(s * 10000 / full, 0, 10000);
}

/* ---- device state ------------------------------------------------------ */
struct fg_sample {
	s64 car_nah;
	u64 nter;
	int cur_ua;
};

struct mt6351_gauge {
	struct platform_device *pwrap_pdev;
	struct regmap *pmic;
	struct power_supply *psy;
	struct delayed_work work;
	struct mutex lock;

	struct fg_sample last;
	bool have_last;
	struct fg_sample win_start;
	bool have_win;
	int cur_avg_ua;
	bool have_avg;

	/* state of charge */
	bool soc_init;
	s64 charge_nah;		/* 0 .. fcc */
	s64 fcc_nah;
	s64 car_prev;		/* CAR at last integration */
	int vbat_mv;		/* last poll */
	int ocv_mv;
	bool vbus;
	bool full;
	int full_hits;
	int low_hits, zero_hits;
	bool have_full_ref;
	s64 car_at_full;
	int disp_pct;		/* what we report */
	int status;
};

static struct mt6351_gauge *gg;

static int rd(struct mt6351_gauge *g, unsigned int reg, unsigned int *val)
{
	int ret = regmap_read(g->pmic, reg, val);

	if (ret)
		*val = 0;
	return ret;
}

static s64 scale_tune(s64 v)
{
	return div_s64(v * 20 * car_tune, (s64)r_fg_mohm * 100);
}

static int fg_sample_locked(struct mt6351_gauge *g, struct fg_sample *out)
{
	unsigned int v = 0, c1, c2, c3, n4, n5, n6, cur;
	s64 car;
	int i, ret;

	ret = regmap_update_bits(g->pmic, MT6351_FGADC_CON(0), 0xFF00, FG_SW_READ_PRE);
	if (ret)
		return ret;
	for (i = 0; i < 1000; i++) {
		rd(g, MT6351_FGADC_CON(0), &v);
		if (v & FG_LATCHDATA_ST)
			break;
		udelay(10);
	}
	if (!(v & FG_LATCHDATA_ST)) {
		ret = -ETIMEDOUT;
		goto clear;
	}

	ret = rd(g, MT6351_FGADC_CON(1), &c1) ?: rd(g, MT6351_FGADC_CON(2), &c2) ?:
	      rd(g, MT6351_FGADC_CON(3), &c3) ?: rd(g, MT6351_FGADC_CON(4), &n4) ?:
	      rd(g, MT6351_FGADC_CON(5), &n5) ?: rd(g, MT6351_FGADC_CON(6), &n6) ?:
	      rd(g, MT6351_FGADC_CON(11), &cur);
	if (ret)
		goto clear;

	car = ((s64)c1 << 19) | ((s64)c2 << 3) | (c3 & 7);
	if (car & (1LL << 34))
		car -= 1LL << 35;
	out->car_nah = scale_tune(div_s64(car * UNIT_FGCAR_NAH_17, 16384 * FG_OSR));
	out->nter = ((u64)n4 << 17) | ((u64)n5 << 1) | (n6 & 1);
	out->cur_ua = (int)div_s64(scale_tune((s64)(s16)cur * UNIT_FGCURRENT_NA), 1000);

clear:
	regmap_update_bits(g->pmic, MT6351_FGADC_CON(0), 0xFF00, FG_SW_CLEAR);
	for (i = 0; i < 1000; i++) {
		rd(g, MT6351_FGADC_CON(0), &v);
		if (!(v & FG_LATCHDATA_ST))
			break;
		udelay(10);
	}
	regmap_update_bits(g->pmic, MT6351_FGADC_CON(0), 0xFF00, 0);
	return ret;
}

static int adc_vbat_mv_locked(struct mt6351_gauge *g)
{
	unsigned int v = 0;
	int i, ret;

	ret = regmap_write(g->pmic, MT6351_AUXADC_RQST0_SET, BIT(0));
	if (ret)
		return ret;
	for (i = 0; i < 40; i++) {
		usleep_range(1400, 1600);
		ret = rd(g, MT6351_AUXADC_ADC(ADC_CH0_BY_AP), &v);
		if (ret)
			return ret;
		if (v & 0x8000)
			return (v & 0x7FFF) * 3 * VFULL_MV / 32768;
	}
	return -ETIMEDOUT;
}

static bool vbus_present(struct mt6351_gauge *g)
{
	unsigned int v;

	return !rd(g, MT6351_CHR_CON0, &v) && (v & CHR_CON0_RGS_CHRDET);
}

static int refresh_locked(struct mt6351_gauge *g)
{
	struct fg_sample s;
	int ret = fg_sample_locked(g, &s);

	if (ret)
		return ret;
	g->last = s;
	g->have_last = true;
	return 0;
}

static int cur_best(struct mt6351_gauge *g)
{
	return g->have_avg ? g->cur_avg_ua : g->last.cur_ua;
}

static int compute_status(struct mt6351_gauge *g)
{
	int ua = cur_best(g);

	if (!g->vbus)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	if (g->full)
		return POWER_SUPPLY_STATUS_FULL;
	if (ua > (int)chg_thresh_ma * 1000)
		return POWER_SUPPLY_STATUS_CHARGING;
	/* after the BQ terminates, ~100 mA of board load sits on the battery
	 * side of the sense resistor until it recharges: that is "Not
	 * charging", not discharging */
	if (ua < -(int)idle_drain_ma * 1000)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static int pct_of(struct mt6351_gauge *g)
{
	if (g->fcc_nah <= 0)
		return 0;
	return (int)clamp_t(s64, div64_s64(g->charge_nah * 100 + g->fcc_nah / 2, g->fcc_nah), 0, 100);
}

/* one state-of-charge step; caller holds g->lock and has a fresh sample */
static void soc_update_locked(struct mt6351_gauge *g)
{
	int ua = cur_best(g);
	int pct;

	g->vbus = vbus_present(g);
	g->vbat_mv = adc_vbat_mv_locked(g);
	if (g->vbat_mv < 0)
		return;	/* keep the previous state; try again next poll */
	/* OCV = V - I*R  (I > 0 charging): uA * mOhm / 1e6 = mV. While
	 * charging, polarization adds to the ohmic drop -> larger R. */
	g->ocv_mv = g->vbat_mv - (int)div_s64((s64)ua * (ua > 0 ? r_chg_mohm : r_bat_mohm), 1000000);

	if (!g->soc_init) {
		g->charge_nah = div_s64(g->fcc_nah * our_soc_x100(g->ocv_mv), 10000);
		g->car_prev = g->last.car_nah;
		g->soc_init = true;
		pr_info("mt6351_gauge: initial SOC %d%% (V %d mV, I %d mA, est. OCV %d mV, FCC %lld mAh)\n",
			pct_of(g), g->vbat_mv, ua / 1000, g->ocv_mv, div_s64(g->fcc_nah, 1000000));
	} else {
		g->charge_nah += g->last.car_nah - g->car_prev;
		g->car_prev = g->last.car_nah;
	}

	/* ---- full detection ---- */
	if (g->vbus && ua > -(int)idle_drain_ma * 1000 && ua < (int)full_ma * 1000 &&
	    g->vbat_mv >= (int)full_mv) {
		if (!g->full && ++g->full_hits >= 2) {
			g->full = true;
			g->charge_nah = g->fcc_nah;
			g->car_at_full = g->last.car_nah;
			g->have_full_ref = true;
			pr_info("mt6351_gauge: FULL (V %d mV, I %d mA) -> 100%%, learning reference set\n",
				g->vbat_mv, ua / 1000);
		}
	} else {
		g->full_hits = 0;
		/* leave Full on unplug, on recharge, or when the load drains the cell */
		if (g->full && (!g->vbus || ua > ((int)full_ma + 150) * 1000 ||
				ua < -(int)idle_drain_ma * 1000))
			g->full = false;
	}
	if (g->full)
		g->charge_nah = g->fcc_nah;

	/* ---- steep-region correction + capacity learning (on battery) ---- */
	if (!g->vbus && g->ocv_mv < 3700 && abs(ua) <= (int)steep_max_ma * 1000) {
		s64 target = div_s64(g->fcc_nah * our_soc_x100(g->ocv_mv), 10000);
		s64 step = div_s64(g->fcc_nah, 100);
		s64 diff = target - g->charge_nah;

		if (g->have_full_ref) {
			s64 out = g->car_at_full - g->last.car_nah;	/* counted out */
			int soc = our_soc_x100(g->ocv_mv);

			if (out >= div_s64(g->fcc_nah * 40, 100) && soc > 200 && soc < 3000) {
				s64 est = div_s64(out * 10000, 10000 - soc);
				s64 old = g->fcc_nah;

				est = clamp_t(s64, est, (s64)FCC_MIN_UAH * 1000, (s64)FCC_MAX_UAH * 1000);
				g->fcc_nah = div_s64(old * 3 + est, 4);
				g->have_full_ref = false;
				pr_info("mt6351_gauge: capacity estimate %lld mAh (out %lld mAh, OCV %d mV = %d.%02d%%) -> charge_full %lld -> %lld mAh\n",
					div_s64(est, 1000000), div_s64(out, 1000000), g->ocv_mv,
					soc / 100, soc % 100, div_s64(old, 1000000),
					div_s64(g->fcc_nah, 1000000));
			}
		}
		if (diff > div_s64(g->fcc_nah * 5, 100) || diff < -div_s64(g->fcc_nah * 5, 100))
			g->charge_nah += clamp_t(s64, diff, -step, step);
	}

	/* count ran dry while the voltage says there is plenty left: charge_full
	 * is too small (or the start estimate was off) -> re-anchor to the curve
	 * rather than report a false 0 % that would shut the device down */
	if (!g->vbus && g->charge_nah <= div_s64(g->fcc_nah * 2, 100) &&
	    our_soc_x100(g->ocv_mv) > 1500) {
		g->charge_nah = div_s64(g->fcc_nah * our_soc_x100(g->ocv_mv), 10000);
		pr_warn("mt6351_gauge: count reached ~0 but OCV %d mV says %d%% -> re-anchored (charge_full %lld mAh may be too low)\n",
			g->ocv_mv, our_soc_x100(g->ocv_mv) / 100, div_s64(g->fcc_nah, 1000000));
	}

	g->charge_nah = clamp_t(s64, g->charge_nah, 0, g->fcc_nah);

	/* ---- displayed % with loaded-voltage safety caps ---- */
	pct = pct_of(g);
	/* never show 100 % on the charger until the real Full event */
	if (g->vbus && !g->full && pct > 99)
		pct = 99;
	if (!g->vbus) {
		/* compensated: ocv_mv = V - I*r_bat (I < 0 on battery) */
		bool raw_empty = g->vbat_mv < (int)vfloor_mv;

		g->low_hits = (g->ocv_mv < (int)vlow_mv || raw_empty) ? g->low_hits + 1 : 0;
		g->zero_hits = (g->ocv_mv < (int)v0_mv || raw_empty) ? g->zero_hits + 1 : 0;
		if (g->zero_hits >= 2)
			pct = 0;
		else if (g->low_hits >= 2)
			pct = min(pct, 5);
	} else {
		g->low_hits = g->zero_hits = 0;
	}
	g->disp_pct = pct;
	g->status = compute_status(g);
}

static void gauge_work(struct work_struct *w)
{
	struct mt6351_gauge *g = container_of(to_delayed_work(w), struct mt6351_gauge, work);
	int old_pct, old_status, changed = 0;

	mutex_lock(&g->lock);
	old_pct = g->disp_pct;
	old_status = g->status;
	if (!refresh_locked(g)) {
		if (g->have_win && g->last.nter > g->win_start.nter) {
			s64 dq = g->last.car_nah - g->win_start.car_nah;
			u64 dt = g->last.nter - g->win_start.nter;

			g->cur_avg_ua = (int)div64_s64(dq * 3600 * NTER_HZ, (s64)dt * 1000);
			g->have_avg = true;
		}
		g->win_start = g->last;
		g->have_win = true;
		soc_update_locked(g);
		changed = g->disp_pct != old_pct || g->status != old_status;
	}
	mutex_unlock(&g->lock);

	if (changed)
		power_supply_changed(g->psy);
	schedule_delayed_work(&g->work, msecs_to_jiffies(poll_ms));
}

static enum power_supply_property gauge_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int gauge_get_property(struct power_supply *psy, enum power_supply_property psp,
			      union power_supply_propval *val)
{
	struct mt6351_gauge *g = power_supply_get_drvdata(psy);
	int ret = 0, mv;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = system_scope ? POWER_SUPPLY_SCOPE_SYSTEM : POWER_SUPPLY_SCOPE_DEVICE;
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "MT6351 fuel gauge (dev)";
		return 0;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "MediaTek";
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = DESIGN_UAH;
		return 0;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		val->intval = (int)div_s64((s64)DESIGN_UAH * NOMINAL_MV, 1000);
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = 3400000;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = 4350000;
		return 0;
	default:
		break;
	}

	mutex_lock(&g->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = g->soc_init ? g->status : POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (g->soc_init)
			val->intval = g->disp_pct;
		else
			ret = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		if (!g->soc_init)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
		else if (g->full)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		else if (g->disp_pct <= 3)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		else if (g->disp_pct <= 10)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:	/* consistent with the displayed % */
		val->intval = (int)div_s64(g->fcc_nah * g->disp_pct, 100 * 1000);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = (int)div_s64(g->fcc_nah, 1000);
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		val->intval = (int)div_s64(div_s64(g->fcc_nah * g->disp_pct, 100 * 1000) * NOMINAL_MV, 1000);
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL:
		val->intval = (int)div_s64(div_s64(g->fcc_nah, 1000) * NOMINAL_MV, 1000);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		mv = adc_vbat_mv_locked(g);
		if (mv < 0)
			ret = mv;
		else
			val->intval = mv * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		if (g->soc_init)
			val->intval = g->ocv_mv * 1000;
		else
			ret = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = refresh_locked(g);
		if (!ret)
			val->intval = g->last.cur_ua;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		if (g->have_avg)
			val->intval = g->cur_avg_ua;
		else
			ret = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		if (g->vbat_mv > 0)
			val->intval = (int)div_s64((s64)g->vbat_mv * abs(cur_best(g)), 1000);
		else
			ret = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		ret = refresh_locked(g);
		if (!ret)
			val->intval = (int)div_s64(g->last.car_nah, 1000);
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&g->lock);
	return ret;
}

static int gauge_set_property(struct power_supply *psy, enum power_supply_property psp,
			      const union power_supply_propval *val)
{
	struct mt6351_gauge *g = power_supply_get_drvdata(psy);

	if (psp == POWER_SUPPLY_PROP_CHARGE_NOW) {
		/* userspace restores the charge saved before a power-off
		 * (battery-guard): the start estimate under boot load is poor */
		if (val->intval < 0)
			return -ERANGE;
		mutex_lock(&g->lock);
		g->charge_nah = clamp_t(s64, (s64)val->intval * 1000, 0, g->fcc_nah);
		g->disp_pct = pct_of(g);
		mutex_unlock(&g->lock);
		pr_info("mt6351_gauge: charge_now set to %d mAh (%d%%)\n", val->intval / 1000, g->disp_pct);
		power_supply_changed(psy);
		return 0;
	}
	if (psp != POWER_SUPPLY_PROP_CHARGE_FULL)
		return -EINVAL;
	if (val->intval < FCC_MIN_UAH || val->intval > FCC_MAX_UAH)
		return -ERANGE;

	mutex_lock(&g->lock);
	/* keep the same fraction of charge when the scale changes */
	if (g->fcc_nah > 0) {
		s64 frac = div64_s64(g->charge_nah * 10000, g->fcc_nah);	/* 0..10000 */

		g->charge_nah = div_s64((s64)val->intval * 1000 * frac, 10000);
	}
	g->fcc_nah = (s64)val->intval * 1000;
	mutex_unlock(&g->lock);
	pr_info("mt6351_gauge: charge_full set to %d mAh\n", val->intval / 1000);
	power_supply_changed(psy);
	return 0;
}

static int gauge_writeable(struct power_supply *psy, enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CHARGE_FULL || psp == POWER_SUPPLY_PROP_CHARGE_NOW;
}

static const struct power_supply_desc gauge_desc = {
	.name			= "mt6351-battery",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.properties		= gauge_props,
	.num_properties		= ARRAY_SIZE(gauge_props),
	.get_property		= gauge_get_property,
	.set_property		= gauge_set_property,
	.property_is_writeable	= gauge_writeable,
};

static int __init mt6351_gauge_init(void)
{
	struct power_supply_config cfg = {};
	struct device_node *np;
	struct mt6351_gauge *g;
	int ret, mv;

	if (!r_fg_mohm || !car_tune || !poll_ms ||
	    fcc_mah * 1000 < FCC_MIN_UAH || fcc_mah * 1000 > FCC_MAX_UAH)
		return -EINVAL;

	g = kzalloc(sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;
	mutex_init(&g->lock);
	g->status = -1;
	g->disp_pct = -1;
	g->fcc_nah = (s64)fcc_mah * 1000000;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-pwrap");
	if (!np) {
		ret = -ENODEV;
		goto err_free;
	}
	g->pwrap_pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!g->pwrap_pdev) {
		ret = -ENODEV;
		goto err_free;
	}
	g->pmic = dev_get_regmap(&g->pwrap_pdev->dev, NULL);
	if (!g->pmic) {
		pr_err("mt6351_gauge: pwrap has no regmap\n");
		ret = -ENODEV;
		goto err_put;
	}

	mutex_lock(&g->lock);
	ret = refresh_locked(g);
	mv = adc_vbat_mv_locked(g);
	if (!ret && mv > 0) {
		g->win_start = g->last;
		g->have_win = true;
		soc_update_locked(g);	/* initial SOC from est. OCV */
	}
	mutex_unlock(&g->lock);
	if (ret) {
		pr_err("mt6351_gauge: FG latch failed (%d) — is the FGADC running?\n", ret);
		goto err_put;
	}
	if (mv < 0) {
		pr_err("mt6351_gauge: AUXADC BATSNS request failed (%d)\n", mv);
		ret = mv;
		goto err_put;
	}

	cfg.drv_data = g;
	g->psy = power_supply_register(&g->pwrap_pdev->dev, &gauge_desc, &cfg);
	if (IS_ERR(g->psy)) {
		ret = PTR_ERR(g->psy);
		goto err_put;
	}

	gg = g;
	INIT_DELAYED_WORK(&g->work, gauge_work);
	schedule_delayed_work(&g->work, msecs_to_jiffies(poll_ms));

	pr_info("mt6351_gauge: registered %s (scope %s): VBAT %d mV, I %d mA, CAR %lld mAh, %llu s since PMIC power-on\n",
		gauge_desc.name, system_scope ? "System" : "Device", mv,
		g->last.cur_ua / 1000, div_s64(g->last.car_nah, 1000000),
		div_u64(g->last.nter, NTER_HZ));
	return 0;

err_put:
	put_device(&g->pwrap_pdev->dev);
err_free:
	kfree(g);
	return ret;
}

static void __exit mt6351_gauge_exit(void)
{
	struct mt6351_gauge *g = gg;

	cancel_delayed_work_sync(&g->work);
	power_supply_unregister(g->psy);
	put_device(&g->pwrap_pdev->dev);
	kfree(g);
}

module_init(mt6351_gauge_init);
module_exit(mt6351_gauge_exit);
MODULE_DESCRIPTION("Gemini PDA: MT6351 fuel gauge power_supply (development)");
MODULE_LICENSE("GPL");
