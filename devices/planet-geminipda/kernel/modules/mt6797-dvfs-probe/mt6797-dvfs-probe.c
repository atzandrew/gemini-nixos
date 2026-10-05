// SPDX-License-Identifier: GPL-2.0
/*
 * mt6797-dvfs-probe.c — READ-ONLY look at the MT6797 CPU clock / voltage
 * state that LK (and ATF) left behind, before we write any cpufreq driver.
 *
 * Project: docs/cpu-dvfs.md (gemini-nixos), step 1 "find out what the
 * clocks and voltages are right now". Vendor receipts:
 * gemian/gemini-linux-kernel-3.18
 *   drivers/misc/mediatek/base/power/mt6797/mt_cpufreq.c   (OPP tables, PLL math)
 *   drivers/misc/mediatek/base/power/mt6797/mt_cpufreq_hybrid.c (CSPM regs)
 *   drivers/misc/mediatek/base/power/mt6797/mt_idvfs.c     (A72 PLL via SMC)
 *   drivers/misc/mediatek/base/power/mt6797/mt_dcm.c       (sync DCM)
 *   drivers/misc/mediatek/freqhopping/mt6797/mt_freqhopping.c (0x1001A lock)
 *
 * What is read, in three opt-in stages (module parameters):
 *
 *   stage A (always): plain MMIO reads, nothing written.
 *     - CSPM (CPU DVFS processor, 0x11015000): POWERON_CONFIG_EN, PCM_CON0/1,
 *       PCM_FSM_STA (bit21 = PCM kicked = vendor DVFS firmware running),
 *       PCM_REG15 (PC), DVFS_CON, the MCU semaphore owners SEMA3_M0/M1/M2
 *       (M0 = kernel/FH, M1 = CSPM, M2 = ATF). Only POWERON_CONFIG_EN is
 *       read unless its bit0 (bus clock CG enable) is already set: the
 *       vendor never reads the rest of the block without it.
 *     - INFRACFG_AO CPULDO_CTRL_0..2 (0x10001F98..FA0) = the LL/L/CCI
 *       SRAM LDO ("VSRAM_L"), vendor set_cur_volt_sram_l().
 *     - TOPCKGEN CLK_MISC_CFG_0 (0x10000104).
 *     - MCUCFG sync-DCM config (0x10220744, 0x10222274).
 *
 *   stage B (mcu=1): the ARM PLL block MCUMIXEDSYS (0x1001A000) under the
 *     vendor's access protocol ("Everest 0x1001AXXX bus access issue": ATF,
 *     the CSPM and the kernel must not touch it concurrently). Protocol =
 *     vendor mt6797_0x1001AXXX_lock(): IRQs off, take HW semaphore 3 / M0
 *     (write 1 to 0x11015440, read bit0 back; 2 ms timeout -> give up, no
 *     BUG), 200 ns before each read, release (write 1 again). The only
 *     writes are those two semaphore writes. Reads: ARMCAXPLL0..3
 *     CON0/CON1/CON2/PWR_CON0 (LL, L, CCI, backup), ARMPLLDIV_MUXSEL,
 *     ARMPLLDIV_CKDIV, ARM_K1, MCU FHCTL (0x1001AF00..). Refuses if
 *     POWERON_CONFIG_EN bit0 is clear, unless cspm_cg=1 (then it does the
 *     vendor's own write 0x0b160001 to 0x11015000 first — the same write
 *     the vendor kernel does at core_initcall).
 *
 *   stage C (smc_b=1): A72 (cluster 2) PLL through the vendor's secure
 *     read SMC (MTK_SIP_KERNEL_IDVFS_READ 0xC200035F) of 0x102224a0
 *     (ARMPLL CON0: bit0 enable, 14:12 posdiv), 0x102224a4 (PCW) and
 *     0x102222b0 (A72 SRAM LDO vosel). The vendor only does this while
 *     cpu8 or cpu9 is online; so does this module (otherwise skipped).
 *
 * Output: everything to the kernel log (prefix "mt6797-dvfs:") at load,
 * and /sys/kernel/debug/mt6797_dvfs_probe/status re-reads on every cat
 * (stage selection = current parameter values, writable at runtime).
 *
 * Not read here (userspace does it, bin/cpu-clocks.sh): the DA9214 VPROC
 * bucks (I2C), the eFuse speed-bin words (/proc/device-tree/chosen/
 * atag,devinfo), measured per-CPU MHz (bin/cpumhz).
 */

