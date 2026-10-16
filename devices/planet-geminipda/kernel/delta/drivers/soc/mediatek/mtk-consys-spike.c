// SPDX-License-Identifier: GPL-2.0
//
// MT6797 CONSYS feasibility-spike probe driver (Gemini PDA, Gates G2a+G2b).
//
// THROWAWAY DIAGNOSTIC, not a production driver: runs the vendor 3.18
// CONSYS power-on sequence (mtk_wcn_consys_hw.c, mt6797) using mainline
// APIs where they exist (regulators, the new CONN scpsys power domain)
// and documented raw register writes where they don't, then polls the
// CONSYS chip-ID register. Success criterion (Gate G2a): chip ID 0x0279
// read back at CONN_MCU_CONFIG_BASE + 0x8 (0x18070008).
//
// Stage W2 extension (Gate G2b): after G2a passes, program the CONSYS
// EMI remap window, initialise the AP<->CONSYS BTIF FIFO (0x1100c000,
// PIO polling only - no IRQ, no DMA), release the CONSYS MCU from the
// watchdog swsysrst, and exchange the first WMT command with the MCU
// boot ROM: WMT_QUERY_STP (01 04 01 00 04) wrapped in STP "mand mode"
// framing (vendor stp_core.c: 4-byte header 0x80|seq<<3,
// type<<4|len>>8, len&0xff, 0x00 + payload + 2 zero CRC bytes; WMT task
// index = 4). Success criterion (Gate G2b): the ROM answers with the
// WMT event payload 02 04 06 00 00 04 ... (vendor wmt_ic_soc.c
// WMT_QUERY_STP_EVT_DEFAULT). On G2b failure the driver deliberately
// leaves CONSYS + BTIF powered and returns 0 so the state can be
// inspected over gadget SSH with devmem.
//
// Stage W3 extension (G2b re-scope, 2026-07-16): after the ROM-only
// query, push the real WMT ROM-patch firmware exactly as the vendor
// does (research.md "WMT Firmware-Push Protocol"): power-on DLM regs,
// 6797 MCU-clock speed-up, then per patch (download-seq order from
// header byte 24) the two patch-address reg-writes, 1000-byte
// fragmented WMT_PATCH commands, and a WMT_RESET; finally the MCU
// clock is restored and WMT_QUERY_STP re-issued - that final query is
// the re-scoped Gate G2b. The blobs are built into the kernel image
// via CONFIG_EXTRA_FIRMWARE (probe runs before the rootfs is mounted,
// so /lib/firmware is not reachable). The push sequence is attempted
// even when the initial query times out: its reg-write commands use
// WMT opcode 0x08, so the responses (or their absence) distinguish
// "ROM ignores opcode 0x04" from "ROM ignores all BTIF traffic".
//
// Every step logs to dmesg BEFORE it executes so a hang identifies the
// exact stalled step from the serial/panel console (patches/STANDARDS.md
// serial-observability rule).
//
// Raw-poke registers (all vendor-cited, research.md "CONSYS Stage W0
// harvest"):
//   0x10001350  CONN2AP sleep mask       |= BIT(8)
//   0x10007018  AP_RGU swsysrst          |= BIT(12) with key 0x88<<24
//   0x10006000  SPM PWRON_CONFG_EN       = 0x0b160001 (project code/key)
// These live inside regions owned by scpsys (0x10006000) and the
// watchdog (0x10007000), so this driver deliberately uses plain
// ioremap() on exact 4-byte windows instead of devm_ioremap_resource()
// (which would request_mem_region() and collide with those drivers).

#include <asm/unaligned.h>
#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/consys_wmt.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/poll.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/kthread.h>

#define CONSYS_CHIP_ID_PA	0x18070008UL	/* CONN_MCU_CONFIG_BASE + 0x8 */
#define CONSYS_MCU_ACR_PA	0x18070110UL	/* MCU_CFG_ACR */
#define CONSYS_CPUPCR_PA	0x18070160UL	/* MCU program counter (RO) */
#define CONSYS_MCU_ACR_MBIST	BIT(18)
#define CONSYS_CHIP_ID_EXPECT	0x0279

#define CONN2AP_SLEEP_MASK_PA	0x10001350UL
#define CONN2AP_SLEEP_MASK_BIT	BIT(8)

#define AP_RGU_SWSYSRST_PA	0x10007018UL
#define AP_RGU_CONN_MCU_RST	BIT(12)
#define AP_RGU_KEY		(0x88U << 24)

#define SPM_PWRON_CONFG_EN_PA	0x10006000UL
#define SPM_PWRON_CONFG_EN_VAL	0x0b160001

#define CHIP_ID_RETRIES		10

/* --- Stage W2 (Gate G2b) --- */

/* CONSYS->AP EMI remap: TOPCKGEN + 0x1340 gets (emi_base & 0xFFF00000)
 * >> 20, plus BIT(12) = remap enable (vendor mtk_wcn_consys_hw.c). The
 * 343K window at emi_base + 512K is the fw coredump/ctrl area the
 * vendor zeroes before releasing the MCU. */
#define CONSYS_EMI_MAPPING_PA	0x10001340UL
#define CONSYS_EMI_REMAP_EN	BIT(12)
#define CONSYS_EMI_COREDUMP_OFF	(SZ_1M / 2)
#define CONSYS_EMI_COREDUMP_SZ	(343 * SZ_1K)

/* BTIF block (vendor DTB btif@1100c000; register map from vendor
 * btif_priv.h). PIO polling only. */
#define BTIF_PA			0x1100c000UL
#define BTIF_SZ			0x100
#define BTIF_RBR		0x00	/* RX buffer (RO) */
#define BTIF_THR		0x00	/* TX holding (WO) */
#define BTIF_IER		0x04
#define BTIF_IIR		0x08	/* RO */
#define BTIF_FIFOCTRL		0x08	/* WO */
#define BTIF_FAKELCR		0x0c
#define BTIF_LSR		0x14
#define BTIF_SLEEP_EN		0x48
#define BTIF_DMA_EN		0x4c
#define BTIF_TRI_LVL		0x60
#define BTIF_WAK		0x64	/* WO: ap_wakeup_consys line */
#define BTIF_HANDSHAKE		0x6c

#define BTIF_LSR_DR		BIT(0)
#define BTIF_LSR_TEMT		BIT(6)
#define BTIF_IER_RXFEN		BIT(0)	/* vendor hal_btif_hw_init() leaves this set */
#define BTIF_FIFOCTRL_CLR_TX	BIT(2)
#define BTIF_FIFOCTRL_CLR_RX	BIT(1)
#define BTIF_DMA_EN_AUTORST	BIT(2)
#define BTIF_DMA_EN_RX		BIT(0)	/* BTIF_DMA_EN @ base+0x4c (vendor) */
#define BTIF_DMA_EN_TX		BIT(1)
/* Golden values from the working vendor stack (Stage W0b harvest,
 * boot.md 2026-07-15): TRI_LVL = 0x18 (TX thresh 8, RX thresh 1),
 * HANDSHAKE = 0x3 -- not the 0x48/0x1 earlier builds used. */
#define BTIF_TRI_LVL_VAL	(8 | (1 << 4))	/* TX thresh 8, RX thresh 1, loopback off */
#define BTIF_HANDSHAKE_EN	3

/* Build B: BTIF DMA mode - the vendor's real transport (btif_dma_plat.c
 * + mtk_btif.c _btif_{tx,rx}_dma_setup). The APDMA block (0x11000000,
 * INFRA_AP_DMA clock = infra1_cg bit 18, OFF by default - verified
 * 0x10001094=0x4 on #301) carries BTIF's dedicated vFIFO bridge
 * channels: TX @ 0x11000a00 (GIC SPI 116), RX @ 0x11000a80 (SPI 117).
 * 8 KB vFIFOs in system memory (32-bit physical here, so ADDR_H=0):
 *   TX: SW memcpys the frame into the vFIFO at WPT, then writes the
 *       advanced WPT to VFF_WPT (the kick); the engine moves bytes
 *       vFIFO -> BTIF THR. A leftover <8-byte tail needs the FLUSH bit.
 *       Done when VFF_VALID==0 && TX_INTBUF==0 (vendor hal_dma_is_tx_
 *       complete; with MTK_BTIF_ENABLE_CLK_REF_COUNTER=1 the INT_FLAG
 *       check is bypassed).
 *   RX: the engine autonomously moves BTIF RBR -> vFIFO; SW polls
 *       VFF_VALID / WPT!=RPT, copies out from RPT, acks via VFF_RPT
 *       (vendor hal_rx_dma_irq_handler loop, polling version - DMA_EN
 *       is independent of the IER, so no IRQ is needed).
 * No WAK pulse in DMA mode (vendor _btif_dma_write doesn't pulse; H35
 * trace = DMA). H35 evidence that DMA is the transition fix: the
 * mcuclk-up HCLK write (live re-clock) is ACKed in 0.38 ms and the
 * down-clock RATIO_DIS in 0.36 ms, with zero losses across the whole
 * 259-fragment push - vs the PIO coin flip (2/8 cycles, #299-#301). */
#define BTIF_TXDMA_PA		0x11000a00UL
#define BTIF_RXDMA_PA		0x11000a80UL
#define BTIF_DMA_SZ		0x80
#define BTIF_DMA_VFF_SIZE	(8 * 1024)
#define BTIF_TX_THRE		(BTIF_DMA_VFF_SIZE - 7)	/* vendor DMA_TX_THRE(n) */
#define BTIF_RX_THRE		(BTIF_DMA_VFF_SIZE * 3 / 4)	/* DMA_RX_THRE(n) */
/* per-channel register offsets (vendor btif_dma_priv.h) */
#define BTIFDMA_INT_FLAG	0x00
#define BTIFDMA_INT_EN		0x04
#define BTIFDMA_EN		0x08
#define BTIFDMA_RST		0x0c
#define BTIFDMA_STOP		0x10
#define BTIFDMA_FLUSH		0x14
#define BTIFDMA_VFF_ADDR	0x1c
#define BTIFDMA_VFF_LEN		0x24
#define BTIFDMA_VFF_THRE	0x28
#define BTIFDMA_VFF_WPT		0x2c
#define BTIFDMA_VFF_RPT		0x30
#define BTIFDMA_TX_INTBUF	0x38
#define BTIFDMA_VFF_VALID	0x3c
#define BTIFDMA_VFF_LEFT	0x40
#define BTIFDMA_VFF_ADDR_H	0x54
#define BTIFDMA_WPT_MASK	0x0000ffffU
#define BTIFDMA_WPT_WRAP	0x00010000U
#define BTIFDMA_WARM_RST	0x1
#define BTIFDMA_FLUSH_BIT	0x1
#define BTIFDMA_EN_BIT		0x1

static void __iomem *txdma_regs, *rxdma_regs;
static struct clk *apdma_clk;
static u8 *tx_vff, *rx_vff;
static dma_addr_t tx_vff_dma, rx_vff_dma;
static u32 tx_wpt, tx_wpt_wrap, rx_rpt, rx_rpt_wrap;
static bool btif_dma_active;

/* ---- Live WMT client (post-boot) ----
 * After gate G2b the CONSYS MCU is running the patched ROM with a
 * live full-mode WMT link over the (DMA) BTIF transport. The BTIF
 * mapping is kept alive (wmt_btif) and exposed to userspace via
 * /dev/consys_wmt so the WMT-firmware phase - and, later, the host
 * driver - can drive the link after boot. write() = a raw WMT command
 * payload (sent in full-mode STP framing); read() = the captured
 * response payload. */
#define WMT_LIVE_RESP_SZ	2048
static void __iomem *wmt_btif;
static struct device *wmt_dev;
static bool wmt_ready;

/* VCN33 BT/WiFi sub-rails (B-30): fetched at probe but kept OFF (radio
 * idle). The WMT core's sw_init powers them up via the PALDO ops for
 * the post-push RF-calibration exchange (the calibration engine needs
 * the RF LDOs; without them the start-RF-cal command gets no response)
 * and back down afterwards - same as the vendor's
 * mtk_wcn_consys_hw_{bt,wifi}_paldo_ctrl. */
static struct regulator *g_vcn33_bt, *g_vcn33_wifi;
static DEFINE_MUTEX(wmt_mtx);
static u8 wmt_cmd[WMT_LIVE_RESP_SZ];
static u8 wmt_resp[WMT_LIVE_RESP_SZ];
static int wmt_resp_len;

#define BTIF_TX_FIFO_SIZE	16

/* WMT over STP mand-mode framing (vendor stp_core.c / stp_exp.h) */
#define STP_HDR_SIZE		4
#define STP_CRC_SIZE		2
#define WMT_TASK_INDX		4

/* Boot-path diagnostic (docs/wifi-consys.md): INFRA_MISC. The H35
 * golden trace holds 0x6d403a00 at the driver's reg_ctrl entry — set
 * by the preloader (boot0) on a true cold power-on; neither LK nor the
 * 3.18 kernel writes it. Every WDT/adb warm-reboot 6.6 test saw
 * 0x11403200. Logged at probe start before any CONSYS power-on. */
#define INFRA_MISC_PA		0x10001f00UL
#define INFRA_MISC_GOLDEN	0x6d403a00U

/* Vendor FIRST WMT commands (H35 trace t≈105.258 s; wmt_core_hw_check()
 * + mtk_wcn_soc_ver_check()): three reg-READs (opcode 0x08, op=2) with
 * mask 0x0000ffff, 16-byte RD event echoing addr+value, BEFORE any
 * QUERY_STP. Addresses/mask from wmt_ic.h:83-88. */
#define GEN_CONFG_BASE		0x80000000UL
#define GEN_HCR_ADDR		(GEN_CONFG_BASE + 0x8)	/* chip id, golden 0x0279 */
#define GEN_HVR_ADDR		(GEN_CONFG_BASE + 0x0)	/* hw_ver, golden 0x8a00 */
#define GEN_FVR_ADDR		(GEN_CONFG_BASE + 0x4)	/* fw_ver, golden 0x8a00 */
#define GEN_VER_MASK		0x0000ffffUL
static const u8 wmt_reg_rd_evt_hdr[] = {
	0x02, 0x08, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x01
};

