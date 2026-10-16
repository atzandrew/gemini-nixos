// SPDX-License-Identifier: GPL-2.0
/*
 * wcn_hw_glue.c - hardware glue for the MediaTek MT6797 Wi-Fi port (B-21)
 *
 * The vendor WMT core (common_main) talks to the SoC through two interfaces:
 *   1. wmt_plat_*        - power / clock / GPIO / eirq / thermal / wake-lock
 *   2. mtk_wcn_cmb_hw_*  - combo (Wi-Fi/BT/GPS) power-on / off / reset / init
 *
 * In the vendor 3.18 tree these live in common_main/mt6797/ on top of MediaTek
 * SoC drivers (mt_clkmgr, emi_mpu, upmu, mtk_hibernate_dpm, mt_clkbuf_ctl) that
 * are not in mainline 6.6.
 *
 * On the Gemini PDA the CONSYS bring-up (power-on, MCU reset, ROM patch push,
 * BTIF/DMA transport) is ALREADY done directly by
 * drivers/soc/mediatek/mtk-consys-spike.c. This glue backs the wmt_plat_* /
 * mtk_wcn_cmb_hw_* surface so the WMT protocol core builds and can drive the
 * already-running CONSYS MCU.
 *
 * STATUS (B-21): the WMT protocol core now COMPILES and the module LINKS on
 * 6.6. The stubs below return success / no-op for now; each is filled in as the
 * spike transport + hooks are wired up (see docs/wifi-consys.md for the plan).
 * wmt_plat_read_cpupcr() is already backed by a real read (the spike's
 * 0x18070160 register) so the WMT core sees the true patched-CPUPCR value.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/firmware.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include <linux/consys_wmt.h>
#include <linux/debugfs.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>

#include "osal_typedef.h"
#include "osal.h"
#include "wmt_plat.h"
#include "mtk_wcn_consys_hw.h"
#include "stp_sdio.h"
#include "hif_sdio.h"
#include "sdio_detect.h"
#include <mt-plat/aee.h>
#include "mtk_btif_exp.h"
#include "wmt_core.h"
#include "wmt_lib.h"
#include "wmt_dev.h"

/* CONSYS (MT6630) chip-id / CPUPCR registers, shared with the spike. */
#define WCN_CONSYS_CHIPID_REG	0x18070008
#define WCN_CONSYS_CPUPCR_REG	0x18070160
#define WCN_CONSYS_CPUPCR_BARE	0x112c

static void __iomem *wcn_map_reg(u32 addr)
{
	return ioremap(addr, 4);
}

/* ==========================================================================
 * CONSYS shared-EMI window + EMI MPU (wlan driver imports)
 *
 * The vendor WMT core (common_main/mt6797/mtk_wcn_consys_hw.c) fills
 * gConEmiPhyBase from the "mediatek,consys-reserve-memory" reserved-memory
 * node (2 MB, alloc-ranges 0x40000000-0x80000000) and the wlan/gen3 stack
 * imports it to ioremap + MPU-protect the window for the Wi-Fi firmware's
 * EMI sections. Our dts does not (yet) carve out that region, so the base
 * starts at 0 and is assigned when the carve-out lands (wlan firmware
 * download will fail with a clear log until then).
 * ========================================================================== */
phys_addr_t gConEmiPhyBase; /* [port 6.6] vendor mtk_wcn_consys_hw.c:90 */
EXPORT_SYMBOL_GPL(gConEmiPhyBase);

int emi_mpu_set_region_protection(unsigned long long start_addr,
	unsigned long long end_addr, int region,
	unsigned int access_permission)
{
	/* [port 6.6] no EMI MPU driver in mainline; LK leaves regions open,
	 * so restriction writes are moot until a real firmware load. Log the
	 * first call per region so a later EMI issue points here. */
	pr_debug("wcn-glue: emi_mpu_set_region_protection(0x%llx-0x%llx, region %d, perm 0x%x)\n",
		 start_addr, end_addr, region, access_permission);
	return 0;
}
EXPORT_SYMBOL_GPL(emi_mpu_set_region_protection);

/* ==========================================================================
 * wlan-driver imports that live in the vendor WMT core / SoC layer
 * ========================================================================== */

/* The wlan/gen3 ahb HIF calls this when it (re)configures the Wi-Fi
 * datapath power (vendor mtk_wcn_consys_hw_wifi_paldo_ctrl -> VCN33 wifi
 * rail). B-30 wired the same rail into wmt_plat_soc_paldo_ctrl; forward via
 * the spike's consys_paldo_ctrl. */
INT32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT32 enable)
{
	pr_debug("wcn-glue: wifi_paldo_ctrl(%u)\n", enable);
	return consys_paldo_ctrl(WIFI_PALDO, enable ? 1 : 0);
}
EXPORT_SYMBOL_GPL(mtk_wcn_consys_hw_wifi_paldo_ctrl);

/* MediaTek's whole-chip wifi-reset + p2p-mode frameworks (exported by the
 * stock wmt_drv.ko to the wlan driver). No consumers exist on this port - the
 * spike owns CONSYS resets and P2P mode is driven by the supplicant through
 * cfg80211 - so these are logged no-ops that keep the wlan_gen3 module
 * linkable. */