#include <linux/arm-smccc.h>
#include <linux/cpumask.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>

static bool mcu;
module_param(mcu, bool, 0644);
MODULE_PARM_DESC(mcu, "stage B: read the 0x1001A ARM PLL block under the vendor HW-semaphore protocol");

static bool cspm_cg;
module_param(cspm_cg, bool, 0644);
MODULE_PARM_DESC(cspm_cg, "allow the vendor write 0x0b160001 -> 0x11015000 if CSPM bus CG is off (needed for the semaphore)");

static bool smc_b;
module_param(smc_b, bool, 0644);
MODULE_PARM_DESC(smc_b, "stage C: read the A72 PLL via the vendor iDVFS secure-read SMC (only while cpu8/9 online)");

/* ---- physical blocks ------------------------------------------------- */
#define CSPM_PHYS		0x11015000UL
#define MCUMIXED_PHYS		0x1001A000UL
#define INFRACFG_AO_PHYS	0x10001000UL
#define TOPCKGEN_PHYS		0x10000000UL
#define MCUCFG_PHYS		0x10220000UL
#define MCUCFG2_PHYS		0x10222000UL

/* CSPM (mt_cpufreq_hybrid.c) */
#define CSPM_POWERON_CONFIG_EN	0x000
#define CSPM_PCM_CON0		0x018
#define CSPM_PCM_CON1		0x01c
#define CSPM_PCM_REG15_DATA	0x13c
#define CSPM_PCM_FSM_STA	0x178
#define CSPM_DVFS_CON		0x400
#define CSPM_SEMA3_M0		0x440	/* MCU SEMA (FH = kernel) */
#define CSPM_SEMA3_M1		0x444	/* MCU SEMA (CPU SPM) */
#define CSPM_SEMA3_M2		0x448	/* MCU SEMA (ATF) */
#define CSPM_SW_RSV(n)		(0x608 + 4 * (n))
#define FSM_PCM_KICK		BIT(21)
#define CSPM_CG_KEY_EN		0x0b160001

/* MCUMIXEDSYS (mt_cpufreq.c / mt_clkmgr.h / mt_fhreg.h) */
#define ARMCAXPLL_CON0(n)	(0x200 + 0x10 * (n))
#define ARMCAXPLL_CON1(n)	(0x204 + 0x10 * (n))
#define ARMCAXPLL_CON2(n)	(0x208 + 0x10 * (n))
#define ARMCAXPLL_PWR_CON0(n)	(0x20c + 0x10 * (n))
#define ARMPLLDIV_MUXSEL	0x270
#define ARMPLLDIV_CKDIV		0x274
#define ARMPLLDIV_ARM_K1	0x27c
#define MCU_FHCTL		0xf00	/* HP_EN +0, CLK_CON +4, RST_CON +8, SLOPE0 +c, DSSC_CFG +10 */
#define MCU_FHCTL_CFG(n)	(0xf34 + 0x14 * (n))	/* CFG, UPDNLMT, DDS, DVFS, MON */

/* others */
#define CPULDO_CTRL(n)		(0xf98 + 4 * (n))	/* in INFRACFG_AO */
#define CLK_MISC_CFG_0		0x104			/* in TOPCKGEN */
#define MCUCFG_SYNC_DCM_CONFIG	0x744			/* in MCUCFG */
#define MCUCFG_SYNC_DCM_MP2	0x274			/* in MCUCFG2 */