/* WMT_QUERY_STP_CMD and its default-ROM event (vendor wmt_ic_soc.c) */
static const u8 wmt_query_stp_cmd[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
/* Full vendor WMT_QUERY_STP_EVT_DEFAULT (wmt_ic_soc.c) - build #256
 * source audit found our prior 6-byte constant was missing the
 * trailing 4 bytes, so a real ROM reply could fail memcmp even when
 * the MCU answered correctly. */
static const u8 wmt_query_stp_evt[] = {
	0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0x11, 0x00, 0x00, 0x00
};

/* WMT_SET_STP_CMD/EVT (vendor wmt_ic_soc.c:190): switch the chip from
 * mand mode to full-mode STP. The host follows after a 10 ms settle
 * (vendor sw_init: "enough for chip do mechanism switch"). The H35
 * golden trace does exactly this between its mand and full phases. */
static const u8 wmt_set_stp_cmd[] = {
	0x01, 0x04, 0x05, 0x00, 0x03, 0xDF, 0x0E, 0x68, 0x01
};
static const u8 wmt_set_stp_evt[] = {
	0x02, 0x04, 0x02, 0x00, 0x00, 0x03
};

/* WMT_QUERY_STP_EVT (vendor, post-switch query): the STP conf comes
 * back as 0x0004 df 0e 68 01. For the gate queries we only match the
 * 4-byte event header (02 04 06 00) and log the rest - the CRC in
 * full mode already proves frame integrity. */
static const u8 wmt_query_stp_evt_hdr[] = {
	0x02, 0x04, 0x06, 0x00
};

#define G2B_RX_BUF_SZ		64
#define G2B_RX_TIMEOUT_MS	3000	/* A3: the push now runs at the MCU's
					 * default 26 MHz (no clock-up) - allow slow
					 * ACKs without a false timeout + retransmit */

/* Build A2: mand-mode STP has no NAK/retransmit (the vendor gets it
 * from full-mode STP), so a dropped command is lost. Our commands are
 * idempotent in the failure modes observed (a command that was never
 * received; reg writes re-apply the same value; a re-sent firmware
 * fragment either lands in the same MCU RAM slot or, in the rarer
 * "processed-but-ACK-lost" case, fails the push exactly like a no-
 * retransmit build — the post-reset query can never false-PASS). */
#define WMT_MAX_ATTEMPTS	4	/* 1 + 3 retransmits */
#define WMT_FASTFAIL_AFTER	3	/* consecutive no-response cmds
					 * after which later cmds make a
					 * single attempt (link-dead fast
					 * path; bounds a deaf boot) */
static int wmt_consecutive_noresp;
/* Once bytes start arriving, stop draining after this much silence so
 * per-fragment acks don't each burn the full timeout. */
#define G2B_RX_IDLE_MS		30

/* --- Stage W3: WMT firmware push (vendor wmt_ic_soc.c, 6797 path; full
 * byte-level derivation in research.md "WMT Firmware-Push Protocol") --- */

/* WMT_PATCH header (wmt_core.h): 16B datetime, 4B "ALPS", u2HwVer,
 * u2SwVer, u4PatchVer. Byte 24 = patch-count<<4 | download-seq; bytes
 * 24..27 with byte 24 zeroed = the patch RAM address for
 * WMT_PATCH_P_ADDRESS_CMD (stp_uart_launcher.c srh_patch()). */
#define WMT_PATCH_HDR_SIZE	28
#define WMT_PATCH_INFO_OFF	24
#define WMT_PATCH_FRAG_SIZE	1000
#define WMT_PATCH_FRAG_1ST	1
#define WMT_PATCH_FRAG_MID	2
#define WMT_PATCH_FRAG_LAST	3

/* In vendor download-seq order (seq 1 = _1_1, seq 2 = _1_0 - from each
 * file's header byte 24: 0x21 and 0x22). */
static const char *const wmt_patch_names[] = {
	"ROMv3_patch_1_1_hdr.bin",
	"ROMv3_patch_1_0_hdr.bin",
};

/* Reg-write op (opcode 0x08) shared event */
static const u8 wmt_reg_evt[] = { 0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01 };

/* WMT_PATCH_ADDRESS_CMD with the 6797 override already applied
 * (wmt_ic_soc.c: bytes 8/9 = 0x08/0x05 for icId 0x0279): write 0 to
 * 0x02090508, mask 0xffffffff. */
static const u8 wmt_patch_address_cmd[] = {
	0x01, 0x08, 0x10, 0x00,
	0x01, 0x01, 0x00, 0x01,
	0x08, 0x05, 0x09, 0x02,
	0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff
};

/* WMT_PATCH_P_ADDRESS_CMD, 6797 override (bytes 8/9 = 0x2c/0x0b):
 * write <patch address> to 0x02090b2c. Bytes 12..15 are filled per
 * patch from its header. */
static const u8 wmt_patch_p_address_cmd[] = {
	0x01, 0x08, 0x10, 0x00,
	0x01, 0x01, 0x00, 0x01,
	0x2c, 0x0b, 0x09, 0x02,
	0x00, 0x00, 0x00, 0x00,	/* <- patch addRess[4] */
	0xff, 0xff, 0xff, 0xff
};

static const u8 wmt_patch_evt[] = { 0x02, 0x01, 0x01, 0x00, 0x00 };
static const u8 wmt_reset_cmd[] = { 0x01, 0x07, 0x01, 0x00, 0x04 };
static const u8 wmt_reset_evt[] = { 0x02, 0x07, 0x01, 0x00, 0x00 };

/* wmt_power_on_dlm_table: three masked writes to 0x80100060 (non-fatal
 * in the vendor flow too). */
static const u8 wmt_dlm_cmd1[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x60, 0x00, 0x10, 0x80, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x0f, 0x00, 0x00
};
static const u8 wmt_dlm_cmd2[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x60, 0x00, 0x10, 0x80, 0x00, 0x00, 0x00, 0x00,
	0xf0, 0x00, 0x00, 0x00
};
static const u8 wmt_dlm_cmd3[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x60, 0x00, 0x10, 0x80, 0x00, 0x00, 0x00, 0x00,
	0x08, 0x00, 0x00, 0x00
};

/* set_mcuclk_table_3/_4 (6797): speed the MCU clock up for the
 * download, restore 26 MHz afterwards. Non-fatal in the vendor flow. */
static const u8 wmt_mcuclk_en[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x10, 0x11, 0x02, 0x81, 0x00, 0x00, 0x00, 0x10,
	0x00, 0x00, 0x00, 0x10
};
static const u8 wmt_mcuclk_ratio[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x0c, 0x01, 0x00, 0x80, 0x40, 0x00, 0x00, 0x00,
	0xc0, 0x00, 0x00, 0x00
};
static const u8 wmt_mcuclk_div[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x18, 0x11, 0x02, 0x80, 0x07, 0x00, 0x00, 0x00,
	0x3f, 0x00, 0x00, 0x00
};
static const u8 wmt_mcuclk_hclk[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x00, 0x11, 0x02, 0x81, 0x04, 0x00, 0x00, 0x00,
	0x07, 0x00, 0x00, 0x00
};
static const u8 wmt_mcuclk_hclk_dis[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x00, 0x11, 0x02, 0x81, 0x00, 0x00, 0x00, 0x00,
	0x07, 0x00, 0x00, 0x00
};
static const u8 wmt_mcuclk_ratio_dis[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x0c, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
	0xc0, 0x00, 0x00, 0x00
};
static const u8 wmt_mcuclk_dis[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x10, 0x11, 0x02, 0x81, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x10
};

static int consys_poke(unsigned long pa, u32 set_bits, u32 key,
		       bool overwrite, u32 value, struct device *dev,
		       const char *what)
{
	void __iomem *va;
	u32 old, new;

	va = ioremap(pa, 4);
	if (!va) {
		dev_err(dev, "consys-spike: ioremap(%#lx) for %s failed\n",
			pa, what);
		return -ENOMEM;
	}
	old = readl(va);
	if (overwrite)
		new = value;
	else
		new = old | set_bits | key;
	dev_info(dev, "consys-spike: %s %#lx: %#x -> %#x\n",
		 what, pa, old, new);
	writel(new, va);
	iounmap(va);
	return 0;
}

static int consys_poke_clear(unsigned long pa, u32 clr_bits, u32 key,
			     struct device *dev, const char *what)
{
	void __iomem *va;
	u32 old, new;

	va = ioremap(pa, 4);
	if (!va) {
		dev_err(dev, "consys-spike: ioremap(%#lx) for %s failed\n",
			pa, what);
		return -ENOMEM;
	}
	old = readl(va);
	new = (old & ~clr_bits) | key;
	dev_info(dev, "consys-spike: %s %#lx: %#x -> %#x\n", what, pa, old, new);
	writel(new, va);
	iounmap(va);
	return 0;
}

/* The reserved region once mapped (shared MCU<->AP memory). */
phys_addr_t g_emi_base; /* B-32: exported so the wlan/gen3 divided FW download
			  * (gConEmiPhyBase) writes into the same CONSYS-visible window. */
static size_t g_emi_size;

/* Program the CONSYS->AP EMI remap register and zero the fw ctrl
 * window inside the reserved region (vendor does both before the MCU
 * is released). */
static int consys_emi_setup(struct device *dev)
{
	struct device_node *np;
	struct reserved_mem *rmem;
	phys_addr_t base;
	void __iomem *win;
	u32 map;
	int ret;

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np) {
		dev_err(dev, "consys-spike: no memory-region\n");
		return -ENODEV;
	}
	/* The region is a dynamic-allocation reserved-memory node (no
	 * "reg"), so of_address_to_resource() cannot resolve it; the
	 * kernel-chosen base is only in the reserved_mem table. */
	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem) {
		dev_err(dev, "consys-spike: memory-region unresolved\n");
		return -EINVAL;
	}
	base = rmem->base;
	g_emi_base = base;
	g_emi_size = rmem->size;

	map = ((u32)(base & 0xFFF00000) >> 20) | CONSYS_EMI_REMAP_EN;
	dev_info(dev, "consys-spike: EMI base %pa size %pa, remap word %#x\n",
		 &base, &rmem->size, map);
	ret = consys_poke(CONSYS_EMI_MAPPING_PA, map, 0, false, 0, dev,
			  "CONSYS_EMI_MAPPING");
	if (ret)
		return ret;

	dev_info(dev, "consys-spike: zeroing EMI ctrl window (+%#x, %#x bytes)\n",
		 CONSYS_EMI_COREDUMP_OFF, CONSYS_EMI_COREDUMP_SZ);
	win = ioremap(base + CONSYS_EMI_COREDUMP_OFF,
		      CONSYS_EMI_COREDUMP_SZ);
	if (!win) {
		dev_err(dev, "consys-spike: EMI window ioremap failed\n");
		return -ENOMEM;
	}
	memset_io(win, 0, CONSYS_EMI_COREDUMP_SZ);
	iounmap(win);
	return 0;
}

/* Vendor hal_btif_hw_init(), PIO subset: normal LCR, new-handshake
 * mode, FIFO clear, trigger levels + loopback off, DMA off, Tx IRQ
 * masked (we poll LSR for TX). Rx IER is left SET (BTIF_IER_RXFEN),
 * matching hal_btif_hw_init()'s "enable Rx IER by default" - build
 * #257 source audit found we masked it too, the one BTIF-register
 * divergence from vendor found after a full bit-for-bit review; no
 * IRQ handler is registered so this has no runtime effect beyond
 * matching the register value, but is cheap to rule in/out. */
static void btif_hw_init(void __iomem *btif)
{
	writel(0, btif + BTIF_FAKELCR);
	writel(BTIF_HANDSHAKE_EN, btif + BTIF_HANDSHAKE);
	/* The FIFO-clear bits are level-held, not self-clearing: the
	 * vendor sets then explicitly clears each (build #237/#238
	 * left them asserted, holding both FIFOs in reset — RX data
	 * from the ROM was silently discarded). */
	writel(BTIF_FIFOCTRL_CLR_RX, btif + BTIF_FIFOCTRL);
	writel(0, btif + BTIF_FIFOCTRL);
	writel(BTIF_FIFOCTRL_CLR_TX, btif + BTIF_FIFOCTRL);
	writel(0, btif + BTIF_FIFOCTRL);
	writel(BTIF_TRI_LVL_VAL, btif + BTIF_TRI_LVL);
	writel(readl(btif + BTIF_DMA_EN) & ~3, btif + BTIF_DMA_EN);
	writel(readl(btif + BTIF_DMA_EN) | BTIF_DMA_EN_AUTORST,
	       btif + BTIF_DMA_EN);
	writel(BTIF_IER_RXFEN, btif + BTIF_IER);
	writel(0, btif + BTIF_SLEEP_EN);
}

/* Vendor hal_btif_send_wakeup_signal(): pulse ap_wakeup_consys low
 * for longer than one 32k period, then high, before transmitting.
 * Build B comment said "skipped in DMA mode - the vendor's
 * _btif_dma_write never pulses WAK (the MCU never sleeps here)";
 * corrected 2026-09-10 (BT bring-up): after ~70 s of host-side
 * inactivity the MCU DOES go to its autonomous sleep even in DMA
 * mode, and with no WAK pulse the WMT core's WAKEUP handshake times
 * out and asserts a whole-chip reset (the exact crash hci_stp hit at
 * BT func-on). Pulse unconditionally now; the line is a WO
 * ap_wakeup_consys signal and the pulse is the vendor's own
 * hal_btif_send_wakeup_signal, so it is safe while the MCU is awake
 * too. */
static void btif_wakeup_consys(void __iomem *btif)
{
	writel(0, btif + BTIF_WAK);
	usleep_range(64, 96);
	writel(1, btif + BTIF_WAK);
}