int wifi_reset_start(void)
{
	pr_warn("wcn-glue: wifi_reset_start (no-op on this port)\n");
	return 0;
}
EXPORT_SYMBOL_GPL(wifi_reset_start);

void wifi_reset_end(int status)
{
	pr_warn("wcn-glue: wifi_reset_end(%d) (no-op on this port)\n", status);
}
EXPORT_SYMBOL_GPL(wifi_reset_end);

void register_set_p2p_mode_handler(void *handler)
{
	pr_debug("wcn-glue: register_set_p2p_mode_handler(%ps) (unused on this port)\n", handler);
}
EXPORT_SYMBOL_GPL(register_set_p2p_mode_handler);


/* ==========================================================================
 * mtk_wcn_cmb_hw_*  - combo power / reset / init
 * CONSYS is already powered and running (spike). No-ops that report success.
 * ========================================================================== */
INT32 mtk_wcn_cmb_hw_pwr_on(VOID)
{
	pr_info("wcn-glue: cmb_hw_pwr_on (CONSYS already up via spike)\n");
	return 0;
}

INT32 mtk_wcn_cmb_hw_pwr_off(VOID)
{
	pr_info("wcn-glue: cmb_hw_pwr_off (no-op)\n");
	return 0;
}

INT32 mtk_wcn_cmb_hw_rst(VOID)
{
	pr_info("wcn-glue: cmb_hw_rst (no-op; spike owns MCU reset)\n");
	return 0;
}

INT32 mtk_wcn_cmb_hw_init(P_PWR_SEQ_TIME pPwrSeqTime)
{
	pr_info("wcn-glue: cmb_hw_init\n");
	return 0;
}

INT32 mtk_wcn_cmb_hw_deinit(VOID)
{
	pr_info("wcn-glue: cmb_hw_deinit\n");
	return 0;
}

INT32 mtk_wcn_cmb_hw_state_show(VOID)
{
	pr_info("wcn-glue: cmb_hw_state_show (CONSYS up)\n");
	return 0;
}

/* ==========================================================================
 * wmt_plat_*  - power / clock / GPIO / eirq / thermal / wake-lock / misc
 * Stubs return success / no-op; filled in as the spike hooks are wired up.
 * ========================================================================== */
INT32 wmt_plat_init(P_PWR_SEQ_TIME pPwrSeqTime)
{
	return 0;
}

INT32 wmt_plat_soc_init(UINT32 co_clock_type)
{
	return 0;
}

INT32 wmt_plat_deinit(VOID)
{
	return 0;
}

INT32 wmt_plat_merge_if_flag_get(VOID)
{
	return 0;
}

INT32 wmt_plat_set_comm_if_type(ENUM_STP_TX_IF_TYPE type)
{
	return 0;
}

INT32 wmt_plat_merge_if_flag_ctrl(UINT32 enagle)
{
	return 0;
}

ENUM_STP_TX_IF_TYPE wmt_plat_get_comm_if_type(VOID)
{
	return STP_BTIF_IF_TX;
}

/* CONN domain power-off at WMT HW_PWR_OFF (connsys-sleep work, 2026-10-15).
 *
 * Stock mtk_wcn_consys_hw_reg_ctrl(0) turns the CONN MTCMOS off when the
 * WMT core powers the chip down (all functions off); this port's
 * HW_PWR_OFF was a no-op, so connsys kept CONN_SRCCLKENA (PCM_REG13 bit
 * 14) asserted forever. The spike already owns the CONN scpsys domain
 * through runtime PM on its device and drops it the same way for its
 * B-27 power cycle (measured: PWR_CON 0x10d -> 0x112, r13 bits 13/14/27
 * clear while off). Here: FUNC_OFF puts the spike's reference so the
 * domain powers off; FUNC_ON takes it back before the core's STP init,
 * whose BTIF hand-off runs B-27 (put/get) against a balanced count.
 * Test-gated by conn_pwr_off (default 0 = previous no-op behaviour). */
static bool conn_pwr_off;
module_param(conn_pwr_off, bool, 0644);
static bool wcn_conn_released;	/* we hold the spike's domain ref down */
#define WCN_SPM_CONN_PWR_CON	0x1000632C

static struct device *wcn_conn_dev(void)
{
	static struct device *dev;
	struct device_node *np;
	struct platform_device *pdev;

	if (dev)
		return dev;
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-consys");
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (pdev)
		dev = &pdev->dev;	/* keep the reference for the module's life */
	return dev;
}

static u32 wcn_reg32(u32 pa);

INT32 wmt_plat_pwr_ctrl(ENUM_FUNC_STATE state)
{
	struct device *dev;
	int ret;

	if (state == FUNC_OFF) {
		if (!conn_pwr_off || wcn_conn_released)
			return 0;
		dev = wcn_conn_dev();
		if (!dev) {
			pr_warn("wcn-glue: HW_PWR_OFF: consys device not found\n");
			return 0;
		}
		ret = pm_runtime_put_sync(dev);
		wcn_conn_released = true;
		pr_info("wcn-glue: HW_PWR_OFF: CONN domain put -> %d (suspended=%d PWR_CON=%#x)\n",
			ret, pm_runtime_suspended(dev),
			wcn_reg32(WCN_SPM_CONN_PWR_CON));
	} else if (state == FUNC_ON) {
		if (!wcn_conn_released)
			return 0;
		dev = wcn_conn_dev();
		if (!dev)
			return -1;
		ret = pm_runtime_get_sync(dev);
		wcn_conn_released = false;
		pr_info("wcn-glue: HW_PWR_ON: CONN domain get -> %d (PWR_CON=%#x)\n",
			ret, wcn_reg32(WCN_SPM_CONN_PWR_CON));
		if (ret < 0)
			return -1;
	}
	return 0;
}