/* A72 iDVFS secure read (mt_idvfs.h, ARM64 IDs) */
#define MTK_SIP_IDVFS_READ	0xC200035FUL
#define B_ARMPLL_CON0		0x102224a0UL
#define B_ARMPLL_CON1		0x102224a4UL
#define B_SRAMLDO		0x102222b0UL

static void __iomem *cspm, *mcumix, *infra, *topck, *mcucfg, *mcucfg2;
static DEFINE_SPINLOCK(probe_lock);
static DEFINE_MUTEX(status_lock);
static struct dentry *dbg_dir;

static const char *const cl_name[4] = { "LL (cpu0-3)", "L (cpu4-7)", "CCI", "backup" };

/* ---- helpers -------------------------------------------------------- */

static const char *muxsel_name(u32 v)
{
	switch (v & 3) {
	case 0: return "clksq(26M)";
	case 1: return "armpll";
	case 2: return "mainpll";
	default: return "univpll";
	}
}

/* ARMPLLDIV_CKDIV field -> ratio x1000 (vendor _cpu_freq_calc) */
static unsigned int ckdiv_x1000(u32 v)
{
	switch (v) {
	case 8: return 1000;
	case 9: return 750;
	case 10: return 500;
	case 11: return 250;
	case 17: return 800;
	case 18: return 600;
	case 19: return 400;
	default: return 1000;	/* vendor _cpu_freq_calc: any other code = /1 (LK leaves 0) */
	}
}

/* CPULDO 4-bit vosel -> mV (vendor get_cur_volt_sram_l) */
static unsigned int ldo_mv(u32 v)
{
	switch (v & 0xf) {
	case 0: return 1050;
	case 1: return 600;
	case 2: return 700;
	default: return 900 + ((v & 0xf) - 3) * 25;
	}
}

/* ---- stage A ---------------------------------------------------------- */

static void stage_a(struct seq_file *m)
{
	u32 en, v[3];
	int i;

	en = readl(cspm + CSPM_POWERON_CONFIG_EN);
	seq_printf(m, "CSPM POWERON_CONFIG_EN = 0x%08x (bus CG %s)\n", en,
		   (en & 1) ? "ON" : "OFF");
	if (en & 1) {
		u32 fsm = readl(cspm + CSPM_PCM_FSM_STA);

		seq_printf(m, "CSPM PCM_CON0 = 0x%08x  PCM_CON1 = 0x%08x  PCM_REG15(PC) = 0x%08x\n",
			   readl(cspm + CSPM_PCM_CON0), readl(cspm + CSPM_PCM_CON1),
			   readl(cspm + CSPM_PCM_REG15_DATA));
		seq_printf(m, "CSPM PCM_FSM_STA = 0x%08x -> vendor DVFS firmware %s\n", fsm,
			   (fsm & FSM_PCM_KICK) ? "RUNNING (PCM kicked)" : "not running");
		seq_printf(m, "CSPM DVFS_CON = 0x%08x  SW_RSV0..3 = %08x %08x %08x %08x\n",
			   readl(cspm + CSPM_DVFS_CON),
			   readl(cspm + CSPM_SW_RSV(0)), readl(cspm + CSPM_SW_RSV(1)),
			   readl(cspm + CSPM_SW_RSV(2)), readl(cspm + CSPM_SW_RSV(3)));
		seq_printf(m, "MCU semaphore 3: M0(kernel)=0x%x M1(CSPM)=0x%x M2(ATF)=0x%x\n",
			   readl(cspm + CSPM_SEMA3_M0), readl(cspm + CSPM_SEMA3_M1),
			   readl(cspm + CSPM_SEMA3_M2));
	} else {
		seq_puts(m, "CSPM: rest of block not read (vendor only accesses it with CG on)\n");
	}

	for (i = 0; i < 3; i++)
		v[i] = readl(infra + CPULDO_CTRL(i));
	seq_printf(m, "CPULDO_CTRL_0..2 = 0x%08x 0x%08x 0x%08x (enable mask 0x%02x)\n",
		   v[0], v[1], v[2], v[0] & 0xff);
	for (i = 0; i < 4; i++)
		seq_printf(m, "  VSRAM_L ldo%d: ctrl1 vosel %u = %u mV, ctrl2 vosel %u = %u mV\n", i,
			   (v[1] >> (8 * i)) & 0xf, ldo_mv(v[1] >> (8 * i)),
			   (v[2] >> (8 * i)) & 0xf, ldo_mv(v[2] >> (8 * i)));

	seq_printf(m, "TOPCKGEN CLK_MISC_CFG_0 = 0x%08x (bits5:4 = %u)\n",
		   readl(topck + CLK_MISC_CFG_0), (readl(topck + CLK_MISC_CFG_0) >> 4) & 3);
	seq_printf(m, "MCUCFG SYNC_DCM_CONFIG = 0x%08x  SYNC_DCM_MP2_CONFIG = 0x%08x\n",
		   readl(mcucfg + MCUCFG_SYNC_DCM_CONFIG), readl(mcucfg2 + MCUCFG_SYNC_DCM_MP2));
}