/* Build B: program + enable one APDMA vFIFO channel (vendor
 * hal_btif_dma_hw_init: WARM_RST, wait for EN to fall, then vFIFO
 * geometry). Idempotent - re-running it after an MCU reset cycle
 * drops any stale RX vFIFO content. */
static void btif_dma_chan_setup(void __iomem *r, dma_addr_t vff_dma,
				 u32 thre)
{
	writel(BTIFDMA_WARM_RST, r + BTIFDMA_RST);
	while (readl_relaxed(r + BTIFDMA_EN) & BTIFDMA_EN_BIT)
		;	/* vendor: do { } while (0x01 & EN) */
	writel(lower_32_bits(vff_dma), r + BTIFDMA_VFF_ADDR);
	writel((u32)(vff_dma >> 32), r + BTIFDMA_VFF_ADDR_H);
	writel(BTIF_DMA_VFF_SIZE, r + BTIFDMA_VFF_LEN);
	writel(thre, r + BTIFDMA_VFF_THRE);
	writel(0, r + BTIFDMA_VFF_WPT);
	writel(0, r + BTIFDMA_VFF_RPT);
	writel(0, r + BTIFDMA_INT_FLAG);	/* vendor clears TX via 0-write */
	writel(0x3, r + BTIFDMA_INT_FLAG);	/* vendor clears RX W1C */
	writel(BTIFDMA_EN_BIT, r + BTIFDMA_EN);
}

/* Build B: set up the BTIF DMA transport. One-time: APDMA clock +
 * 8 KB vFIFOs + channel ioremaps. Per call: channel re-program
 * (drops stale state after MCU reset cycles) + the BTIF-side
 * DMA_EN_RX|TX mode bits (vendor _btif_{tx,rx}_dma_setup order:
 * channels first, mode bits last). On any failure the caller keeps
 * the PIO path (btif_dma_active stays false). */
static int btif_dma_init(struct device *dev, void __iomem *btif)
{
	int ret;

	if (!txdma_regs) {
		apdma_clk = devm_clk_get(dev, "apdma");
		if (IS_ERR(apdma_clk))
			return PTR_ERR(apdma_clk);
		ret = clk_prepare_enable(apdma_clk);
		if (ret)
			return ret;
		txdma_regs = ioremap(BTIF_TXDMA_PA, BTIF_DMA_SZ);
		rxdma_regs = ioremap(BTIF_RXDMA_PA, BTIF_DMA_SZ);
		tx_vff = dma_alloc_coherent(dev, BTIF_DMA_VFF_SIZE,
					     &tx_vff_dma, GFP_KERNEL);
		rx_vff = dma_alloc_coherent(dev, BTIF_DMA_VFF_SIZE,
					     &rx_vff_dma, GFP_KERNEL);
		if (!txdma_regs || !rxdma_regs || !tx_vff || !rx_vff) {
			/* Leave the state fully torn down so a later
			 * call (retry cycle) retries the one-time path
			 * instead of programming a half-set channel. */
			if (tx_vff)
				dma_free_coherent(dev, BTIF_DMA_VFF_SIZE, tx_vff, tx_vff_dma);
			if (rx_vff)
				dma_free_coherent(dev, BTIF_DMA_VFF_SIZE, rx_vff, rx_vff_dma);
			if (txdma_regs)
				iounmap(txdma_regs);
			if (rxdma_regs)
				iounmap(rxdma_regs);
			txdma_regs = rxdma_regs = NULL;
			tx_vff = rx_vff = NULL;
			ret = -ENOMEM;
			dev_err(dev, "consys-spike: BTIF DMA setup failed (regs/vFIFO) (%d) - staying PIO\n", ret);
			return ret;
		}
	}
	tx_wpt = tx_wpt_wrap = rx_rpt = rx_rpt_wrap = 0;
	btif_dma_chan_setup(txdma_regs, tx_vff_dma, BTIF_TX_THRE);
	btif_dma_chan_setup(rxdma_regs, rx_vff_dma, BTIF_RX_THRE);
	writel(readl(btif + BTIF_DMA_EN) | BTIF_DMA_EN_RX | BTIF_DMA_EN_TX,
	       btif + BTIF_DMA_EN);
	btif_dma_active = true;
	dev_info(dev, "consys-spike: BTIF DMA mode up: 8 KB vFIFOs (tx 0x%llx, rx 0x%llx), APDMA clock on, DMA_EN=%#x\n",
		 (unsigned long long)tx_vff_dma,
		 (unsigned long long)rx_vff_dma,
		 readl(btif + BTIF_DMA_EN));
	return 0;
}

/* Build B: send one frame through the TX vFIFO (vendor
 * hal_dma_send_data): memcpy at WPT (wrap-aware), kick via the WPT
 * register write, flush a <8-byte tail, poll for completion. Our
 * frames (<= 1011 B) always fit the 8 KB vFIFO. */
static int btif_tx_dma(struct device *dev, const u8 *buf, int len)
{
	u32 avail, wpt, tail, valid;
	unsigned long t = jiffies + msecs_to_jiffies(200);

	while ((avail = readl(txdma_regs + BTIFDMA_VFF_LEFT)) < (u32)len) {
		if (time_after(jiffies, t)) {
			dev_err(dev, "consys-spike: BTIF DMA TX vFIFO full (left %u)\n", avail);
			return -ETIMEDOUT;
		}
		usleep_range(50, 100);
	}

	wpt = tx_wpt;
	if (wpt + (u32)len >= BTIF_DMA_VFF_SIZE) {
		tail = BTIF_DMA_VFF_SIZE - wpt;
		memcpy(&tx_vff[wpt], buf, tail);
		memcpy(&tx_vff[0], buf + tail, len - tail);
		wpt = wpt + (u32)len - BTIF_DMA_VFF_SIZE;
		tx_wpt_wrap ^= BTIFDMA_WPT_WRAP;
	} else {
		memcpy(&tx_vff[wpt], buf, len);
		wpt += len;
	}
	tx_wpt = wpt;
	wmb();

	/* Vendor re-enables the channel (idempotent) then writes the
	 * advanced WPT - that write is the transfer kick. */
	writel(BTIFDMA_EN_BIT, txdma_regs + BTIFDMA_EN);
	writel(tx_wpt | tx_wpt_wrap, txdma_regs + BTIFDMA_VFF_WPT);

	/* The engine moves in 8-byte steps and leaves a <8-byte tail
	 * in the vFIFO. The vendor flushes it from the TX IRQ handler
	 * (hal_tx_dma_irq_handler: 0 < valid < 8 -> _tx_dma_flush,
	 * repeated); polling, we check the same condition inside the
	 * completion loop - a one-shot check right after the kick
	 * fires too early (nothing consumed yet, valid == frame len). */
	t = jiffies + msecs_to_jiffies(500);
	for (;;) {
		valid = readl(txdma_regs + BTIFDMA_VFF_VALID);
		if (!valid &&
		    !readl(txdma_regs + BTIFDMA_TX_INTBUF))
			break;	/* done (vendor hal_dma_is_tx_complete) */
		if (valid && valid < 8)
			writel(BTIFDMA_FLUSH_BIT,
			       txdma_regs + BTIFDMA_FLUSH);
		if (time_after(jiffies, t)) {
			dev_err(dev, "consys-spike: BTIF DMA TX incomplete (valid %u, intbuf %u)\n",
				valid,
				readl(txdma_regs + BTIFDMA_TX_INTBUF));
			return -ETIMEDOUT;
		}
		udelay(10);
	}
	return 0;
}

/* Build B: one byte from the RX vFIFO (polling version of the vendor
 * hal_rx_dma_irq_handler drain loop): wait for VFF_VALID with WPT
 * ahead of our RPT, copy the byte, ack by advancing VFF_RPT. -1 on
 * deadline. */
static int btif_dma_rx_byte(unsigned long deadline)
{
	u32 wpt, rpt, valid, b;

	for (;;) {
		wpt = readl(rxdma_regs + BTIFDMA_VFF_WPT);
		rpt = rx_rpt | rx_rpt_wrap;
		valid = readl(rxdma_regs + BTIFDMA_VFF_VALID);
		if (valid && wpt != rpt)
			break;
		if (time_after(jiffies, deadline))
			return -1;
		usleep_range(50, 100);
	}

	b = rx_vff[rx_rpt];
	rx_rpt++;
	if (rx_rpt >= BTIF_DMA_VFF_SIZE) {
		rx_rpt = 0;
		rx_rpt_wrap ^= BTIFDMA_WPT_WRAP;
	}
	writel(rx_rpt | rx_rpt_wrap, rxdma_regs + BTIFDMA_VFF_RPT);
	return b;
}

static int btif_tx(struct device *dev, void __iomem *btif, const u8 *buf,
		   int len);
static int btif_rx_drain(void __iomem *btif, u8 *buf, int max,
			 unsigned int timeout_ms, unsigned int idle_ms);
static int wmt_cmd_full(struct device *dev, void __iomem *btif,
			 const u8 *cmd, int cmdlen, const u8 *evt, int evtlen,
			 bool quiet);

/* One WMT reg-READ (opcode 0x08, op=2): 20-byte command, 16-byte
 * event `02 08 0c 00 00 00 00 01 <addr LE> <value LE>` (vendor
 * wmt_core_reg_rw_raw(), wmt_core.c:585; the ROM fills the length
 * field with 0x0c — the vendor only checks total length 16).
 * Returns 0 and *val on match, -ETIMEDOUT/-EBADMSG otherwise. */
static int wmt_reg_read(struct device *dev, void __iomem *btif, u32 addr,
			u32 mask, u32 *val)
{
	u8 cmd[20], frame[STP_HDR_SIZE + 20 + STP_CRC_SIZE], rx[G2B_RX_BUF_SZ];
	int n, i, ret;

	cmd[0] = 0x01;
	cmd[1] = 0x08;
	cmd[2] = 0x10;
	cmd[3] = 0x00;
	cmd[4] = 0x02;		/* op: read */
	cmd[5] = 0x01;		/* type: reg */
	cmd[6] = 0x00;
	cmd[7] = 0x01;		/* count: 1 register */
	put_unaligned_le32(addr, cmd + 8);
	put_unaligned_le32(0, cmd + 12);	/* value field: scratch for reads */
	put_unaligned_le32(mask, cmd + 16);

	frame[0] = 0x80;
	frame[1] = (WMT_TASK_INDX << 4) | ((20 >> 8) & 0x0f);
	frame[2] = 20;
	frame[3] = 0x00;
	memcpy(&frame[STP_HDR_SIZE], cmd, 20);
	frame[STP_HDR_SIZE + 20] = 0x00;
	frame[STP_HDR_SIZE + 20 + 1] = 0x00;

	dev_info(dev, "consys-spike: WMT reg-read TX 0x%08x %*ph\n",
		 addr, 20, cmd);
	btif_wakeup_consys(btif);
	ret = btif_tx(dev, btif, frame, sizeof(frame));
	if (ret)
		return ret;

	n = btif_rx_drain(btif, rx, sizeof(rx), G2B_RX_TIMEOUT_MS,
			  G2B_RX_IDLE_MS);
	dev_info(dev, "consys-spike: WMT reg-read RX %d bytes: %*ph\n",
		 n, n, rx);
	if (n < sizeof(wmt_reg_rd_evt_hdr) + 8)
		return -ETIMEDOUT;
	for (i = 0; i + 16 <= n; i++) {
		if (!memcmp(&rx[i], wmt_reg_rd_evt_hdr,
			    sizeof(wmt_reg_rd_evt_hdr))) {
			u32 evt_addr, evt_val;

			evt_addr = get_unaligned_le32(&rx[i + 8]);
			evt_val = get_unaligned_le32(&rx[i + 12]);
			if (evt_addr != addr)
				return -EBADMSG;
			*val = evt_val;
			return 0;
		}
	}
	return -EBADMSG;
}

/* TX one frame: Build B vFIFO/DMA path when active, else blocking PIO
 * (wait TEMT, burst up to the FIFO size). */
static int btif_tx(struct device *dev, void __iomem *btif, const u8 *buf,
		   int len)
{
	int i, chunk;

	if (btif_dma_active)
		return btif_tx_dma(dev, buf, len);

	while (len > 0) {
		unsigned long t = jiffies + msecs_to_jiffies(100);

		while (!(readl(btif + BTIF_LSR) & BTIF_LSR_TEMT)) {
			if (time_after(jiffies, t)) {
				dev_err(dev, "consys-spike: BTIF TX stuck, LSR=%#x\n",
					readl(btif + BTIF_LSR));
				return -ETIMEDOUT;
			}
			udelay(10);
		}
		chunk = min(len, BTIF_TX_FIFO_SIZE);
		for (i = 0; i < chunk; i++)
			writel(buf[i], btif + BTIF_THR);
		buf += chunk;
		len -= chunk;
	}
	return 0;
}

/* Drain RX for up to timeout_ms into buf; returns bytes read. Once at
 * least one byte has arrived, idle_ms of silence ends the drain early
 * (a complete reply is ~16 bytes; waiting out the full timeout per
 * exchange would make the 259-fragment firmware push take minutes).
 * idle_ms = 0: return as soon as the FIFO is empty after the first
 * byte (vendor btif_rx_thread hands each DMA batch on at once; used
 * by the live STP reader, whose parser takes a byte stream). */