INT32 wmt_plat_gpio_ctrl(ENUM_PIN_ID id, ENUM_PIN_STATE state)
{
	return 0;
}

INT32 wmt_plat_eirq_ctrl(ENUM_PIN_ID id, ENUM_PIN_STATE state)
{
	return 0;
}

INT32 wmt_plat_wake_lock_ctrl(ENUM_WL_OP opId)
{
	return 0;
}

INT32 wmt_plat_sdio_ctrl(UINT32 sdioPortNum, ENUM_FUNC_STATE on)
{
	return 0;
}

INT32 wmt_plat_audio_ctrl(CMB_STUB_AIF_X state, CMB_STUB_AIF_CTRL ctrl)
{
	return 0;
}

VOID wmt_plat_irq_cb_reg(irq_cb bgf_irq_cb)
{
}

VOID wmt_plat_aif_cb_reg(device_audio_if_cb aif_ctrl_cb)
{
}

VOID wmt_plat_func_ctrl_cb_reg(func_ctrl_cb subsys_func_ctrl)
{
}

VOID wmt_plat_thermal_ctrl_cb_reg(thermal_query_ctrl_cb thermal_query_ctrl)
{
}

VOID wmt_plat_deep_idle_ctrl_cb_reg(deep_idle_ctrl_cb deep_idle_ctrl)
{
}

INT32 wmt_plat_soc_paldo_ctrl(ENUM_PALDO_TYPE ePt, ENUM_PALDO_OP ePo)
{
	/* B-30: back the PALDO ops with the real VCN33 sub-rails (the spike
	 * holds them off until here - the RF-calibration exchange after the
	 * ROMv3 push needs the RF LDOs powered; without them the chip never
	 * answers the start-RF-cal command). BT rail for BT_PALDO, WiFi
	 * rail for WIFI_PALDO; the other types (FM/GPS/...) have no rail on
	 * this unit. */
	if (ePt == BT_PALDO || ePt == WIFI_PALDO) {
		int ret = consys_paldo_ctrl((int)ePt, ePo == PALDO_ON);

		if (ret) {
			pr_warn("wcn-glue: paldo type %d %s failed (%d)\n",
				ePt, ePo == PALDO_ON ? "on" : "off", ret);
			return -1;
		}
	}
	return 0;
}

UINT8 *wmt_plat_get_emi_virt_add(UINT32 offset)
{
	/* EMI shared-memory window; not used on the BTIF path yet. */
	return NULL;
}

UINT32 wmt_plat_jtag_flag_ctrl(UINT32 en)
{
	return 0;
}

P_CONSYS_EMI_ADDR_INFO wmt_plat_get_emi_phy_add(VOID)
{
	return NULL;
}

/* Real read: the CONSYS CPUPCR register (same the spike reads). Lets the WMT
 * core observe the true patched-CPUPCR (0x9b5d6) vs bare (0x112c). */
UINT32 wmt_plat_read_cpupcr(VOID)
{
	void __iomem *r;
	UINT32 v;

	if (wcn_conn_released)
		return WCN_CONSYS_CPUPCR_BARE;
	r = wcn_map_reg(WCN_CONSYS_CPUPCR_REG);
	if (!r)
		return WCN_CONSYS_CPUPCR_BARE;
	v = readl(r);
	iounmap(r);
	return v;
}
EXPORT_SYMBOL_GPL(wmt_plat_read_cpupcr);

UINT32 wmt_plat_read_dmaregs(UINT32 off)
{
	return 0;
}

INT32 wmt_plat_set_host_dump_state(ENUM_HOST_DUMP_STATE state)
{
	return 0;
}

UINT32 wmt_plat_force_trigger_assert(ENUM_FORCE_TRG_ASSERT_T type)
{
	return 0;
}

INT32 wmt_plat_update_host_sync_num(VOID)
{
	return 0;
}

INT32 wmt_plat_get_dump_info(UINT32 offset)
{
	return 0;
}

UINT32 wmt_plat_get_soc_chipid(VOID)
{
	/* MT6797 SoC id. The WMT detect layer uses this to pick the chip. */
	return 0x6797;
}

UINT32 wmt_plat_soc_co_clock_flag_get(VOID)
{
	return 0;
}

INT32 wmt_plat_set_dbg_mode(UINT32 flag)
{
	return 0;
}

VOID wmt_plat_set_dynamic_dumpmem(UINT32 *buf)
{
}

INT32 wmt_plat_get_tdm_antsel_index(VOID)
{
	return 0;
}