/* ---- stage B ---------------------------------------------------------- */

struct mcu_regs {
	u32 con0[4], con1[4], con2[4], pwr[4];
	u32 muxsel, ckdiv, k1;
	u32 fh[5], fhcfg[4][5];
	u32 sema_before, sema_after;
	int tries;
};

static inline u32 mrd(unsigned int off)
{
	ndelay(200);	/* vendor: "DE workaround, for first read after sequential write" */
	return readl(mcumix + off);
}

/* vendor mt6797_0x1001AXXX_get_semaphore(), without the BUG_ON */
static int sema_get(struct mcu_regs *r)
{
	int i;

	r->sema_before = readl(cspm + CSPM_SEMA3_M0);
	for (i = 0; i < 200; i++) {	/* 200 x 10 us = 2 ms, vendor SEMA_GET_TIMEOUT */
		writel(0x1, cspm + CSPM_SEMA3_M0);
		if (readl(cspm + CSPM_SEMA3_M0) & 0x1) {
			r->tries = i + 1;
			return 0;
		}
		udelay(10);
	}
	r->tries = i;
	return -EBUSY;
}

static void sema_release(struct mcu_regs *r)
{
	if (readl(cspm + CSPM_SEMA3_M0) & 0x1)
		writel(0x1, cspm + CSPM_SEMA3_M0);
	r->sema_after = readl(cspm + CSPM_SEMA3_M0);
}

static int read_mcu(struct mcu_regs *r)
{
	unsigned long flags;
	int i, j, ret;

	spin_lock_irqsave(&probe_lock, flags);
	ret = sema_get(r);
	if (ret) {
		spin_unlock_irqrestore(&probe_lock, flags);
		return ret;
	}
	for (i = 0; i < 4; i++) {
		r->con0[i] = mrd(ARMCAXPLL_CON0(i));
		r->con1[i] = mrd(ARMCAXPLL_CON1(i));
		r->con2[i] = mrd(ARMCAXPLL_CON2(i));
		r->pwr[i] = mrd(ARMCAXPLL_PWR_CON0(i));
	}
	r->muxsel = mrd(ARMPLLDIV_MUXSEL);
	r->ckdiv = mrd(ARMPLLDIV_CKDIV);
	r->k1 = mrd(ARMPLLDIV_ARM_K1);
	for (i = 0; i < 5; i++)
		r->fh[i] = mrd(MCU_FHCTL + 4 * i);
	for (i = 0; i < 4; i++)
		for (j = 0; j < 5; j++)
			r->fhcfg[i][j] = mrd(MCU_FHCTL_CFG(i) + 4 * j);
	sema_release(r);
	spin_unlock_irqrestore(&probe_lock, flags);
	return 0;
}