static int btif_rx_drain(void __iomem *btif, u8 *buf, int max,
			 unsigned int timeout_ms, unsigned int idle_ms)
{
	unsigned long t = jiffies + msecs_to_jiffies(timeout_ms);
	unsigned long idle = 0;
	int n = 0, b;

	if (btif_dma_active) {
		/* Same contract as the PIO drain below: fill until max,
		 * the timeout, or G2B_RX_IDLE_MS of silence after the
		 * first byte (a complete reply is ~16 bytes). */
		while (n < max) {
			unsigned long d = n ? min(idle, t) : t;

			if (idle_ms && time_after(d, jiffies) == 0 && n)
				break;
			b = btif_dma_rx_byte(d);
			if (b < 0)
				break;
			buf[n++] = b;
			idle = idle_ms ? jiffies + msecs_to_jiffies(idle_ms) :
					 jiffies - 1;
		}
		return n;
	}

	while (n < max && !time_after(jiffies, t)) {
		if (readl(btif + BTIF_LSR) & BTIF_LSR_DR) {
			buf[n++] = readl(btif + BTIF_RBR) & 0xff;
			idle = idle_ms ? jiffies + msecs_to_jiffies(idle_ms) :
					 jiffies - 1;
		} else {
			if (n && time_after(jiffies, idle))
				break;
			usleep_range(200, 500);
		}
	}
	return n;
}

/* Send one WMT command in STP mand-mode framing and scan the reply
 * bytes for the expected WMT event payload. cmdlen may exceed the old
 * 16-byte stack frame (firmware fragments are 1005 bytes), so the
 * frame is heap-allocated. quiet suppresses the per-exchange TX/RX
 * dumps for the fragment loop; failures always log. */