/* ==========================================================================
 * BTIF transport interface (mtk_wcn_btif_*)
 *
 * The critical integration point (B-21). The WMT core sends/receives WMT
 * frames through these. In the vendor tree they are implemented by the BTIF
 * driver (drivers/misc/mediatek/btif); on the Gemini PDA they are backed by
 * the spike's BTIF/DMA transport (drivers/soc/mediatek/mtk-consys-spike.c)
 * via the exported consys_wmt_transport ops (include/linux/consys_wmt.h).
 *
 * The WMT core's STP layer produces complete STP frames (header + payload +
 * CRC); mtk_wcn_btif_write() passes them to the spike's raw TX verbatim. RX
 * is delivered from the spike's RX kthread to the registered callback (the
 * STP parser) in process context - the same model as the vendor's
 * btif_rx_thread consumer.
 *
 * mtk_wcn_btif_open() is a pure transport open (the spike owns CONSYS
 * power). The fresh-MCU state that the WMT core's mand-mode init
 * sequence requires happens in mtk_wcn_btif_rx_cb_register() (spike
 * ops: B-27 power-cycles the CONN domain - an SWSYSRST would leave a
 * patched MCU deaf for the rest of the power-on, see
 * docs/handover-2026-09-06-b.md - so the core sees boot #1 of a fresh
 * power-on, bare ROM, and runs the full vendor init itself: reg-reads,
 * SET_STP, DLM, ROM patch push, 26 MHz restore). On release the MCU
 * is left patched in full mode; the live client is re-synced by the
 * spike (full-mode probe + seq jump).
 * ========================================================================== */
/* WAK keep-awake heartbeat for the BTIF session (defined below, after the
 * wake-pulse implementation); open-gated start/stop. */
static void btif_wak_hb_start(void);
static void btif_wak_hb_stop(void);

int mtk_wcn_btif_open(char *p_owner, unsigned long *p_id)
{
	const struct consys_wmt_ops *ops = consys_wmt_transport;

	if (!ops || !ops->ready()) {
		pr_err("wcn-glue: btif_open: CONSYS WMT link not up (spike G2b failed?)\n");
		return -ENETDOWN;
	}
	/* The stp layer (stpBtifId) uses the returned id as a truthiness
	 * check ("NULL BTIF ID reference"); the vendor hands out a
	 * non-zero per-user pointer. Opaque to the spike's ops. */
	if (p_id)
		*p_id = 1;
	/* Keep the CONSYS MCU awake for the whole BT session (see the
	 * heartbeat comment below): a scan/connection runs for seconds with
	 * no host writes, and the MCU's autonomous sleep mid-session made
	 * the STP layer hit its TX retry limit and forced a spike resync
	 * (glass 2026-09-11, bluetoothd discovery). */
	btif_wak_hb_start();
	pr_info("wcn-glue: btif_open (spike BTIF/DMA transport, WAK heartbeat on)\n");
	return 0;
}

int mtk_wcn_btif_close(unsigned long u_id)
{
	/* The link release (MCU reset + live-client re-init) happens in
	 * rx_cb_register(NULL); nothing to do here. */
	btif_wak_hb_stop();
	return 0;
}

/* Defined below (the ioremap'd BTIF WAK pulse); called from the write
 * path and the heartbeat so a prototype lives here. */
int mtk_wcn_btif_wakeup_consys(unsigned long u_id);

/* BTIF WAK keep-awake heartbeat (BT rx-stall work, 2026-09-11).
 *
 * The wake-before-send pulse in mtk_wcn_btif_write wakes the CONSYS MCU
 * for the TX moment, but the controller's own activity windows (a reply
 * taking ~40-80 ms, a scan/connect running for seconds with NO host
 * writes at all) far outlive that single pulse, and the MCU's
 * autonomous sleep dozes it again mid-window: replies land late or
 * never, the STP TX layer hits its retry limit and the spike live
 * client has to resync (glass 2026-09-11 during a bluetoothd LE
 * discovery: no host writes for the discovery interval -> MCU asleep ->
 * the scan-disable got no ACK -> "stp_do_tx_timeout: TX retry limit =
 * 10" -> spike resync).
 *
 * So the heartbeat is gated by the BTIF link being OPEN, not by recent
 * writes: mtk_wcn_btif_open starts a WAK pulse train every ~30 ms and
 * mtk_wcn_btif_close stops it. While BT is in use the MCU stays awake
 * (waking an awake MCU is harmless; the cadence that was proven clean
 * on glass by hammering WAK every 80 ms); when hci0 is closed the MCU
 * is allowed back to sleep and the next open's first write wakes it via
 * wake-before-send. The 30 ms train costs nothing measurable vs the
 * vendor's negotiated-sleep path that this port disables.
 */
static bool btif_hb_inited;
static bool btif_hb_running;
static struct delayed_work btif_wak_hb_work;
static unsigned int btif_wak_hb_ms = 30; /* WAK pulse cadence while the BTIF link is open */

module_param(btif_wak_hb_ms, uint, 0644);

static void btif_wak_hb_fn(struct work_struct *work)
{
	if (!btif_wak_hb_ms)
		return;
	/* Pulse and keep going for as long as the BTIF link is open. */
	if (btif_hb_running) {
		mtk_wcn_btif_wakeup_consys(0);
		schedule_delayed_work(&btif_wak_hb_work,
				       msecs_to_jiffies(btif_wak_hb_ms));
	}
}