static void stage_b(struct seq_file *m)
{
	static struct mcu_regs r;	/* under status_lock */
	u32 en;
	int i, ret;

	en = readl(cspm + CSPM_POWERON_CONFIG_EN);
	if (!(en & 1)) {
		if (!cspm_cg) {
			seq_puts(m, "stage B skipped: CSPM bus CG is off; reload with cspm_cg=1 to allow the vendor CG write\n");
			return;
		}
		writel(CSPM_CG_KEY_EN, cspm + CSPM_POWERON_CONFIG_EN);
		seq_printf(m, "wrote 0x%08x to CSPM POWERON_CONFIG_EN (vendor CG enable); now 0x%08x\n",
			   CSPM_CG_KEY_EN, readl(cspm + CSPM_POWERON_CONFIG_EN));
	}

	memset(&r, 0, sizeof(r));
	ret = read_mcu(&r);
	seq_printf(m, "semaphore3/M0: before 0x%x, %s after %d tries, after release 0x%x\n",
		   r.sema_before, ret ? "NOT taken (timeout)" : "taken", r.tries, r.sema_after);
	if (ret)
		return;

	seq_printf(m, "ARMPLLDIV_MUXSEL = 0x%08x  CKDIV = 0x%08x  ARM_K1 = 0x%08x\n",
		   r.muxsel, r.ckdiv, r.k1);
	for (i = 0; i < 4; i++) {
		static const u8 mux_shift[4] = { 2, 4, 6, 0 };	/* LL, L, CCI, (B field) */
		static const u8 div_shift[4] = { 5, 10, 15, 0 };
		u32 dds = r.con1[i] & GENMASK(20, 0);
		u32 posdiv = 1u << ((r.con1[i] >> 24) & 7);
		u64 vco_khz = ((u64)dds * 26000) >> 14;
		u32 mux = (r.muxsel >> mux_shift[i]) & 3;
		u32 ckd = (r.ckdiv >> div_shift[i]) & 0x1f;
		unsigned int ratio = ckdiv_x1000(ckd);

		seq_printf(m, "ARMCAXPLL%d %-12s CON0=0x%08x CON1=0x%08x CON2=0x%08x PWR_CON0=0x%08x\n",
			   i, cl_name[i], r.con0[i], r.con1[i], r.con2[i], r.pwr[i]);
		seq_printf(m, "    en=%u dds=0x%06x vco=%llu kHz posdiv=/%u", r.con0[i] & 1, dds,
			   vco_khz, posdiv);
		if (i < 3)
			seq_printf(m, " mux=%u(%s) ckdiv=%u (x%u/1000) -> %llu kHz\n", mux,
				   muxsel_name(mux), ckd, ratio,
				   ratio ? div_u64(vco_khz * ratio, posdiv * 1000) : 0);
		else
			seq_puts(m, "\n");
	}
	seq_printf(m, "B (A72) mux field = %u(%s), ckdiv field = %u (x%u/1000) [B PLL itself: stage C]\n",
		   r.muxsel & 3, muxsel_name(r.muxsel), r.ckdiv & 0x1f, ckdiv_x1000(r.ckdiv & 0x1f));
	seq_printf(m, "MCU FHCTL HP_EN=0x%08x CLK_CON=0x%08x RST_CON=0x%08x SLOPE0=0x%08x DSSC_CFG=0x%08x\n",
		   r.fh[0], r.fh[1], r.fh[2], r.fh[3], r.fh[4]);
	for (i = 0; i < 4; i++)
		seq_printf(m, "  FHCTL%d CFG=0x%08x UPDNLMT=0x%08x DDS=0x%08x DVFS=0x%08x MON=0x%08x\n",
			   i, r.fhcfg[i][0], r.fhcfg[i][1], r.fhcfg[i][2], r.fhcfg[i][3], r.fhcfg[i][4]);
}

/* ---- stage C ---------------------------------------------------------- */