static int wmt_cmd_evt(struct device *dev, void __iomem *btif, u8 seq,
		       const u8 *cmd, int cmdlen, const u8 *evt, int evtlen,
		       bool quiet)
{
	u8 *frame;
	u8 rx[G2B_RX_BUF_SZ];
	int n, i, ret, attempt;

	frame = kmalloc(STP_HDR_SIZE + cmdlen + STP_CRC_SIZE, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;

	/* Mand-mode byte0 is FIXED 0x80 (vendor stp_send_data_no_ps mand
	 * branch) -- no tx-seq field; earlier builds' seq<<3 retries were
	 * malformed. seq is kept only for log correlation. */
	frame[0] = 0x80;
	frame[1] = (WMT_TASK_INDX << 4) | ((cmdlen >> 8) & 0x0f);
	frame[2] = cmdlen & 0xff;
	frame[3] = 0x00;
	memcpy(&frame[STP_HDR_SIZE], cmd, cmdlen);
	frame[STP_HDR_SIZE + cmdlen] = 0x00;
	frame[STP_HDR_SIZE + cmdlen + 1] = 0x00;

	if (!quiet)
		dev_info(dev, "consys-spike: WMT TX %*ph\n",
			 min(STP_HDR_SIZE + cmdlen + STP_CRC_SIZE, 32), frame);

	for (n = 0, attempt = 1;
	     attempt <= ((wmt_consecutive_noresp >= WMT_FASTFAIL_AFTER) ?
			   1 : WMT_MAX_ATTEMPTS);
	     attempt++) {
		if (attempt > 1)
			dev_info(dev,
				 "consys-spike: WMT retransmit %d/%d (no response)\n",
				 attempt - 1, WMT_MAX_ATTEMPTS - 1);
		btif_wakeup_consys(btif);
		ret = btif_tx(dev, btif, frame,
			      STP_HDR_SIZE + cmdlen + STP_CRC_SIZE);
		if (ret)
			break;
		n = btif_rx_drain(btif, rx, sizeof(rx), G2B_RX_TIMEOUT_MS,
				  G2B_RX_IDLE_MS);
		if (n > 0)
			break;	/* something came back; judge below */
	}
	kfree(frame);
	if (ret)
		return ret;

	if (n > 0)
		wmt_consecutive_noresp = 0;
	else
		wmt_consecutive_noresp++;

	if (!quiet || n < evtlen)
		dev_info(dev, "consys-spike: WMT RX %d bytes: %*ph\n",
			 n, n, rx);
	if (n < evtlen)
		return -ETIMEDOUT;
	for (i = 0; i + evtlen <= n; i++)
		if (!memcmp(&rx[i], evt, evtlen))
			return 0;
	return -EBADMSG;
}

/* Run a named list of non-fatal reg-write commands (vendor
 * wmt_core_init_script equivalents where the vendor also only logs
 * failures). Build B: full-mode framing. */
static void wmt_run_script(struct device *dev, void __iomem *btif,
			   const char *what, const u8 *const cmds[],
			   const int lens[], int count)
{
	int i, ret;

	for (i = 0; i < count; i++) {
		ret = wmt_cmd_full(dev, btif, cmds[i], lens[i],
				   wmt_reg_evt, sizeof(wmt_reg_evt), false);
		if (ret)
			dev_err(dev, "consys-spike: %s cmd %d failed (%d) - continuing\n",
				what, i, ret);
	}
}

/* ===================== full-mode STP (Build B) =====================
 *
 * The vendor's reliable transport (stp_core.c). Differences from the
 * mand mode used pre-switch: header[0] = 0x80 | txseq<<3 | txack
 * (3-bit seq/ack, wrap 0-7), header[3] = (h0+h1+h2)&0xff checksum,
 * real CRC16 (table-driven, init 0, reflected 0xA001 = vendor
 * osal_crc16), the receiver ACKs every frame (4-byte header-only
 * frame 0x80|acked_seq | 0 0 | sum) and the sender retransmits
 * un-ACKed frames (MTKSTP_TX_TIMEOUT 180 ms, RETRY_LIMIT 10).
 * Sequence state after the mode switch: txseq=0, txack=7 (vendor
 * ctx init; H35 trace's first full frame 0x87 = 0x80+0<<3+7). */

static const u16 stp_crc16_table[256] = {
	0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
	0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
	0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
	0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
	0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
	0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
	0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
	0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
	0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
	0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
	0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
	0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
	0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
	0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
	0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
	0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
	0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
	0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
	0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
	0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
	0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
	0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
	0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
	0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
	0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
	0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
	0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
	0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
	0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
	0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
	0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
	0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
};

static u16 stp_crc16(const u8 *buf, int len)
{
	u16 crc = 0;
	int i;

	for (i = 0; i < len; i++)
		crc = (crc >> 8) ^ stp_crc16_table[(crc ^ buf[i]) & 0xff];
	return crc;
}

static u8 stp_txseq;	/* our frame sequence, wraps 0-7 */
static u8 stp_txack;	/* seq of last MCU data frame, echoed in TX h0 */

static void stp_seq_inc(u8 *idx)
{
	*idx = (*idx + 1) & 0x7;
}

static int stp_rx_byte(void __iomem *btif, unsigned long deadline)
{
	if (btif_dma_active)
		return btif_dma_rx_byte(deadline);
	while (!time_after(jiffies, deadline)) {
		if (readl(btif + BTIF_LSR) & BTIF_LSR_DR)
			return readl(btif + BTIF_RBR) & 0xff;
		usleep_range(50, 100);
	}
	return -1;
}

/* Discard whatever is left in the RX FIFO (after a parse error the
 * rest of a half-read frame would corrupt the next parse). */
static void btif_rx_flush(void __iomem *btif)
{
	u32 wpt;
	unsigned long t = jiffies + msecs_to_jiffies(20);

	if (btif_dma_active) {
		/* Ack everything currently in the RX vFIFO (RPT = WPT). */
		wpt = readl(rxdma_regs + BTIFDMA_VFF_WPT);
		rx_rpt = wpt & BTIFDMA_WPT_MASK;
		rx_rpt_wrap = wpt & BTIFDMA_WPT_WRAP;
		writel(wpt, rxdma_regs + BTIFDMA_VFF_RPT);
		return;
	}

	while (!time_after(jiffies, t)) {
		if (readl(btif + BTIF_LSR) & BTIF_LSR_DR)
			readl(btif + BTIF_RBR);
		else
			break;
	}
}

/* ACK a data frame received from the MCU (vendor stp_send_ack: the
 * 4-byte header-only frame 0x80|ackseq | 0 0 | sum). Mandatory - the
 * MCU's 7-frame TX window clogs without our acks (vendor pipelining). */
static void stp_ack_rx_frame(struct device *dev, void __iomem *btif,
			     u8 ackseq)
{
	u8 ack[4];

	ack[0] = 0x80 | (ackseq & 0x7);
	ack[1] = 0;
	ack[2] = 0;
	ack[3] = ack[0];
	btif_wakeup_consys(btif);
	btif_tx(dev, btif, ack, sizeof(ack));
}

/* Parse one STP frame off the RX byte stream.
 * Returns 0 for a header-only ACK frame (acked seq in *acked), >0 =
 * event payload length (frame acked back to the MCU, stp_txack
 * updated), <0 on error (RX already flushed by the caller's path). */
static int stp_rx_frame(struct device *dev, void __iomem *btif,
			unsigned long deadline, u8 *acked,
			u8 *payload, int payload_max)
{
	int h0, h1, h2, h3, i, b, len, tries, crcb;
	u16 crc, calc;

	for (tries = 0; tries < 256; tries++) {
		h0 = stp_rx_byte(btif, deadline);
		if (h0 < 0)
			return -ETIMEDOUT;
		if ((h0 & 0xc0) != 0x80)
			continue;		/* not a frame start; resync */
		h1 = stp_rx_byte(btif, deadline);
		h2 = stp_rx_byte(btif, deadline);
		h3 = stp_rx_byte(btif, deadline);
		if (h1 < 0 || h2 < 0 || h3 < 0)
			return -ETIMEDOUT;
		if (((h0 + h1 + h2) & 0xff) != h3)
			continue;		/* bad checksum; resync */
		len = ((h1 & 0x0f) << 8) | h2;
		if (len == 0) {
			*acked = h0 & 0x7;
			return 0;
		}
		if (len > payload_max)
			continue;		/* nonsense length; resync */
		for (i = 0; i < len; i++) {
			b = stp_rx_byte(btif, deadline);
			if (b < 0)
				return -ETIMEDOUT;
			payload[i] = b;
		}
		crcb = stp_rx_byte(btif, deadline);
		if (crcb < 0)
			return -ETIMEDOUT;
		calc = stp_rx_byte(btif, deadline);
		if (calc < 0)
			return -ETIMEDOUT;
		crc = crcb | ((u16)(calc & 0xff) << 8);
		if (stp_crc16(payload, len) != crc)
			continue;		/* bad CRC; resync */
		stp_txack = (h0 >> 3) & 0x7;
		stp_ack_rx_frame(dev, btif, stp_txack);
		return len;
	}
	return -EBADMSG;
}

/* Send one WMT command in full-mode STP framing: wait for the MCU's
 * header-only ACK of our seq AND (if evt given) the WMT event; 
 * retransmit the whole frame on timeout (vendor MTKSTP_RETRY_LIMIT).
 * Our commands are idempotent (same reasoning as the A2 comment). */
static int wmt_cmd_full(struct device *dev, void __iomem *btif,
			const u8 *cmd, int cmdlen, const u8 *evt, int evtlen,
			bool quiet)
{
	u8 *frame;
	u8 payload[G2B_RX_BUF_SZ];
	u16 crc;
	int i, ret, attempt;

	frame = kmalloc(STP_HDR_SIZE + cmdlen + STP_CRC_SIZE, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;

	for (attempt = 1;
	     attempt <= ((wmt_consecutive_noresp >= WMT_FASTFAIL_AFTER) ?
			   1 : WMT_MAX_ATTEMPTS);
	     attempt++) {
		unsigned long deadline;
		int got_ack = 0, got_evt = 0, plen;
		u8 acked;

		if (attempt > 1)
			dev_info(dev,
				 "consys-spike: WMT full retransmit %d/%d (no response)\n",
				 attempt - 1, WMT_MAX_ATTEMPTS - 1);

		frame[0] = 0x80 | ((stp_txseq & 0x7) << 3) | (stp_txack & 0x7);
		frame[1] = (WMT_TASK_INDX << 4) | ((cmdlen & 0xf00) >> 8);
		frame[2] = cmdlen & 0xff;
		frame[3] = (frame[0] + frame[1] + frame[2]) & 0xff;
		memcpy(&frame[STP_HDR_SIZE], cmd, cmdlen);
		crc = stp_crc16(cmd, cmdlen);
		frame[STP_HDR_SIZE + cmdlen] = crc & 0xff;
		frame[STP_HDR_SIZE + cmdlen + 1] = crc >> 8;

		if (!quiet)
			dev_info(dev,
				 "consys-spike: WMT full TX seq=%d ack=%d %*ph\n",
				 stp_txseq, stp_txack,
				 min(STP_HDR_SIZE + cmdlen + STP_CRC_SIZE, 32),
				 frame);

		btif_rx_flush(btif);		/* stale bytes from before */
		btif_wakeup_consys(btif);
		ret = btif_tx(dev, btif, frame,
			      STP_HDR_SIZE + cmdlen + STP_CRC_SIZE);
		if (ret)
			break;

		deadline = jiffies + msecs_to_jiffies(G2B_RX_TIMEOUT_MS);
		while (!time_after(jiffies, deadline)) {
			plen = stp_rx_frame(dev, btif, deadline, &acked,
					    payload, sizeof(payload));
			if (plen < 0) {
				btif_rx_flush(btif);
				ret = plen;
				break;
			}
			if (plen == 0) {
				if (acked == (stp_txseq & 0x7))
					got_ack = 1;
			} else {
				if (!quiet)
					dev_info(dev,
						 "consys-spike: WMT full RX %d bytes: %*ph\n",
						 plen, plen, payload);
				if (evt && !got_evt && plen >= evtlen)
					for (i = 0; i + evtlen <= plen; i++)
						if (!memcmp(&payload[i], evt,
							    evtlen)) {
							got_evt = 1;
							break;
						}
				}
			if (got_ack && (got_evt || !evt))
				break;
		}

		if (ret == 0 && time_after(jiffies, deadline))
			ret = -ETIMEDOUT;

		if (got_ack)
			wmt_consecutive_noresp = 0;
		else
			wmt_consecutive_noresp++;

		if (ret == 0) {
			stp_seq_inc(&stp_txseq);
			kfree(frame);
			return 0;
		}
	}
	kfree(frame);
	return ret ? ret : -ETIMEDOUT;
}

/* Push one WMT ROM-patch blob: the two patch-address reg-writes, then
 * the body (file minus 28-byte header) as 1000-byte WMT_PATCH
 * fragments, then WMT_RESET (vendor mtk_wcn_soc_patch_dwn() +
 * init_table_3). */
static int wmt_push_patch(struct device *dev, void __iomem *btif,
			  const struct firmware *fw, const char *name)
{
	u8 p_address_cmd[sizeof(wmt_patch_p_address_cmd)];
	u8 patch_cmd[5] = { 0x01, 0x01, 0x00, 0x00, 0x00 };
	u8 *frag;
	const u8 *body;
	u32 body_len, off, frag_len, frag_num, frag_seq;
	u16 cmd_len;
	int ret;

	if (fw->size <= WMT_PATCH_HDR_SIZE) {
		dev_err(dev, "consys-spike: %s too small (%zu)\n",
			name, fw->size);
		return -EINVAL;
	}
	dev_info(dev, "consys-spike: pushing %s: %zu bytes, hdr info %*ph\n",
		 name, fw->size, 4, fw->data + WMT_PATCH_INFO_OFF);

	ret = wmt_cmd_full(dev, btif, wmt_patch_address_cmd,
			   sizeof(wmt_patch_address_cmd), wmt_reg_evt,
			   sizeof(wmt_reg_evt), false);
	if (ret) {
		dev_err(dev, "consys-spike: patch-address cmd failed (%d)\n",
			ret);
		return ret;
	}

	/* Per-patch RAM address = header bytes 24..27 with byte 24
	 * zeroed (launcher srh_patch()), placed in the value field. */
	memcpy(p_address_cmd, wmt_patch_p_address_cmd, sizeof(p_address_cmd));
	memcpy(&p_address_cmd[12], fw->data + WMT_PATCH_INFO_OFF, 4);
	p_address_cmd[12] = 0x00;
	ret = wmt_cmd_full(dev, btif, p_address_cmd, sizeof(p_address_cmd),
			   wmt_reg_evt, sizeof(wmt_reg_evt), false);
	if (ret) {
		dev_err(dev, "consys-spike: part-patch-address cmd failed (%d)\n",
			ret);
		return ret;
	}

	body = fw->data + WMT_PATCH_HDR_SIZE;
	body_len = fw->size - WMT_PATCH_HDR_SIZE;
	frag_num = DIV_ROUND_UP(body_len, WMT_PATCH_FRAG_SIZE);
	dev_info(dev, "consys-spike: %s body %u bytes, %u fragments\n",
		 name, body_len, frag_num);

	frag = kmalloc(sizeof(patch_cmd) + WMT_PATCH_FRAG_SIZE, GFP_KERNEL);
	if (!frag)
		return -ENOMEM;

	for (frag_seq = 0, off = 0; frag_seq < frag_num;
	     frag_seq++, off += WMT_PATCH_FRAG_SIZE) {
		frag_len = min(body_len - off, (u32)WMT_PATCH_FRAG_SIZE);
		cmd_len = 1 + frag_len;
		patch_cmd[2] = cmd_len & 0xff;
		patch_cmd[3] = cmd_len >> 8;
		if (frag_seq == frag_num - 1)
			patch_cmd[4] = WMT_PATCH_FRAG_LAST;
		else
			patch_cmd[4] = frag_seq ? WMT_PATCH_FRAG_MID
						: WMT_PATCH_FRAG_1ST;
		memcpy(frag, patch_cmd, sizeof(patch_cmd));
		memcpy(frag + sizeof(patch_cmd), body + off, frag_len);

		ret = wmt_cmd_full(dev, btif, frag,
				  sizeof(patch_cmd) + frag_len,
				  wmt_patch_evt, sizeof(wmt_patch_evt),
				  true);
		if (ret) {
			dev_err(dev, "consys-spike: %s fragment %u/%u failed (%d)\n",
				name, frag_seq + 1, frag_num, ret);
			kfree(frag);
			return ret;
		}
		if (!(frag_seq % 32) || frag_seq == frag_num - 1)
			dev_info(dev, "consys-spike: %s fragment %u/%u ok\n",
				 name, frag_seq + 1, frag_num);
	}
	kfree(frag);

	ret = wmt_cmd_full(dev, btif, wmt_reset_cmd, sizeof(wmt_reset_cmd),
			   wmt_reset_evt, sizeof(wmt_reset_evt), false);
	if (ret)
		dev_err(dev, "consys-spike: post-patch WMT_RESET failed (%d)\n",
			ret);
	return ret;
}

/* Full vendor-order firmware push: DLM power-on regs, MCU clock up,
 * both patches in download-seq order (Build E: NO clock restore -
 * the down-clock kills the PIO link), final query (the re-scoped Gate
 * G2b, run by the caller). Build D: one full init attempt - mand
 * QUERY_STP link check, SET_STP mode switch, full-mode phase, push.
 * The caller (gate G2b) retries the whole thing after an MCU reset
 * cycle when it fails (the clock-up transition is a coin flip and a
 * failed one leaves the MCU's WMT RX deaf for the rest of the boot).
 */
static int wmt_init_and_push(struct device *dev, void __iomem *btif)
{
	static const u8 *const dlm_cmds[] = {
		wmt_dlm_cmd1, wmt_dlm_cmd2, wmt_dlm_cmd3
	};
	static const int dlm_lens[] = {
		sizeof(wmt_dlm_cmd1), sizeof(wmt_dlm_cmd2),
		sizeof(wmt_dlm_cmd3)
	};
	/* set_mcuclk_table_3/4 (icId 0x0279) - H35 trace confirms the
	 * order: DLM x3, then EN, RATIO, DIV, HCLK, all full-mode. */
	static const u8 *const clk_up_cmds[] = {
		wmt_mcuclk_en, wmt_mcuclk_ratio, wmt_mcuclk_div,
		wmt_mcuclk_hclk
	};
	static const int clk_up_lens[] = {
		sizeof(wmt_mcuclk_en), sizeof(wmt_mcuclk_ratio),
		sizeof(wmt_mcuclk_div), sizeof(wmt_mcuclk_hclk)
	};
	/* set_mcuclk_table_4 (icId 0x0279): HCLK_DIS, RATIO_DIS, DIS. */
	static const u8 *const clk_dn_cmds[] = {
		wmt_mcuclk_hclk_dis, wmt_mcuclk_ratio_dis, wmt_mcuclk_dis
	};
	static const int clk_dn_lens[] = {
		sizeof(wmt_mcuclk_hclk_dis), sizeof(wmt_mcuclk_ratio_dis),
		sizeof(wmt_mcuclk_dis)
	};
	const struct firmware *fw;
	int i, ret, push_ret = 0;

	dev_info(dev, "consys-spike: WMT init attempt: mand QUERY_STP link check\n");
	ret = wmt_cmd_evt(dev, btif, 0, wmt_query_stp_cmd,
			   sizeof(wmt_query_stp_cmd), wmt_query_stp_evt,
			   sizeof(wmt_query_stp_evt), false);
	if (ret) {
		dev_info(dev, "consys-spike: ROM-only query FAIL (%d) - attempt unusable\n", ret);
		return ret;
	}

	/* Build B: the vendor runs the push in FULL-MODE STP (H35
	 * trace: seq+ack headers, per-frame 4-byte ACKs, CRC16,
	 * retransmit). Switch: SET_STP (mand) -> 10 ms settle -> host
	 * full mode, exactly per vendor sw_init (wmt_ic_soc.c). */
	ret = wmt_cmd_evt(dev, btif, 1, wmt_set_stp_cmd,
			   sizeof(wmt_set_stp_cmd), wmt_set_stp_evt,
			   sizeof(wmt_set_stp_evt), false);
	if (ret) {
		dev_err(dev, "consys-spike: SET_STP failed (%d) - full mode unavailable, aborting attempt\n", ret);
		return ret;
	}
	msleep(10);	/* vendor: "enough for chip do mechanism switch" */

	stp_txseq = 0;
	stp_txack = 7;
	dev_info(dev, "consys-spike: host switched to full-mode STP (txseq=0 txack=7)\n");

	ret = wmt_cmd_full(dev, btif, wmt_query_stp_cmd,
			   sizeof(wmt_query_stp_cmd), wmt_query_stp_evt_hdr,
			   sizeof(wmt_query_stp_evt_hdr), false);
	if (ret)
		dev_err(dev, "consys-spike: full-mode QUERY_STP failed (%d) - continuing\n", ret);

	wmt_run_script(dev, btif, "power-on-dlm", dlm_cmds, dlm_lens,
		       ARRAY_SIZE(dlm_cmds));

	/* Build C: the ROM's full-mode path for the 1011-byte patch
	 * frames is deaf at 26 MHz (#298: control frames fine, fragment
	 * silence); the golden trace pushes at 138 MHz (DLM x3 then
	 * mcuclk-up x4, both full-mode). Re-enable the vendor's
	 * set_mcuclk_table_3; full-mode retransmit now covers the
	 * transition risk that killed the mand-mode speed-ups. */
	wmt_run_script(dev, btif, "mcuclk-up", clk_up_cmds, clk_up_lens,
		       ARRAY_SIZE(clk_up_cmds));

	for (i = 0; i < ARRAY_SIZE(wmt_patch_names); i++) {
		ret = request_firmware(&fw, wmt_patch_names[i], dev);
		if (ret) {
			dev_err(dev, "consys-spike: request_firmware(%s) failed (%d) - is CONFIG_EXTRA_FIRMWARE set?\n",
				wmt_patch_names[i], ret);
			return ret;
		}
		ret = wmt_push_patch(dev, btif, fw, wmt_patch_names[i]);
		release_firmware(fw);
		if (ret) {
			push_ret = ret;
			break;
		}
	}

	/* Build B2: the vendor restores 26 MHz here (set_mcuclk_table_4).
	 * In PIO this down-clock killed the link (#295 b1/b3, #300 c2)
	 * while the vendor's DMA-mode trace ACKs the identical RATIO_DIS
	 * in 0.36 ms (H35 t=105.784) - with Build B's DMA transport
	 * the restore should now be reliable: the full vendor-faithful
	 * init, MCU ending at 26 MHz (low-power steady state). Only on
	 * success - after a failed attempt the next cycle resets the MCU
	 * anyway. */
	if (!push_ret)
		wmt_run_script(dev, btif, "mcuclk-restore", clk_dn_cmds,
				clk_dn_lens, ARRAY_SIZE(clk_dn_cmds));
	return push_ret;
}

/* Assert (hold) the CONSYS MCU reset (AP_RGU, key-gated). Pair with
 * consys_mcu_reset_release(). Holding the MCU in reset while the
 * AP-side BTIF is re-initialized is the G2b order (see
 * consys_mcu_power_cycle): releasing first lets the ROM's one-shot
 * BTIF init run against a BTIF that still carries the previous
 * session's state, and its RX comes up deaf. */
/* B-22 (2026-09-06): clear the shared EMI window while the MCU is
 * held in reset. Hypothesis: the WMT_RESET-armed patch stays in the
 * shared MCU RAM across SWSYSRST (SRAM is not in the reset scope),
 * and the ROM's boot path jumps back to it after any reset (evidence:
 * post-hand-off resets show the MCU executing at 0x2xxxxx-0x4xxxxx /
 * fault-looping at 0x172c8 instead of the bare-ROM wait state, and
 * never answering mand-mode WMT). G2b's fresh boots work because the
 * window starts clean. Dump 16 words at +0x10000 (suspected patch
 * start if the MCU maps this 1 MB at 0x80000: patched CPUPCR was
 * 0x9b5d6) as evidence, then zero the whole region. */
static void consys_emi_clear_fw(struct device *dev)
{
	void __iomem *win;
	u32 w[16];
	int i;

	if (!g_emi_base)
		return;
	win = ioremap(g_emi_base, g_emi_size);
	if (!win)
		return;
	for (i = 0; i < 16; i++)
		w[i] = readl(win + 0x10000 + i * 4);
	dev_info(dev, "consys-spike: EMI clear: +%#x w0-3 = %08x %08x %08x %08x\n",
		 0x10000, w[0], w[1], w[2], w[3]);
	dev_info(dev, "consys-spike: EMI clear: +%#x w4-7 = %08x %08x %08x %08x\n",
		 0x10010, w[4], w[5], w[6], w[7]);
	memset_io(win, 0, g_emi_size);
	dev_info(dev, "consys-spike: EMI shared window cleared (%zu bytes)\n",
		 g_emi_size);
	iounmap(win);
}

static void consys_mcu_reset_hold(struct device *dev)
{
	consys_poke(AP_RGU_SWSYSRST_PA, AP_RGU_CONN_MCU_RST, AP_RGU_KEY,
		     false, 0, dev, "AP_RGU_SWSYSRST(assert)");
	msleep(50);
	consys_emi_clear_fw(dev);
}

static void consys_mcu_reset_release(struct device *dev)
{
	consys_poke_clear(AP_RGU_SWSYSRST_PA, AP_RGU_CONN_MCU_RST,
			  AP_RGU_KEY, dev, "AP_RGU_SWSYSRST(release)");
	msleep(50);
}

/* ===================== WMT transport (mtk_wcn module hand-off) =====================
 *
 * After G2b the link is owned by the live WMT client below. The mtk_wcn
 * module (the ported vendor WMT protocol core, drivers/misc/mediatek-
 * connectivity) takes the same link over via the ops here (see
 * include/linux/consys_wmt.h): its STP core produces complete STP
 * frames that tx() writes raw to the BTIF, and its STP parser consumes
 * the RX byte stream delivered by the kthread below.
 *
 * The MCU's STP state machine only returns to a known state on two
 * events: a CONN-domain power cycle (fresh boot #1 of a power-on:
 * bare ROM, mand mode) and SET_STP (the mand->full switch, which
 * resets the full-mode seq state to 0). A plain AP_RGU SWSYSRST is
 * NOT one of them once the ROM patch has run (B-25, 2026-09-06: the
 * reset does not clear the MCU SRAM the patch executes from, so the
 * ROM re-detects the patch and runs it with its WMT/BTIF state
 * destroyed - the MCU is deaf to mand-mode WMT for the rest of the
 * power-on). The WMT core's init always starts in mand mode, so
 * handing the link over first POWER-CYCLES the CONN domain (B-27:
 * consys_mcu_power_cycle, the only in-boot way back to a pre-patch
 * MCU); the core then runs the full vendor init itself (reg-reads,
 * SET_STP, DLM, ROM patch push, 26 MHz restore). Releasing the link
 * (cb == NULL) does NOT reset the MCU - it stays patched in full-mode
 * STP - so the live client above is re-synced to the MCU's current
 * STP sequence state instead (full-mode probe + dup-ACK seq jump,
 * wmt_live_resync_full): the mand re-init handshake cannot reach a
 * full-mode MCU.
 */

static enum { WMT_OWN_LIVE = 0, WMT_OWN_CORE } wmt_owner = WMT_OWN_LIVE;
static int (*wmt_rx_cb)(const u8 *buf, unsigned int len);
static struct task_struct *wmt_rxd;

static u32 wmt_cpupcr_read(void)
{
	void __iomem *pcr = ioremap(CONSYS_CPUPCR_PA, 4);
	u32 v;

	if (!pcr)
		return 0;
	v = readl(pcr);
	iounmap(pcr);
	return v;
}

/* SPM CONN MTCMOS control register (SPM + 0x32C): the ground truth
 * for the domain power state - ~0x10D running (golden vendor value
 * with WiFi up), the off pattern clears the PWR_ON bits. Logged
 * around the B-27 power cycle. */
#define SPM_CONN_PWR_CON_PA	0x1000632CUL

static u32 conn_pwr_con_read(void)
{
	void __iomem *r = ioremap(SPM_CONN_PWR_CON_PA, 4);
	u32 v = 0;

	if (!r)
		return 0;
	v = readl(r);
	iounmap(r);
	return v;
}

/* B-25 post-reset link test (kept as the B-27 power-cycle
 * verification): the spike's own G2b reg-read to the just-(re)booted
 * MCU. A fresh power-on boot #1 answers in mand mode with chip id
 * 0x279; a deaf MCU returns nothing (the pre-B-27 SWSYSRST
 * signature). Diagnostic only - the hand-off proceeds either way and
 * the WMT core's own hw_check REG_CMD is the real gate. */
static void wmt_link_test(const char *tag)
{
	u32 chip = 0;
	int r;

	r = wmt_reg_read(wmt_dev, wmt_btif, GEN_HCR_ADDR, GEN_VER_MASK,
			 &chip);
	dev_info(wmt_dev,
		 "consys-spike: %s link test: HCR read %s (chip 0x%08x)\n",
		 tag, r ? "FAIL" : "OK", chip);
}

/* B-27 (2026-09-06): power-cycle the CONN scpsys domain instead of an
 * SWSYSRST for the hand-off reset. Caller holds wmt_mtx.
 *
 * Why (see the B-25 analysis in docs/handover-2026-09-06-b.md): the
 * MCU's MAND-mode WMT RX is boot-scoped to pre-patch boots. Once the
 * ROM patch has executed (from MCU-private SRAM that AP_RGU SWSYSRST
 * does not clear), every subsequent SWSYSRST leaves the MCU deaf for
 * the rest of the power-on. A CONN-domain power cycle (pm_runtime
 * put/get on the scpsys genpd) drops the MTCMOS: the MCU SRAM is
 * cleared, so the next boot is boot #1 of a fresh power-on - bare
 * ROM, mand responder, exactly the G2a->G2b state.
 *
 * Sequence (mirrors the probe's power-on path + G2b order): domain
 * off (pm_runtime put) -> 50 ms -> verify the domain truly dropped
 * (else bail with the MCU untouched - an SWSYSRST on a still-patched
 * MCU is the deafening path) -> SWSYSRST assert while unpowered
 * (probe order; no bus master is live at the power-up transition, so
 * no TOPAXI bus protection is needed - the scpsys CONN bus_prot_mask
 * is removed for the same reason, see mtk-scpsys.c mt6797 data) ->
 * domain on (get) -> 100 ms settle -> re-apply the domain-scoped AP
 * writes the probe applies at power-on (CONN2AP sleep mask, EMI remap
 * + fw-window zero, BTIF hw + DMA init) -> release -> 300 ms settle
 * -> link test. Returns 0 on success, -errno otherwise. */
static int consys_mcu_power_cycle(void)
{
	struct device *dev = wmt_dev;
	void __iomem *btif = wmt_btif;
	int ret;

	if (!dev || !btif)
		return -ENODEV;

	dev_info(dev,
		 "consys-spike: B-27 CONN power cycle start (PWR_CON=%#x CPUPCR=%#x rpm_usage=%d)\n",
		 conn_pwr_con_read(), wmt_cpupcr_read(),
		 atomic_read(&dev->power.usage_count));

	ret = pm_runtime_put_sync(dev);
	if (ret)
		dev_info(dev,
			 "consys-spike: B-27 pm_runtime_put_sync -> %d (continuing)\n",
			 ret);
	msleep(50);
	dev_info(dev,
		 "consys-spike: B-27 CONN off (PWR_CON=%#x rpm_suspended=%d)\n",
		 conn_pwr_con_read(), pm_runtime_suspended(dev));
	if (!pm_runtime_suspended(dev)) {
		/* The domain did not drop (e.g. another runtime-PM user).
		 * Bail BEFORE touching the MCU: an SWSYSRST on a still-
		 * patched MCU is the deafening path. The MCU is untouched
		 * (still patched, healthy in full mode) and the core's
		 * hw_check failure will release the link cleanly. */
		dev_err(dev,
			 "consys-spike: B-27 CONN domain did not power off - aborting power cycle (MCU left as-is)\n");
		return -EIO;
	}

	/* Domain is down (MCU SRAM cleared). Assert SWSYSRST while the
	 * MCU is unpowered (probe order: assert before power-on) so it
	 * boots held when power returns: nothing runs during the AP-side
	 * re-init and no bus master is live at the power-up transition -
	 * what makes an unprotected power-off safe (see mtk-scpsys.c CONN
	 * comment). consys_mcu_reset_hold also does the B-22 EMI clear. */
	consys_mcu_reset_hold(dev);

	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev,
			 "consys-spike: B-27 CONN domain power-on failed after put (%d)\n",
			 ret);
		return ret;
	}
	msleep(100);
	dev_info(dev, "consys-spike: B-27 CONN on (PWR_CON=%#x)\n",
		 conn_pwr_con_read());

	/* G2b order continued: with the MCU still held in reset, re-apply
	 * everything the probe's power-on path applied (the BTIF must be
	 * pristine BEFORE the ROM boots its one-shot BTIF init against it -
	 * releasing first is the pre-2026-09-06 deafness). */
	consys_poke(CONN2AP_SLEEP_MASK_PA, CONN2AP_SLEEP_MASK_BIT, 0,
		    false, 0, dev, "CONN2AP_SLEEP_MASK");
	consys_emi_setup(dev);
	btif_hw_init(btif);
	if (btif_dma_init(dev, btif))
		dev_info(dev,
			 "consys-spike: B-27 BTIF staying in PIO mode (DMA init failed)\n");
	consys_mcu_reset_release(dev);
	msleep(300);	/* let the ROM boot + its one-shot BTIF init run */

	dev_info(dev,
		 "consys-spike: B-27 power cycle done (CPUPCR=%#x DMA_EN=%#x)\n",
		 wmt_cpupcr_read(), readl(btif + BTIF_DMA_EN));
	wmt_link_test("B-27 post-power-cycle");
	return 0;
}