static void btif_wak_hb_start(void)
{
	if (!btif_wak_hb_ms)
		return;
	if (!btif_hb_inited) {
		INIT_DELAYED_WORK(&btif_wak_hb_work, btif_wak_hb_fn);
		btif_hb_inited = true;
	}
	if (!btif_hb_running) {
		btif_hb_running = true;
		schedule_delayed_work(&btif_wak_hb_work,
				       msecs_to_jiffies(btif_wak_hb_ms));
	}
}

static void btif_wak_hb_stop(void)
{
	if (btif_hb_running) {
		btif_hb_running = false;
		cancel_delayed_work_sync(&btif_wak_hb_work);
	}
}

int mtk_wcn_btif_write(unsigned long u_id, const unsigned char *p_buf,
		      unsigned int len)
{
	const struct consys_wmt_ops *ops = consys_wmt_transport;
	int ret;

	if (!ops || !ops->ready())
		return -ENETDOWN;
	if (len == 0)
		return 0;

	/* Wake-before-send (BT rx-stall fix, 2026-09-11): the CONSYS MCU
	 * autonomously sleeps even on the DMA transport once host TX
	 * activity pauses (observed within ~ms of the last exchange), and
	 * a sleeping MCU swallows the reply to the frame just sent: the
	 * cmd-complete sits INSIDE the chip until the next WAK pulse
	 * (glass: a reset's reply surfaced 17 s late at the next open;
	 * with per-write WAK the whole hciconfig init answered in ~ms).
	 * The vendor wakes the chip via the negotiated WMT sleep path,
	 * which this port disables (the spike never arms CONSYS PSM), so
	 * the only wake left is the BTIF WAK line - pulse it before every
	 * write (one pulse keeps the MCU awake for the whole exchange;
	 * ~100 us of usleep per frame is noise). Waking an awake MCU is
	 * harmless. */
	ret = mtk_wcn_btif_wakeup_consys(u_id);
	if (ret)
		pr_debug("wcn-glue: btif_write: WAK pulse failed (%d) - continuing\n",
			 ret);

	/* The open-gated heartbeat normally keeps the MCU awake; this
	 * immediate pulse covers the first write of a session before the
	 * heartbeat's first tick lands. */
	if (ops->tx(p_buf, (int)len))
		return -EIO;
	return (int)len;
}

int mtk_wcn_btif_rx_cb_register(unsigned long u_id, MTK_WCN_BTIF_RX_CB rx_cb)
{
	const struct consys_wmt_ops *ops = consys_wmt_transport;

	if (!ops)
		return -ENODEV;
	if (ops->rx_cb_register((int (*)(const u8 *, unsigned int))rx_cb))
		return -ENETDOWN;
	pr_info("wcn-glue: btif_rx_cb_register (%s)\n",
		rx_cb ? "link handed to the WMT core" : "link released to the live client");
	return 0;
}

int mtk_wcn_btif_wakeup_consys(unsigned long u_id)
{
	/* Pulse the BTIF WAK line (ap_wakeup_consys, BTIF base 0x1100c000
	 * + 0x64, vendor hal_btif_send_wakeup_signal: low > one 32k period
	 * then high) to wake the CONSYS MCU from its autonomous sleep.
	 *
	 * Self-contained ioremap on purpose (corrected 2026-09-10, BT
	 * bring-up): the sleep is real even on the DMA transport (observed
	 * ~70 s idle -> WMT WAKEUP handshake timeout -> whole-chip assert
	 * when hci_stp turns BT on), and a module->builtin ops call for the
	 * wake would ABI-mismatch until the boot.img catches up (the spike
	 * is builtin - Image; the .wake op only exists in Image builds
	 * newer than the one on glass today, and dereferencing it there
	 * oopses). The BTIF clock is left running by the spike after the
	 * transport handover, so the poke is safe. A kernel that HAS the
	 * spike wake op could use ops->wake instead; this is the portable
	 * path.
	 */
	void __iomem *wak = ioremap(0x1100c000UL + 0x64, 4);

	if (!wak)
		return -ENOMEM;
	writel(0, wak);
	usleep_range(64, 96);
	writel(1, wak);
	iounmap(wak);
	return 0;
}

int mtk_wcn_btif_dbg_ctrl(unsigned long u_id, ENUM_BTIF_DBG_ID flag)
{
	return 0;
}

int mtk_wcn_btif_dpidle_ctrl(unsigned long u_id, ENUM_BTIF_DPIDLE_CTRL en_flag)
{
	return 0;
}

int mtk_wcn_btif_loopback_ctrl(unsigned long u_id, ENUM_BTIF_LPBK_MODE enable)
{
	return 0;
}

bool mtk_wcn_btif_parser_wmt_evt(unsigned long u_id, const char *sub_str,
			      unsigned int str_len)
{
	/* True if the buffer starts with a WMT event (cmd byte 0x02). */
	return (str_len > 0) && (sub_str[0] == 0x02);
}