static unsigned long sec_read(unsigned long addr)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_IDVFS_READ, addr, 0, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static void stage_c(struct seq_file *m)
{
	unsigned long con0, con1, ldo;
	u64 vco_mhz;
	unsigned int posdiv;

	if (!cpu_online(8) && !cpu_online(9)) {
		seq_puts(m, "stage C skipped: cpu8/cpu9 offline (vendor reads the A72 PLL only while online)\n");
		return;
	}
	con0 = sec_read(B_ARMPLL_CON0);
	con1 = sec_read(B_ARMPLL_CON1);
	ldo = sec_read(B_SRAMLDO);
	vco_mhz = ((u64)(con1 & 0x7fffffff) * 26) >> 24;
	posdiv = 1u << ((con0 >> 12) & 7);
	seq_printf(m, "A72 PLL (SMC read): CON0=0x%08lx CON1(PCW)=0x%08lx SRAMLDO=0x%08lx\n",
		   con0, con1, ldo);
	seq_printf(m, "    en=%lu vco=%llu MHz posdiv=/%u -> %llu MHz (before the B ckdiv field above); VSRAM_B vosel %lu = %u mV\n",
		   con0 & 1, vco_mhz, posdiv, div_u64(vco_mhz, posdiv), ldo & 0xf, ldo_mv(ldo));
}

/* ---- output ------------------------------------------------------------ */

static int status_show(struct seq_file *m, void *v)
{
	mutex_lock(&status_lock);
	seq_printf(m, "online cpus: %*pbl\n", cpumask_pr_args(cpu_online_mask));
	stage_a(m);
	if (mcu)
		stage_b(m);
	else
		seq_puts(m, "stage B (0x1001A ARM PLLs) not run: mcu=0\n");
	if (smc_b)
		stage_c(m);
	else
		seq_puts(m, "stage C (A72 PLL via SMC) not run: smc_b=0\n");
	mutex_unlock(&status_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

/* print the same report to the kernel log once, at load */
static int log_report(void)
{
	char *buf;
	struct seq_file m = { };
	char *line, *p;

	buf = kzalloc(PAGE_SIZE * 2, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	m.buf = buf;
	m.size = PAGE_SIZE * 2;
	status_show(&m, NULL);
	p = buf;
	while ((line = strsep(&p, "\n")) != NULL)
		if (*line)
			pr_info("mt6797-dvfs: %s\n", line);
	kfree(buf);
	return 0;
}

static void unmap_all(void)
{
	if (cspm) iounmap(cspm);
	if (mcumix) iounmap(mcumix);
	if (infra) iounmap(infra);
	if (topck) iounmap(topck);
	if (mcucfg) iounmap(mcucfg);
	if (mcucfg2) iounmap(mcucfg2);
}

static int __init probe_init(void)
{
	cspm = ioremap(CSPM_PHYS, 0x1000);
	mcumix = ioremap(MCUMIXED_PHYS, 0x1000);
	infra = ioremap(INFRACFG_AO_PHYS, 0x1000);
	topck = ioremap(TOPCKGEN_PHYS, 0x1000);
	mcucfg = ioremap(MCUCFG_PHYS, 0x1000);
	mcucfg2 = ioremap(MCUCFG2_PHYS, 0x1000);
	if (!cspm || !mcumix || !infra || !topck || !mcucfg || !mcucfg2) {
		unmap_all();
		return -ENOMEM;
	}
	pr_info("mt6797-dvfs: read-only probe, mcu=%d cspm_cg=%d smc_b=%d\n", mcu, cspm_cg, smc_b);
	log_report();
	dbg_dir = debugfs_create_dir("mt6797_dvfs_probe", NULL);
	debugfs_create_file("status", 0444, dbg_dir, NULL, &status_fops);
	return 0;
}

static void __exit probe_exit(void)
{
	debugfs_remove_recursive(dbg_dir);
	unmap_all();
}

module_init(probe_init);
module_exit(probe_exit);
MODULE_DESCRIPTION("MT6797 CPU DVFS state probe (read-only; Gemini PDA)");
MODULE_LICENSE("GPL");