/* B-27 release-path resync (no MCU reset): the MCU stays patched in
 * full-mode STP, so its STP expected-rxseq state is wherever the WMT
 * core's session left it (unknown to this driver - the core runs its
 * own seq counter). Full-mode STP self-resyncs: the chip responds to
 * any well-formed out-of-window frame with a 4-byte dup-ACK whose low
 * 3 bits are its last accepted seq (stp_core.c stp_process_packet),
 * so probe with QUERY_STP and on a dup-ACK jump our txseq to ack+1
 * and re-send until the chip accepts. The mand-framed re-init cannot
 * reach a full-mode MCU (mand h3=0 frames fail the full parser's
 * checksum). Non-fatal: on failure the live client stays down until
 * the next pwr-on/off cycle (or a reboot). */
static int wmt_live_resync_full(void)
{
	u8 payload[G2B_RX_BUF_SZ];
	u8 *frame;
	u16 crc;
	int attempt, ret = -ETIMEDOUT;

	frame = kmalloc(STP_HDR_SIZE + sizeof(wmt_query_stp_cmd) +
			STP_CRC_SIZE, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;

	dev_info(wmt_dev,
		 "consys-spike: live client resync (MCU stays patched, full mode; stp txseq=%d txack=%d)\n",
		 stp_txseq, stp_txack);

	for (attempt = 0; attempt < 8; attempt++) {
		unsigned long t = jiffies + msecs_to_jiffies(250);
		bool saw_ack = false, saw_data = false, accepted = false;
		u8 acked = 0xff;

		frame[0] = 0x80 | ((stp_txseq & 0x7) << 3) |
			(stp_txack & 0x7);
		frame[1] = (WMT_TASK_INDX << 4) |
			((sizeof(wmt_query_stp_cmd) & 0xf00) >> 8);
		frame[2] = sizeof(wmt_query_stp_cmd);
		frame[3] = (frame[0] + frame[1] + frame[2]) & 0xff;
		memcpy(&frame[STP_HDR_SIZE], wmt_query_stp_cmd,
		       sizeof(wmt_query_stp_cmd));
		crc = stp_crc16(wmt_query_stp_cmd,
				sizeof(wmt_query_stp_cmd));
		frame[STP_HDR_SIZE + sizeof(wmt_query_stp_cmd)] =
			crc & 0xff;
		frame[STP_HDR_SIZE + sizeof(wmt_query_stp_cmd) + 1] =
			crc >> 8;

		dev_info(wmt_dev, "consys-spike: resync probe %d: full TX seq=%d ack=%d\n",
			 attempt, stp_txseq, stp_txack);
		btif_rx_flush(wmt_btif);
		btif_wakeup_consys(wmt_btif);
		ret = btif_tx(wmt_dev, wmt_btif, frame,
			      STP_HDR_SIZE + sizeof(wmt_query_stp_cmd) +
			      STP_CRC_SIZE);
		if (ret)
			break;

		while (!time_after(jiffies, t)) {
			/* Once the chip has answered (in-window ack or a
			 * data frame), a short idle means it is done - don't
			 * burn the whole budget waiting for a response that
			 * is not coming. */
			unsigned long d = (saw_ack || saw_data) ?
				min(t, jiffies + msecs_to_jiffies(60)) : t;
			int plen = stp_rx_frame(wmt_dev, wmt_btif,
						 d, &acked, payload,
						 sizeof(payload));

			if (plen < 0)
				break;
			if (plen == 0) {
				/* header-only ack; in-window iff it
				 * echoes our sent seq */
				if (acked == (stp_txseq & 0x7))
					saw_ack = true;
			} else {
				/* data frame (the QUERY_STP event): the
				 * chip processed our probe */
				saw_data = true;
				dev_info(wmt_dev,
					 "consys-spike: resync QUERY_STP RX %d bytes: %*ph\n",
					 plen, plen, payload);
			}
		}
		accepted = saw_ack || saw_data;
		if (accepted) {
			stp_seq_inc(&stp_txseq);
			dev_info(wmt_dev,
				 "consys-spike: live client re-synced to the patched full-mode MCU (txseq=%d txack=%d)\n",
				 stp_txseq, stp_txack);
			kfree(frame);
			return 0;
		}
		if (acked != 0xff) {
			/* dup-ACK of the chip's last accepted seq: jump
			 * our txseq there and re-probe */
			stp_txseq = (acked + 1) & 0x7;
			dev_info(wmt_dev,
				 "consys-spike: resync dup-ACK acked=%d - jumping txseq to %d\n",
				 acked, stp_txseq);
			continue;
		}
		break;	/* silence: not a full-mode responder */
	}
	kfree(frame);
	dev_err(wmt_dev,
		"consys-spike: live client resync FAIL (%d) - live client down until the next pwr cycle\n",
		ret);
	return ret;
}

static int wmt_ops_tx(const u8 *buf, int len)
{
	if (!wmt_ready || !wmt_btif)
		return -ENETDOWN;
	if (wmt_owner != WMT_OWN_CORE)
		return -EBUSY;
	if (len <= 0 || len > WMT_LIVE_RESP_SZ)
		return -EINVAL;
	return btif_tx(wmt_dev, wmt_btif, buf, len);
}

static int wmt_ops_rx_cb_register(int (*cb)(const u8 *buf, unsigned int len))
{
	mutex_lock(&wmt_mtx);
	if (!wmt_ready || !wmt_btif) {
		mutex_unlock(&wmt_mtx);
		return -ENETDOWN;
	}
	if (cb) {
		/* Hand the link to the WMT core: power-cycle the CONN domain
		 * so the MCU is at boot #1 of a fresh power-on (bare ROM,
		 * mand mode - the state the core's init expects). */
		consys_mcu_power_cycle();
		btif_rx_flush(wmt_btif);	/* bytes the ROM sent during boot */
		wmt_rx_cb = cb;
		wmt_owner = WMT_OWN_CORE;
		dev_info(wmt_dev, "consys-spike: WMT link handed to the mtk_wcn core (CONN power-cycled, MCU at fresh boot #1)\n");
	} else {
		/* Release the link back to the live client: NO MCU reset -
		 * the MCU stays patched in full mode (a SWSYSRST now would
		 * deafen it for the rest of the power-on). Resync the live
		 * client's STP seq state to the MCU's instead. */
		wmt_rx_cb = NULL;
		wmt_owner = WMT_OWN_LIVE;
		dev_info(wmt_dev, "consys-spike: WMT link released back to the live client (MCU NOT reset - stays patched full-mode)\n");
		wmt_live_resync_full();
	}
	mutex_unlock(&wmt_mtx);
	return 0;
}

static void wmt_ops_rx_flush(void)
{
	if (wmt_btif)
		btif_rx_flush(wmt_btif);
}

static int wmt_ops_mcu_reset(void)
{
	if (!wmt_ready || !wmt_btif)
		return -ENETDOWN;
	mutex_lock(&wmt_mtx);
	consys_mcu_power_cycle();
	mutex_unlock(&wmt_mtx);
	return 0;
}

static bool wmt_ops_ready(void)
{
	return wmt_ready;
}

static int wmt_ops_wake(void)
{
	if (!wmt_ready || !wmt_btif)
		return -ENETDOWN;
	mutex_lock(&wmt_mtx);
	btif_wakeup_consys(wmt_btif);
	mutex_unlock(&wmt_mtx);
	return 0;
}

static const struct consys_wmt_ops consys_wmt_ops_impl = {
	.tx = wmt_ops_tx,
	.rx_cb_register = wmt_ops_rx_cb_register,
	.rx_flush = wmt_ops_rx_flush,
	.mcu_reset = wmt_ops_mcu_reset,
	.ready = wmt_ops_ready,
	.wake = wmt_ops_wake,
};
const struct consys_wmt_ops *consys_wmt_transport = &consys_wmt_ops_impl;
EXPORT_SYMBOL(consys_wmt_transport);

/* B-30: VCN33 BT/WiFi sub-rail control for the mtk_wcn module's PALDO
 * ops (type 0 = BT rail, 1 = WiFi rail; on 1 = enable, 0 = disable).
 * The rails are fetched at probe and stay OFF until the WMT core's
 * sw_init powers them up for the RF-calibration exchange. Returns 0
 * on success, -ENODEV if the supply is missing from the DT. */
int consys_paldo_ctrl(int type, int on)
{
	struct regulator *r = type ? g_vcn33_wifi : g_vcn33_bt;
	int ret;

	if (!r)
		return -ENODEV;
	ret = on ? regulator_enable(r) : regulator_disable(r);
	if (ret)
		dev_warn(wmt_dev,
			 "consys-spike: vcn33-%s %s failed (%d)\n",
			 type ? "wifi" : "bt", on ? "on" : "off", ret);
	else
		dev_info(wmt_dev, "consys-spike: vcn33-%s %s\n",
			 type ? "wifi" : "bt", on ? "on" : "off");
	return ret;
}
EXPORT_SYMBOL(consys_paldo_ctrl);
EXPORT_SYMBOL(g_emi_base);

/* RX consumer for the WMT core (vendor btif_rx_thread equivalent,
 * polling): while the core owns the link, drain the RX vFIFO and hand
 * the bytes to its STP parser. The 50 ms poll keeps the STP 180 ms
 * TX-timeout budget comfortable. No idle wait: a 30 ms idle per reply
 * made the ROM patch push ~10 s (40 ms/fragment), past the stock 4 s
 * Wi-Fi func-on wait on a re-power-on (opId(3) timeout). */
#define WMT_RXD_POLL_MS		50
#define WMT_RXD_BUF_SZ		(4 * 1024)

static int consys_wmtrxd(void *data)
{
	u8 *buf = kmalloc(WMT_RXD_BUF_SZ, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;
	while (!kthread_should_stop()) {
		int (*cb)(const u8 *, unsigned int);
		bool ready;
		int n;

		mutex_lock(&wmt_mtx);
		cb = (wmt_owner == WMT_OWN_CORE) ? wmt_rx_cb : NULL;
		ready = wmt_ready && wmt_btif != NULL;
		mutex_unlock(&wmt_mtx);

		if (cb && ready) {
			n = btif_rx_drain(wmt_btif, buf, WMT_RXD_BUF_SZ,
					  WMT_RXD_POLL_MS, 0);
			if (n > 0)
				cb(buf, n);
			usleep_range(2000, 5000);
		} else {
			schedule_timeout_interruptible(msecs_to_jiffies(20));
		}
	}
	kfree(buf);
	return 0;
}

/* ---- Live WMT client: raw command + response capture ---- */

/* Send one full-mode STP WMT command and capture the raw response
 * payload (all data frames until a short idle or the hard timeout).
 * Returns the number of payload bytes captured (0 = ack only, no data
 * frame) or -errno. The command is consumed into the STP frame before
 * any response is written, so cmd/resp may alias. */
static int wmt_raw_cmd(struct device *dev, void __iomem *btif,
		       const u8 *cmd, int cmdlen, u8 *resp, int resplen)
{
	u8 *frame;
	u8 payload[G2B_RX_BUF_SZ];
	u16 crc;
	int ret, got = 0;

	if (cmdlen <= 0 || cmdlen > 255 || resplen < 0)
		return -EINVAL;

	frame = kmalloc(STP_HDR_SIZE + cmdlen + STP_CRC_SIZE, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;

	frame[0] = 0x80 | ((stp_txseq & 0x7) << 3) | (stp_txack & 0x7);
	frame[1] = (WMT_TASK_INDX << 4) | ((cmdlen & 0xf00) >> 8);
	frame[2] = cmdlen & 0xff;
	frame[3] = (frame[0] + frame[1] + frame[2]) & 0xff;
	memcpy(&frame[STP_HDR_SIZE], cmd, cmdlen);
	crc = stp_crc16(cmd, cmdlen);
	frame[STP_HDR_SIZE + cmdlen] = crc & 0xff;
	frame[STP_HDR_SIZE + cmdlen + 1] = crc >> 8;

	dev_info(dev, "consys-wmt: TX seq=%d ack=%d len=%d %*ph\n",
		 stp_txseq, stp_txack, cmdlen, min(cmdlen, 32), cmd);

	btif_rx_flush(btif);
	btif_wakeup_consys(btif);
	ret = btif_tx(dev, btif, frame, STP_HDR_SIZE + cmdlen + STP_CRC_SIZE);
	kfree(frame);
	if (ret)
		return ret;

	{
		unsigned long deadline = jiffies + msecs_to_jiffies(G2B_RX_TIMEOUT_MS);
		bool got_ack = false, got_frame = false;

		while (!time_after(jiffies, deadline) && got < resplen) {
			unsigned long d = deadline;
			int plen;
			u8 acked;

			/* Once the MCU has acked (or sent a data frame), a short
			 * idle means it is done: don't wait the full timeout. The
			 * ack and the event come back-to-back in one burst. */
			if (got_ack || got_frame)
				d = min(d, jiffies + msecs_to_jiffies(30));
			plen = stp_rx_frame(dev, btif, d, &acked,
					    payload, sizeof(payload));
			if (plen < 0)
				break;
			if (plen == 0) {
				if (acked == (stp_txseq & 0x7))
					got_ack = true;
				continue;	/* header-only ack; keep waiting */
			}
			/* A data frame means the MCU processed our command. */
			got_ack = true;
			got_frame = true;
			dev_info(dev, "consys-wmt: RX %d bytes: %*ph\n",
				 plen, plen, payload);
			if (got + plen > resplen)
				plen = resplen - got;
			memcpy(&resp[got], payload, plen);
			got += plen;
		}

		/* Advance our STP sequence ONLY if the MCU acked, exactly like
		 * wmt_cmd_full(): a duplicate seq is ignored by the MCU, and a
		 * genuine timeout must retransmit the same seq. */
		if (got_ack) {
			stp_seq_inc(&stp_txseq);
			return got;
		}
	}
	return -ETIMEDOUT;
}

static ssize_t consys_wmt_write(struct file *filp, const char __user *buf,
				size_t count, loff_t *f_pos)
{
	if (count == 0 || count > WMT_LIVE_RESP_SZ)
		return -EINVAL;
	mutex_lock(&wmt_mtx);
	if (!wmt_ready || !wmt_btif) {
		mutex_unlock(&wmt_mtx);
		return -ENETDOWN;
	}
	if (wmt_owner != WMT_OWN_LIVE) {
		mutex_unlock(&wmt_mtx);
		return -EBUSY;	/* link owned by the mtk_wcn core */
	}
	if (copy_from_user(wmt_cmd, buf, count)) {
		mutex_unlock(&wmt_mtx);
		return -EFAULT;
	}
	wmt_resp_len = wmt_raw_cmd(wmt_dev, wmt_btif, wmt_cmd, (int)count,
				   wmt_resp, sizeof(wmt_resp));
	*f_pos = 0;
	mutex_unlock(&wmt_mtx);
	return count;
}

static ssize_t consys_wmt_read(struct file *filp, char __user *buf,
			       size_t count, loff_t *f_pos)
{
	mutex_lock(&wmt_mtx);
	if (!wmt_ready || !wmt_btif) {
		mutex_unlock(&wmt_mtx);
		return -ENETDOWN;
	}
	if (wmt_owner != WMT_OWN_LIVE) {
		mutex_unlock(&wmt_mtx);
		return -EBUSY;	/* link owned by the mtk_wcn core */
	}
	if (*f_pos >= (loff_t)wmt_resp_len) {
		mutex_unlock(&wmt_mtx);
		return 0;	/* EOF for this response */
	}
	if ((long)count > wmt_resp_len - *f_pos)
		count = wmt_resp_len - *f_pos;
	if (copy_to_user(buf, &wmt_resp[*f_pos], count)) {
		mutex_unlock(&wmt_mtx);
		return -EFAULT;
	}
	*f_pos += count;
	mutex_unlock(&wmt_mtx);
	return count;
}

static const struct file_operations consys_wmt_fops = {
	.owner = THIS_MODULE,
	.write = consys_wmt_write,
	.read = consys_wmt_read,
};

static struct miscdevice consys_wmt_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "consys_wmt",
	.fops = &consys_wmt_fops,
	.mode = 0600,
};

/* Register the live WMT client on /dev/consys_wmt. wmt_btif/wmt_dev
 * are already set by consys_gate_g2b (which keeps the BTIF mapping
 * alive instead of unmapping it). */
static int consys_wmt_client_init(struct device *dev, bool ready)
{
	int ret;

	wmt_ready = ready;
	ret = misc_register(&consys_wmt_misc);
	if (ret) {
		dev_err(dev, "consys-spike: /dev/consys_wmt register failed (%d)\n", ret);
		return ret;
	}
	/* RX consumer for the mtk_wcn core hand-off (idle until the
	 * core registers its RX callback). */
	wmt_rxd = kthread_run(consys_wmtrxd, NULL, "consys_wmtrxd");
	if (IS_ERR(wmt_rxd)) {
		dev_err(dev, "consys-spike: consys_wmtrxd kthread failed (%ld)\n",
			PTR_ERR(wmt_rxd));
		wmt_rxd = NULL;
	}
	dev_info(dev, "consys-spike: live WMT client on /dev/consys_wmt (link %s)\n",
		 ready ? "up" : "NOT up");
	return 0;
}

/* Stage W2: EMI remap + BTIF up + MCU release + first WMT exchange. */
static int consys_gate_g2b(struct device *dev)
{
	void __iomem *btif;
	struct clk *btif_clk;
	u8 rx[G2B_RX_BUF_SZ];
	int n, ret;

	dev_info(dev, "consys-spike: Gate G2b starting\n");

	ret = consys_emi_setup(dev);
	if (ret)
		return ret;

	btif_clk = devm_clk_get(dev, "btif");
	if (IS_ERR(btif_clk)) {
		dev_err(dev, "consys-spike: no btif clock (%ld)\n",
			PTR_ERR(btif_clk));
		return PTR_ERR(btif_clk);
	}
	ret = clk_prepare_enable(btif_clk);
	if (ret) {
		dev_err(dev, "consys-spike: btif clock enable failed (%d)\n",
			ret);
		return ret;
	}

	btif = ioremap(BTIF_PA, BTIF_SZ);
	if (!btif) {
		dev_err(dev, "consys-spike: BTIF ioremap failed\n");
		return -ENOMEM;
	}
	dev_info(dev, "consys-spike: BTIF init (LSR=%#x)\n",
		 readl(btif + BTIF_LSR));
	btif_hw_init(btif);
	dev_info(dev,
		 "consys-spike: BTIF regs post-init: IER=%#x IIR=%#x LSR=%#x DMA_EN=%#x TRI_LVL=%#x HANDSHAKE=%#x SLEEP=%#x\n",
		 readl(btif + BTIF_IER), readl(btif + BTIF_IIR),
		 readl(btif + BTIF_LSR), readl(btif + BTIF_DMA_EN),
		 readl(btif + BTIF_TRI_LVL), readl(btif + BTIF_HANDSHAKE),
		 readl(btif + BTIF_SLEEP_EN));

	/* Build B: switch the transport to the vendor's DMA mode
	 * (vFIFOs + APDMA). Non-fatal: on any failure the PIO path
	 * stays active (btif_dma_active = false) and the probe
	 * continues exactly as before. */
	if (btif_dma_init(dev, btif))
		dev_info(dev, "consys-spike: BTIF staying in PIO mode (DMA init failed)\n");

	/* Release the CONSYS MCU: clear swsysrst bit 12 (key 0x88<<24).
	 * Everything up to here ran with the MCU held in reset. */
	dev_info(dev, "consys-spike: releasing CONSYS MCU reset\n");
	ret = consys_poke_clear(AP_RGU_SWSYSRST_PA, AP_RGU_CONN_MCU_RST,
				AP_RGU_KEY, dev, "AP_RGU_SWSYSRST(release)");
	if (ret)
		goto out_unmap;

	/* B-23 experiment (2026-09-06): the hand-off reset (the MCU's
	 * THIRD boot of the power-on) leaves the MCU deaf while G2b's
	 * second boot works. Do a throwaway third boot here (release,
	 * let it settle, re-assert, re-release): if the first exchange
	 * after the FOURTH boot still works, boot count is not the
	 * hand-off problem and the warmed-power state is. */
	msleep(300);
	consys_poke(AP_RGU_SWSYSRST_PA, AP_RGU_CONN_MCU_RST, AP_RGU_KEY,
		  false, 0, dev, "AP_RGU_SWSYSRST(assert)");
	msleep(50);
	dev_info(dev, "consys-spike: B-23 throwaway boot done, re-releasing\n");
	consys_poke_clear(AP_RGU_SWSYSRST_PA, AP_RGU_CONN_MCU_RST,
			  AP_RGU_KEY, dev, "AP_RGU_SWSYSRST(release)");

	/* Let the ROM boot; log anything it sends unsolicited. */
	msleep(50);
	n = btif_rx_drain(btif, rx, sizeof(rx), 200, G2B_RX_IDLE_MS);
	dev_info(dev, "consys-spike: post-release RX %d bytes: %*ph\n",
		 n, n, rx);

	/* Vendor-first commands: the ROM is identified by three reg-reads
	 * (HCR/HVR/FVR) before any QUERY_STP (H35 trace; wmt_core_hw_check
	 * + mtk_wcn_soc_ver_check). Doubles as a live link check — expect
	 * chip id 0x279, hw/fw ver 0x8a00. Non-fatal: the flow continues
	 * to QUERY_STP either way so one boot yields both data points. */
	{
		u32 chip = 0, hvr = 0, fvr = 0;
		int r;

		dev_info(dev, "consys-spike: vendor-first reg reads (HCR/HVR/FVR)\n");
		r = wmt_reg_read(dev, btif, GEN_HCR_ADDR, GEN_VER_MASK, &chip);
		dev_info(dev,
			 "consys-spike: HCR 0x%08lx = 0x%08x (%s)\n",
			 (unsigned long)GEN_HCR_ADDR, chip,
			 r ? "FAIL" : (chip == CONSYS_CHIP_ID_EXPECT) ?
			 "chip id ok" : "UNEXPECTED");
		if (!r) {
			wmt_reg_read(dev, btif, GEN_HVR_ADDR, GEN_VER_MASK, &hvr);
			wmt_reg_read(dev, btif, GEN_FVR_ADDR, GEN_VER_MASK, &fvr);
			dev_info(dev,
				 "consys-spike: HVR=0x%08x FVR=0x%08x (golden 0x00008a00/0x00008a00)\n",
				 hvr, fvr);
		}
	}

	/* Build D: the mcuclk speed-up transition is a coin flip in PIO
	 * mode (2 of 5 boots pass; #295 b1/b3 pass, #295 b2 / #296 b1
	 * / #299 b1 fail at different clock commands), and a failed
	 * transition leaves the MCU's WMT RX deaf for the rest of the
	 * boot (MCU alive, BTIF AP side clean - verified live 15 min
	 * after #299). Retry: MCU reset cycle (ROM reboots, re-inits
	 * its BTIF side) + full re-init. P(pass in 5 cycles) ~ 92%. */
	{
		int cycle;

		for (cycle = 1; cycle <= 5; cycle++) {
			if (cycle > 1) {
				dev_info(dev, "consys-spike: CONSYS init cycle %d: MCU reset + full re-init\n",
					 cycle);
				/* G2b order: BTIF re-init with the MCU held in
			 * reset (same pattern as consys_mcu_power_cycle). */
			consys_mcu_reset_hold(dev);
				btif_hw_init(btif);
				/* Re-program the DMA channels (btif_hw_init
				 * re-cleared the BTIF-side DMA_EN bits):
				 * drops stale RX vFIFO content from the
				 * previous cycle. */
				if (txdma_regs)
					btif_dma_init(dev, btif);
				consys_mcu_reset_release(dev);
				msleep(50);
			}
			ret = wmt_init_and_push(dev, btif);
			if (ret == 0)
				break;
			dev_err(dev, "consys-spike: CONSYS init cycle %d failed (%d)\n",
				cycle, ret);
		}
	}

	/* Build A2: the patched WMT is not ready 12 ms after the 2nd
	 * WMT_RESET (final query timed out 3/3 boots on #295). Wait
	 * before the verification query. Build B: full-mode framed.
	 * Now non-fatal: the gate also accepts CPUPCR evidence. */
	msleep(200);

	{
		int qret;
		void __iomem *pcr = ioremap(CONSYS_CPUPCR_PA, 4);
		u32 pcr0 = pcr ? readl(pcr) : 0;

		qret = wmt_cmd_full(dev, btif, wmt_query_stp_cmd,
				    sizeof(wmt_query_stp_cmd),
				    wmt_query_stp_evt_hdr,
			    sizeof(wmt_query_stp_evt_hdr), false);
		if (pcr)
			dev_info(dev, "consys-spike: MCU CPUPCR after push: %#x (0x112c = bare ROM, 0x9b5d6-class = patched)\n",
				pcr0);
		if (pcr)
			iounmap(pcr);

		if (ret != 0) {
			dev_err(dev, "consys-spike: GATE G2B FAIL (push %d, query %d) - state left up for devmem inspection\n",
				ret, qret);
			goto out_unmap;
		}
		if (qret == 0) {
			dev_info(dev, "consys-spike: *** GATE G2B PASS - CONSYS MCU answers full-mode WMT with firmware pushed (query ok) ***\n");
			ret = 0;
			goto out_unmap;
		}
		if (pcr0 && pcr0 != 0x112c) {
			dev_info(dev, "consys-spike: *** GATE G2B PASS - firmware pushed, CPUPCR %#x shows patched ROM executing (final query deaf - #295 pattern) ***\n",
				pcr0);
			ret = 0;
			goto out_unmap;
		}
		dev_err(dev, "consys-spike: GATE G2B FAIL (push ok, query %d, CPUPCR %#x still bare ROM)\n",
			qret, pcr0);
		ret = -EIO;
		goto out_unmap;
	}
out_unmap:
	/* Build C: keep the BTIF mapping alive for the live WMT client
	 * (/dev/consys_wmt) instead of unmapping; mtk_consys_spike_remove
	 * unmaps it on unbind. */
	wmt_btif = btif;
	wmt_dev = dev;
	return ret;
}

static int mtk_consys_spike_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regulator *vcn18, *vcn28;
	void __iomem *mcu;
	u32 id = 0;
	int i, ret;

	dev_info(dev, "consys-spike: Gate G2a probe starting\n");

	/* Boot-path diagnostic before any power-on (docs/wifi-consys.md:
	 * golden 0x6d403a00 = preloader cold-boot state). */
	{
		void __iomem *infra = ioremap(INFRA_MISC_PA, 4);
		u32 v = infra ? readl(infra) : 0;

		dev_info(dev,
			 "consys-spike: INFRA_MISC 0x10001f00 = 0x%08x (golden 0x%08x, %s)\n",
			 v, INFRA_MISC_GOLDEN,
			 v == INFRA_MISC_GOLDEN ? "match" : "MISMATCH");
		if (infra)
			iounmap(infra);
	}

	/* Step 1: VCN18 on, 240us settle (vendor order: rails first) */
	vcn18 = devm_regulator_get(dev, "vcn18");
	if (IS_ERR(vcn18))
		return dev_err_probe(dev, PTR_ERR(vcn18), "no vcn18\n");
	vcn28 = devm_regulator_get(dev, "vcn28");
	if (IS_ERR(vcn28))
		return dev_err_probe(dev, PTR_ERR(vcn28), "no vcn28\n");

	/* B-30: the VCN33 sub-rails are fetched but NOT enabled here - the
	 * radio stays idle until the WMT core's PALDO ops (RF calibration
	 * exchange, later the real Wi-Fi/BT sessions) ask for them. */
	g_vcn33_bt = devm_regulator_get_optional(dev, "vcn33-bt");
	if (IS_ERR(g_vcn33_bt)) {
		dev_info(dev, "consys-spike: no vcn33-bt supply (%ld) - PALDO BT ops unavailable\n",
			 PTR_ERR(g_vcn33_bt));
		g_vcn33_bt = NULL;
	}
	g_vcn33_wifi = devm_regulator_get_optional(dev, "vcn33-wifi");
	if (IS_ERR(g_vcn33_wifi)) {
		dev_info(dev, "consys-spike: no vcn33-wifi supply (%ld) - PALDO WIFI ops unavailable\n",
			 PTR_ERR(g_vcn33_wifi));
		g_vcn33_wifi = NULL;
	}

	dev_info(dev, "consys-spike: enabling VCN18\n");
	ret = regulator_enable(vcn18);
	if (ret)
		return dev_err_probe(dev, ret, "VCN18 enable failed\n");
	udelay(240);

	/* Step 2: VCN28 on (co_clock_flag=0 path; ON_CTRL set by mt6351 drv) */
	dev_info(dev, "consys-spike: enabling VCN28\n");
	ret = regulator_enable(vcn28);
	if (ret) {
		dev_err(dev, "consys-spike: VCN28 enable failed (%d)\n", ret);
		goto err_vcn18;
	}

	/* Step 3: CONN2AP sleep mask */
	ret = consys_poke(CONN2AP_SLEEP_MASK_PA, CONN2AP_SLEEP_MASK_BIT, 0,
			  false, 0, dev, "CONN2AP_SLEEP_MASK");
	if (ret)
		goto err_vcn28;

	/* Step 4: AP_RGU — keep CONSYS MCU reset asserted across WDT (vendor
	 * mtk_wdt_swsysret_config(BIT(12), 1); write requires key 0x88<<24) */
	ret = consys_poke(AP_RGU_SWSYSRST_PA, AP_RGU_CONN_MCU_RST, AP_RGU_KEY,
			  false, 0, dev, "AP_RGU_SWSYSRST");
	if (ret)
		goto err_vcn28;

	/* Step 5: SPM PWRON_CONFG_EN project key */
	ret = consys_poke(SPM_PWRON_CONFG_EN_PA, 0, 0, true,
			  SPM_PWRON_CONFG_EN_VAL, dev, "SPM_PWRON_CONFG_EN");
	if (ret)
		goto err_vcn28;

	/* Step 6: CONN MTCMOS on via the scpsys power domain (DTS
	 * power-domains property; runtime PM powers the genpd) */
	dev_info(dev, "consys-spike: powering CONN domain (scpsys)\n");
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret) {
		dev_err(dev, "consys-spike: CONN domain power-on failed (%d)\n",
			ret);
		goto err_rpm;
	}

	/* Step 7: 26M settle, then poll chip ID */
	udelay(30);
	mcu = ioremap(CONSYS_CHIP_ID_PA & PAGE_MASK, PAGE_SIZE);
	if (!mcu) {
		ret = -ENOMEM;
		dev_err(dev, "consys-spike: ioremap chip-ID window failed\n");
		goto err_domain;
	}

	dev_info(dev, "consys-spike: polling chip ID at %#lx\n",
		 CONSYS_CHIP_ID_PA);
	for (i = 0; i < CHIP_ID_RETRIES; i++) {
		id = readl(mcu + (CONSYS_CHIP_ID_PA & ~PAGE_MASK));
		dev_info(dev, "consys-spike: chip-ID read %d: %#x\n", i, id);
		if (id == CONSYS_CHIP_ID_EXPECT)
			break;
		msleep(20);
	}

	if (id != CONSYS_CHIP_ID_EXPECT) {
		dev_err(dev,
			"consys-spike: GATE G2A FAIL - chip ID %#x (want %#x)\n",
			id, CONSYS_CHIP_ID_EXPECT);
		ret = -ENODEV;
		iounmap(mcu);
		goto err_domain;
	}

	dev_info(dev, "consys-spike: *** GATE G2A PASS - CONSYS chip ID %#x ***\n",
		 id);

	/* Step 8: MBIST real-speed bit, as vendor does post-ID */
	writel(readl(mcu + (CONSYS_MCU_ACR_PA & ~PAGE_MASK)) |
	       CONSYS_MCU_ACR_MBIST,
	       mcu + (CONSYS_MCU_ACR_PA & ~PAGE_MASK));
	iounmap(mcu);

	/* Stage W2: MCU release + first WMT exchange over BTIF. A G2b
	 * failure is reported in dmesg but does NOT fail the probe -
	 * CONSYS is left powered so the state is devmem-inspectable. */
	ret = consys_gate_g2b(dev);

	/* Build C: expose the live WMT link to userspace (/dev/consys_wmt)
	 * so the WMT-firmware phase - and later the host driver - can drive
	 * it after boot. 'ready' reflects whether G2b passed. */
	consys_wmt_client_init(dev, ret == 0);

	/* Leave the domain and rails ON so the state can be inspected over
	 * SSH (devmem) after boot. Teardown happens on driver unbind. */
	return 0;

err_domain:
	pm_runtime_put_sync(dev);
err_rpm:
	pm_runtime_disable(dev);
err_vcn28:
	regulator_disable(vcn28);
err_vcn18:
	regulator_disable(vcn18);
	dev_err(dev, "consys-spike: probe failed (%d) - sequence rolled back\n",
		ret);
	return ret;
}

static int mtk_consys_spike_remove(struct platform_device *pdev)
{
	if (wmt_rxd) {
		kthread_stop(wmt_rxd);
		wmt_rxd = NULL;
	}
	misc_deregister(&consys_wmt_misc);
	if (wmt_btif) {
		iounmap(wmt_btif);
		wmt_btif = NULL;
	}
	wmt_ready = false;
	return 0;
}

static const struct of_device_id mtk_consys_spike_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_consys_spike_of_match);

static struct platform_driver mtk_consys_spike_driver = {
	.driver = {
		.name = "mtk-consys-spike",
		.of_match_table = mtk_consys_spike_of_match,
	},
	.probe = mtk_consys_spike_probe,
	.remove = mtk_consys_spike_remove,
};
module_platform_driver(mtk_consys_spike_driver);

MODULE_DESCRIPTION("MT6797 CONSYS power-on + WMT firmware-push feasibility spike (Gates G2a/G2b)");
MODULE_LICENSE("GPL");