/* ==========================================================================
 * Kernel-side wmt_loader shim (srh_patch)
 *
 * In the vendor Android system a userspace daemon (/vendor/bin/wmt_loader)
 * answers the WMT core's loader commands: the core's wmt_ctrl_ul_cmd()
 * sets WMT_STAT_CMD, triggers the cmdReq event and waits (2 s) on cmdResp;
 * the daemon reads the command (clearing the bit), scans /lib/firmware for
 * the ROM patches, feeds their names/addresses back via the SET_PATCH_NUM/
 * SET_PATCH_INFO ioctls, then writes "ok" to release the waiter. With no
 * daemon on the Gemini rootfs, pwr-on dies at mtk_wcn_soc_patch_info_
 * prepare() ("wait signal timeout" / "cmd buf is occupied").
 *
 * This kthread plays the daemon for the only command the SOC/BTIF path
 * issues (srh_patch). The patch set is known for this product: the two
 * ROMv3 blobs the G2b spike pushes (download-seq 1 = ROMv3_patch_1_1_hdr
 * .bin, seq 2 = ROMv3_patch_1_0_hdr.bin; /lib/firmware on the device,
 * also built into the kernel image for the spike's pre-rootfs probe).
 * Per-patch RAM address = header bytes 24..27 with byte 24 (patch-count<<4
 * | download-seq) zeroed - the spike's derivation, byte-validated against
 * the golden H35 trace (the launcher's own logic is a closed-source blob;
 * the address bytes land verbatim in WMT_PATCH_P_ADDRESS_CMD[12..15]).
 * ========================================================================== */
static const char *const consys_romv3_patches[] = {
	"ROMv3_patch_1_1_hdr.bin",
	"ROMv3_patch_1_0_hdr.bin",
};

static P_WMT_PATCH_INFO g_consys_patch_info;
static struct task_struct *g_consys_loader_task;

static int consys_loader_srh_patch(void)
{
	P_WMT_PATCH_INFO info;
	const struct firmware *fw;
	int i, ret;

	info = kcalloc(ARRAY_SIZE(consys_romv3_patches), sizeof(*info),
			GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(consys_romv3_patches); i++) {
		info[i].dowloadSeq = i + 1;
		strscpy(info[i].patchName, consys_romv3_patches[i],
			sizeof(info[i].patchName));
		ret = request_firmware(&fw, consys_romv3_patches[i], NULL);
		if (ret) {
			pr_err("wcn-glue: srh_patch: request_firmware(%s) fail (%d)\n",
			       consys_romv3_patches[i], ret);
			kfree(info);
			return ret;
		}
		if (fw->size < sizeof(WMT_PATCH)) {
			pr_err("wcn-glue: srh_patch: %s too small (%zu)\n",
			       consys_romv3_patches[i], fw->size);
			release_firmware(fw);
			kfree(info);
			return -EINVAL;
		}
		info[i].addRess[0] = 0;
		info[i].addRess[1] = fw->data[25];
		info[i].addRess[2] = fw->data[26];
		info[i].addRess[3] = fw->data[27];
		pr_info("wcn-glue: srh_patch: %s seq=%u addr=%02x%02x%02x%02x (%zu bytes)\n",
			info[i].patchName, info[i].dowloadSeq, info[i].addRess[0],
			info[i].addRess[1], info[i].addRess[2], info[i].addRess[3],
			fw->size);
		release_firmware(fw);
	}

	kfree(g_consys_patch_info);
	g_consys_patch_info = info;
	wmt_lib_set_patch_num(ARRAY_SIZE(consys_romv3_patches));
	wmt_lib_set_patch_info(g_consys_patch_info);
	pr_info("wcn-glue: srh_patch: %zu ROMv3 patch(es) registered\n",
		ARRAY_SIZE(consys_romv3_patches));
	return 0;
}

static int consys_loader_thread(void *unused)
{
	P_OSAL_EVENT ev = wmt_lib_get_cmd_event();

	while (!kthread_should_stop()) {
		PUINT8 cmd;
		int ret;

		ret = wait_event_interruptible(ev->waitQueue,
			kthread_should_stop() || wmt_lib_get_cmd_status());
		if (kthread_should_stop())
			break;
		if (ret < 0)
			continue;	/* signalled; re-wait */

		cmd = wmt_lib_get_cmd();	/* consumes + clears WMT_STAT_CMD */
		if (cmd && !strcmp(cmd, "srh_patch"))
			ret = consys_loader_srh_patch();
		else {
			pr_warn("wcn-glue: loader: unsupported cmd '%s'\n",
				cmd ? (const char *)cmd : "(null)");
			ret = -EINVAL;
		}
		wmt_lib_trigger_cmd_signal(ret ? -1 : 0);
	}
	return 0;
}

int wcn_loader_init(void)
{
	if (g_consys_loader_task)
		return 0;
	g_consys_loader_task = kthread_run(consys_loader_thread, NULL,
					    "wcn_loader");
	if (IS_ERR(g_consys_loader_task)) {
		pr_err("wcn-glue: wcn_loader kthread failed (%ld)\n",
		       PTR_ERR(g_consys_loader_task));
		g_consys_loader_task = NULL;
		return PTR_ERR(g_consys_loader_task);
	}
	pr_info("wcn-glue: kernel wmt_loader shim up (srh_patch responder)\n");
	return 0;
}

void wcn_loader_exit(void)
{
	if (g_consys_loader_task) {
		kthread_stop(g_consys_loader_task);
		g_consys_loader_task = NULL;
	}
	kfree(g_consys_patch_info);
	g_consys_patch_info = NULL;
}

/* ==========================================================================
 * Bring-up trigger (until the gen3 802.11 stack is ported): the vendor
 * triggers the WMT power-on from the WLAN driver; we expose the same
 * opid call on debugfs.
 *
 *   echo on  > /sys/kernel/debug/wcn/pwr   (blocks ~2 s: full vendor init)
 *   echo off > /sys/kernel/debug/wcn/pwr
 *   cat /sys/kernel/debug/wcn/status
 * ========================================================================== */
static struct dentry *wcn_dbg;
static WMT_HIF_CONF wcn_hif;
static DEFINE_MUTEX(wcn_pwr_mtx);

static int wcn_do_pwr_on(void)
{
	WMT_OP op;
	INT32 ret;

	/* B-32: back the wlan/gen3 divided-FW-download EMI global with the
	 * CONSYS-visible window the spike resolved from the DT reserved-memory
	 * region (same base it programmed into the CONSYS EMI remap at probe). */
	gConEmiPhyBase = g_emi_base;
	pr_info("wcn-glue: gConEmiPhyBase = %pa (%s)\n", &gConEmiPhyBase,
		gConEmiPhyBase ? "spike EMI window" : "UNRESOLVED");

	memset(&wcn_hif, 0, sizeof(wcn_hif));
	wcn_hif.hifType = WMT_HIF_BTIF;
	/* The vendor WLAN driver's hif setup (STP_BTIF_FULL + WMT_FM_COMM
	 * strap) so wmt_ctrl's BTIF branches are taken. The FM strap ALSO
	 * has to live in the embedded struct below: sw_init's "Set FM
	 * strap" step (init_table_5_1) reads pWmtHifConf->au4StrapConf[0]
	 * from the op-data copy (cmd[5]=evt[5]=strap) and the chip echoes
	 * its own strap = WMT_FM_COMM (2) - with a 0 strap the compare
	 * fails (rx 0x02 vs exp 0x00). wmt_lib_set_hif() alone is not
	 * enough: it fills gDevWmt.rWmtHifConf, a struct that never
	 * reaches sw_init. */
	wcn_hif.au4StrapConf[0] = WMT_FM_COMM;
	ret = wmt_lib_set_hif((WMT_FM_COMM << 4) | STP_BTIF_FULL);
	if (ret)
		return ret;

	memset(&op, 0, sizeof(op));
	op.opId = WMT_OPID_PWR_ON;
	op.u4InfoBit |= WMT_OP_HIF_BIT;
	/* opfunc_hif_conf() does osal_memcpy(&gMtkWmtCtx.wmtHifConf,
	 * &pWmtOp->au4OpData[0], sizeof(WMT_HIF_CONF)) — it reads the
	 * struct BY VALUE from the au4OpData[] array, not from a pointer
	 * stored in au4OpData[0]. A pointer there lands in hifType as
	 * garbage and every STP write branch is skipped ("written(0)").
	 * Embed the struct, as the vendor WLAN driver does. */
	memcpy(op.au4OpData, &wcn_hif, sizeof(wcn_hif));
	ret = wmt_core_opid(&op);
	if (ret)
		return ret;

	/* B-32: the PWR_ON op runs the core init (hw_check -> patch push ->
	 * STP ready) but NOT the per-function on leg. The wlan/gen3 driver
	 * learns it must probe from wmt_func_wifi_on() (SOC chip type on the
	 * 0x279 CONSYS): with wlan_gen3 already loaded it calls the probe cb
	 * directly; without it, it sets gWifiProbed so the later
	 * mtk_wcn_wmt_wlan_reg() (platform probe of wlan_gen3) calls wlanProbe
	 * immediately. */
	ret = mtk_wcn_wmt_func_on(WMTDRV_TYPE_WIFI);
	pr_info("wcn-glue: wifi func-on leg -> %d (0 = probe cb ran, -2 = deferred, wlan_gen3 not loaded yet)\n",
		ret);
	return 0;
}

static ssize_t wcn_pwr_write(struct file *filp, const char __user *ubuf,
			    size_t len, loff_t *off)
{
	char b[16];
	int ret;

	if (len == 0 || len > sizeof(b) - 1)
		return -EINVAL;
	if (copy_from_user(b, ubuf, len))
		return -EFAULT;
	b[len] = '\0';
	while (len && (b[len - 1] == '\n' || b[len - 1] == '\r'))
		b[--len] = '\0';

	mutex_lock(&wcn_pwr_mtx);
	if (!strcmp(b, "on")) {
		ret = wcn_do_pwr_on();
	} else if (!strcmp(b, "off")) {
		WMT_OP op;

		memset(&op, 0, sizeof(op));
		op.opId = WMT_OPID_PWR_OFF;
		ret = wmt_core_opid(&op);
	} else {
		mutex_unlock(&wcn_pwr_mtx);
		return -EINVAL;
	}
	mutex_unlock(&wcn_pwr_mtx);

	pr_info("wcn-glue: pwr %s -> %d\n", b, ret);
	return ret ? -EIO : len;
}

static u32 wcn_reg32(u32 pa)
{
	void __iomem *r = ioremap(pa, 4);
	u32 v;

	if (!r)
		return 0;
	v = readl(r);
	iounmap(r);
	return v;
}

static ssize_t wcn_status_read(struct file *filp, char __user *ubuf,
			      size_t len, loff_t *off)
{
	static const char *const drv_sts[] = { "POWER_OFF", "POWER_ON", "FUNC_ON" };
	char buf[192];
	int n;
	u32 chip, pcr;
	ENUM_DRV_STS s;
	bool ready;

	if (wcn_conn_released) {
		n = scnprintf(buf, sizeof(buf),
			      "conn_domain: OFF (PWR_CON=%#x)\n",
			      wcn_reg32(WCN_SPM_CONN_PWR_CON));
		return simple_read_from_buffer(ubuf, len, off, buf, n);
	}
	chip = wcn_reg32(WCN_CONSYS_CHIPID_REG);
	pcr = wcn_reg32(WCN_CONSYS_CPUPCR_REG);
	s = wmt_core_get_drv_status(WMTDRV_TYPE_WMT);
	ready = consys_wmt_transport && consys_wmt_transport->ready();

	n = scnprintf(buf, sizeof(buf),
		      "chip_id    : 0x%08x\n"
		      "cpupcr     : 0x%08x (%s)\n"
		      "wmt_drv    : %s\n"
		      "link_ready : %s\n",
		      chip,
		      pcr, pcr == WCN_CONSYS_CPUPCR_BARE ? "bare ROM" : "patched",
		      (s < ARRAY_SIZE(drv_sts)) ? drv_sts[s] : "?",
		      ready ? "yes" : "no");
	return simple_read_from_buffer(ubuf, len, off, buf, n);
}

static const struct file_operations wcn_pwr_fops = {
	.owner = THIS_MODULE,
	.write = wcn_pwr_write,
	.llseek = no_llseek,
};
static const struct file_operations wcn_status_fops = {
	.owner = THIS_MODULE,
	.read = wcn_status_read,
	.llseek = generic_file_llseek,
};

int wcn_glue_dbgfs_init(void)
{
	wcn_dbg = debugfs_create_dir("wcn", NULL);
	if (IS_ERR(wcn_dbg))
		return PTR_ERR(wcn_dbg);
	debugfs_create_file("pwr", 0200, wcn_dbg, NULL, &wcn_pwr_fops);
	debugfs_create_file("status", 0400, wcn_dbg, NULL, &wcn_status_fops);
	wcn_loader_init();
	pr_info("wcn-glue: debugfs /sys/kernel/debug/wcn (pwr, status)\n");
	return 0;
}
EXPORT_SYMBOL_GPL(wcn_glue_dbgfs_init);

void wcn_glue_dbgfs_exit(void)
{
	wcn_loader_exit();
	debugfs_remove_recursive(wcn_dbg);
	wcn_dbg = NULL;
}
EXPORT_SYMBOL_GPL(wcn_glue_dbgfs_exit);

/* ==========================================================================
 * SDIO detect + SDIO HIF - not used (BTIF transport only). Linkable stubs.
 * ========================================================================== */
int sdio_detect_init(void)
{
	return -ENODEV;
}

int sdio_detect_exit(void)
{
	return 0;
}

int sdio_detect_query_chipid(int waitFlag)
{
	return -ENODEV;
}

int sdio_detect_do_autok(int chipId)
{
	return -ENODEV;
}

int hif_sdio_is_chipid_valid(int chipId)
{
	return 0;
}

MTK_WCN_STP_SDIO_HIF_INFO g_stp_sdio_host_info;

INT32 stp_sdio_rw_retry(ENUM_STP_SDIO_HIF_TYPE_T type, UINT32 retry_limit,
		MTK_WCN_HIF_SDIO_CLTCTX clt_ctx, UINT32 offset, PUINT32 pData,
		UINT32 len)
{
	return -ENODEV;
}

/* HIF/SDIO control hooks (defined in the SDIO hif_sdio.c we don't build;
 * not used on the BTIF path). */
INT32 mtk_wcn_hif_sdio_wmt_control(WMT_SDIO_FUNC_TYPE func_type, MTK_WCN_BOOL is_on)
{
	return 0;
}

INT32 mtk_wcn_hif_sdio_update_cb_reg(INT32 (*ts_update)(VOID))
{
	return 0;
}

/* ==========================================================================
 * AEE (Android Exception Engine) - not in mainline 6.6. Log-only stubs.
 * ========================================================================== */
void aee_kernel_dal_api(const char *file, const int line, const char *msg)
{
	pr_info("wcn-aee: [%s:%d] %s\n", file, line, msg ? msg : "");
}

void aee_kernel_warning_api(const char *file, const int line, const int db_opt,
			   const char *module, const char *msg, ...)
{
	pr_info("wcn-aee[%s]: [%s:%d] %s\n", module ? module : "?", file, line,
		msg ? msg : "");
}

void aed_combo_exception_api(const int *log, int log_size, const int *phy,
			   int phy_size, const char *detail, const int db_opt)
{
	pr_info("wcn-aee: combo exception (detail %s)\n", detail ? detail : "");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gemini PDA");
MODULE_DESCRIPTION("MT6797 Wi-Fi (CONSYS) hardware glue - Gemini PDA port");
